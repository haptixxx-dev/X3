#pragma once

// =============================================================================
// VulkanContext -- the device, the swapchain, and THE frame.
//
// This header has converged on VulkanContextInterface.h for everything Phase 1
// Part 2 owns: the beginFrame()/endFrame()/present() split, the dynamic
// rendering block, frame identity, the deferred-destruction queue, the per-frame
// staging arena, the sampler cache, the cached device limits and the dummy
// resources. What has NOT converged, deliberately, is called out inline:
//
//   * blitImageToSwapchain() keeps its (VkImage, VkImageLayout, w, h, ...) form.
//     MERGED-2 §6.5 rules on this: the adjudicated VulkanImage& form cannot be
//     written until Renderer stops using VulkanImage2D, which is Part 3's
//     call-site migration. Only the BODY changes here.
//   * The four bound-resource registries survive. VulkanComputeShader still
//     reads them; Part 3 deletes both together.
//   * getCurrentCommandBuffer() survives for the same reason -- the compute
//     shader and ImGui have no FrameContext to take a cmd() from until Part 3
//     threads one down.
//
// THE FRAME-SLOT INVARIANT, which no tool can check and which every per-frame
// ring in the engine depends on (MERGED-2 §2.3):
//
//     Every CPU write to a resource slot indexed by frame.index() happens after
//     beginFrame() has waited m_InFlightFences[m_CurrentFrame] and before
//     endFrame() submits.
//
// After the lifecycle split this holds structurally: Application::run calls
// beginFrame() before LayerStack::onUpdate() and endFrame() after it, in the
// same iteration. Synchronization validation does not model host writes to
// persistently-mapped memory, so re-check this BY HAND whenever
// Application::run is touched.
//
// THE FENCE INVARIANT: m_InFlightFences[i] is signalled if and only if there is
// no pending submit for slot i. vkResetFences therefore lives immediately before
// vkQueueSubmit with nothing between them that can fail.
// =============================================================================

#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanImage.h"
#include "Platform/Vulkan/VulkanStaging.h"
#include "Platform/Vulkan/VulkanTypes.h"

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace X3
{

class VulkanContext {
public:
	// The colour the one rendering block per frame clears to. The old
	// VkRenderPass cleared to opaque black (VulkanContext.cpp:388 pre-migration)
	// and so does this; nothing in application::run supplied a colour after the
	// OpenGL deletion removed IRendererAPI::Clear.
	static constexpr VkClearColorValue kDefaultClearColor{{0.0f, 0.0f, 0.0f, 1.0f}};

	// `vsync` is passed in rather than applied afterwards so the FIRST swapchain
	// already has the right present mode and no immediate recreation happens.
	explicit VulkanContext(GLFWwindow* window, bool vsync = false);
	~VulkanContext();

	void init();

	static void setWindowHints();

	// Singleton access to the current Vulkan context
	static VulkanContext* Get() { return s_Instance; }

	// =========================================================================
	// 1. FRAME LIFECYCLE
	// =========================================================================

	// Waits m_InFlightFences[m_CurrentFrame]; publishes m_CompletedFrame; drains
	// the deletion queue; resets frame m_CurrentFrame's staging arena; acquires a
	// swapchain image; resets and begins m_CommandBuffers[m_CurrentFrame].
	//
	// OPENS NO RENDERING BLOCK. That is the whole point of the dynamic-rendering
	// migration: compute is recorded at top level, so vkCmdDispatch is legal
	// again (VUID-vkCmdDispatch-renderpass / -None-10672).
	//
	// Returns nullptr if the swapchain was out of date -- it has already been
	// recreated and the caller must skip the entire frame, including endFrame()
	// and present(). There is no retry loop.
	//
	// The synchronisation guarantee attached to a non-null return: the fence
	// waited above was signalled by the submit of the previous use of this slot,
	// so every command that referenced slot index() has provably completed.
	const FrameContext* beginFrame();

	// Guarantees the acquired swapchain image ends in PRESENT_SRC_KHR, ends the
	// command buffer, resets the frame fence and submits. Clears m_FrameActive.
	void endFrame();

	// vkQueuePresentKHR; advances m_CurrentFrame and m_FrameNumber together, at
	// the very end; on OUT_OF_DATE/SUBOPTIMAL calls recreateSwapchain().
	void present();

	// Non-null only between beginFrame() and endFrame(). For the three callers
	// whose onUpdate() takes no parameter (ImGuiContext::EndFrame,
	// RuntimeLayer::onUpdate, RenderLayer::onUpdate). Not a general-purpose
	// accessor.
	const FrameContext* currentFrame() const { return m_FrameActive ? &m_Frame : nullptr; }

	// Monotonic frame counter since init. Incremented in present(), never in
	// beginFrame(). Equals frame.number() for the duration of a frame.
	uint64_t frameNumber() const { return m_FrameNumber; }

	// Highest frame number whose submission is known complete. 0 until
	// FRAMES_IN_FLIGHT frames have run.
	uint64_t completedFrame() const { return m_CompletedFrame; }

	// True between beginFrame() and endFrame().
	bool frameActive() const { return m_FrameActive; }

	// True between beginSwapchainRendering() and endSwapchainRendering(). The
	// standing guard against VUID-vkCmdDispatch-renderpass coming back once
	// something starts drawing geometry again.
	bool renderingBlockOpen() const { return m_RenderingBlockOpen; }

	// =========================================================================
	// 2. DYNAMIC RENDERING
	// =========================================================================

	// Opens a single-colour-attachment rendering block on the acquired swapchain
	// image view. Records the UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL barrier
	// itself; there is no render pass to do it implicitly. Must be balanced by
	// endSwapchainRendering().
	void beginSwapchainRendering(VkClearColorValue clear = kDefaultClearColor);
	void endSwapchainRendering();

	// =========================================================================
	// 3. PER-FRAME STAGING -- THE upload mechanism for in-frame uploads
	// =========================================================================

	// Carves `size` bytes out of frame.index()'s staging arena. Never waits,
	// never submits: the caller memcpys into alloc.ptr and records
	// vkCmdCopyBuffer / vkCmdCopyBufferToImage into frame.cmd(), and the frame
	// fence retires it. Throws through X3_VK_CHECK on failure.
	StagingAlloc stage(const FrameContext& frame, VkDeviceSize size,
	                   VkDeviceSize alignment = VulkanStagingArena::kDefaultAlignment);

	// =========================================================================
	// 4. DEFERRED DESTRUCTION
	//
	// All three stamp the entry with retireFrame = m_FrameNumber and destroy it
	// only once retireFrame <= m_CompletedFrame, i.e. after FRAMES_IN_FLIGHT
	// frames retire. drainDeletionQueue() runs at the top of beginFrame(),
	// immediately after the fence wait, BEFORE the acquire -- so an OUT_OF_DATE
	// early return (every frame of a click-and-drag resize) cannot starve it.
	// =========================================================================

	void deferDestroy(VkBuffer buffer, VmaAllocation allocation);
	void deferDestroy(VkImage image, VmaAllocation allocation, VkImageView view);
	void deferFreeDescriptorSets(std::span<const VkDescriptorSet> sets);

	// =========================================================================
	// 5. SHARED RESOURCES
	// =========================================================================

	// Cached, context-owned sampler. Samplers are never created per texture:
	// maxSamplerAllocationCount is as low as 4000 on some drivers. Borrowers must
	// never destroy one; the cache is emptied in cleanup(), after everything that
	// borrowed from it.
	VkSampler getSampler(const SamplerDesc& desc);

	// The always-write rule's escape hatch: an absent skybox or an empty light
	// list still has to write something, and these are that something. Created
	// during init() through the one surviving blocking helper.
	const VulkanTexture& dummyTexture()       const { return *m_DummyTexture; }
	const VulkanBuffer&  dummyStorageBuffer() const { return *m_DummyStorageBuffer; }
	const VulkanBuffer&  dummyUniformBuffer() const { return *m_DummyUniformBuffer; }

	// =========================================================================
	// 6. CACHED DEVICE LIMITS
	// Cached in pickPhysicalDevice, which already called
	// vkGetPhysicalDeviceProperties and threw the result away. A per-call
	// vkGetPhysicalDeviceProperties in an allocation path is a syscall-shaped
	// mistake.
	// =========================================================================
	const VkPhysicalDeviceLimits& limits() const { return m_Limits; }

	// =========================================================================
	// 7. PRESENTATION
	// =========================================================================

	// NOT YET the adjudicated FrameContext/VulkanImage& form -- see the note at
	// the top of this file and MERGED-2 §6.5. Opens no rendering block:
	// vkCmdBlitImage and vkCmdClearColorImage are transfer commands and must be
	// recorded outside one, which under dynamic rendering they now are by
	// construction. THE Y-FLIP PACKING IS CARRIED OVER BYTE FOR BYTE; it is a
	// matched pair with RuntimeLayer::CalculateViewportCoordinates().
	void blitImageToSwapchain(VkImage sourceImage, VkImageLayout currentLayout,
	                          uint32_t srcWidth, uint32_t srcHeight,
	                          glm::ivec4 viewport, glm::ivec2 windowSize);

	// =========================================================================
	// 8. THE ONE SURVIVING BLOCKING HELPER
	//
	// ADJUDICATION.md's CORRECTED wait-idle ruling: in-frame uploads never wait
	// (stage() + frame.cmd()); out-of-frame uploads MAY wait and are confined to
	// initialisation and teardown -- ImGui fonts, the dummy resources, and
	// nothing else. Permitted wait-idle set after Phase 1: endSingleTimeCommands,
	// recreateSwapchain, cleanup, drainDeletionQueueFully.
	// =========================================================================
	VkCommandBuffer beginSingleTimeCommands();
	void endSingleTimeCommands(VkCommandBuffer cmd);

	// =========================================================================
	// 9. RAW HANDLES
	// =========================================================================
	VkInstance       getInstance()           const { return m_Instance; }
	VkPhysicalDevice getPhysicalDevice()     const { return m_PhysicalDevice; }
	VkDevice         getDevice()             const { return m_Device; }
	VkQueue          getGraphicsQueue()      const { return m_GraphicsQueue; }
	VkSurfaceKHR     getSurface()            const { return m_Surface; }
	VkCommandPool    getCommandPool()        const { return m_CommandPool; }
	VmaAllocator     getAllocator()          const { return m_Allocator; }
	VkDescriptorPool getDescriptorPool()     const { return m_DescriptorPool; }
	uint32_t         getGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }
	VkExtent2D       getSwapchainExtent()    const { return m_SwapchainExtent; }
	uint32_t         getSwapchainImageCount() const { return static_cast<uint32_t>(m_SwapchainImages.size()); }

	// For VkPipelineRenderingCreateInfo (ImGui, and every future pipeline).
	VkFormat getSwapchainImageFormat() const { return m_SwapchainImageFormat; }

	// Legacy accessor: the compute shader and ImGui record into the frame command
	// buffer without holding a FrameContext. Part 3 replaces both call sites with
	// frame.cmd().
	VkCommandBuffer getCurrentCommandBuffer() const { return m_CommandBuffers[m_CurrentFrame]; }

	void recreateSwapchain();

	// Recreates the swapchain with the matching present mode. Safe to call from
	// inside a frame -- an ImGui checkbox dispatches SetVSyncEvent while the frame
	// command buffer is recording -- in which case the recreation is deferred to
	// the top of the next beginFrame().
	void setVSync(bool enabled);
	bool getVSync() const { return m_VSync; }

	// Swapchain recreation notification (for ImGui to update its resources)
	bool wasSwapchainRecreated() const { return m_SwapchainRecreated; }
	void clearSwapchainRecreatedFlag() { m_SwapchainRecreated = false; }

	// =========================================================================
	// 10. Bound-resource registry -- SURVIVES ONLY UNTIL PART 3.
	// A resource should reach a shader through a DescriptorWriter at a call site
	// that knows its set, not through a global registry. VulkanComputeShader is
	// the only consumer and it is deleted with these.
	// =========================================================================
	struct BoundStorageBuffer { VkBuffer buffer; uint32_t size; };
	struct BoundUniformBuffer { VkBuffer buffer; uint32_t size; };
	struct BoundStorageImage { VkImageView imageView; };
	struct BoundSampledImage { VkImageView imageView; VkSampler sampler; };

	void registerStorageBuffer(uint32_t binding, VkBuffer buffer, uint32_t size);
	void registerUniformBuffer(uint32_t binding, VkBuffer buffer, uint32_t size);
	void registerStorageImage(uint32_t unit, VkImageView imageView);
	void registerSampledImage(uint32_t unit, VkImageView imageView, VkSampler sampler);

	const std::unordered_map<uint32_t, BoundStorageBuffer>& getBoundStorageBuffers() const { return m_BoundStorageBuffers; }
	const std::unordered_map<uint32_t, BoundUniformBuffer>& getBoundUniformBuffers() const { return m_BoundUniformBuffers; }
	const std::unordered_map<uint32_t, BoundStorageImage>& getBoundStorageImages() const { return m_BoundStorageImages; }
	const std::unordered_map<uint32_t, BoundSampledImage>& getBoundSampledImages() const { return m_BoundSampledImages; }

private:
	void createInstance();
	void pickPhysicalDevice();
	void createLogicalDevice();
	void createSurface();
	void createSwapchain(VkSwapchainKHR oldSwapchain);
	void createAllocator();
	void createCommandPool();
	void createCommandBuffers();
	void createSyncObjects();
	void createDescriptorPool();
	void createDummyResources();
	void cleanup();
	void cleanupSwapchain();

	// Semaphores are NEVER destroyed at swapchain recreation: vkDeviceWaitIdle
	// does not retire an outstanding vkQueuePresentKHR wait
	// (VUID-vkDestroySemaphore-semaphore-01137). m_ImageAvailableSemaphores does
	// not depend on the image count at all; m_RenderFinishedSemaphores only grows.
	void ensureRenderFinishedSemaphores();

	// Every swapchain-image layout transition goes through here. Without render
	// passes there are no initialLayout/finalLayout and no implicit EXTERNAL
	// subpass dependencies, so every transition is an explicit barrier.
	//
	// THE RULE: srcStageMask must be a stage included in the submit's
	// pWaitDstStageMask. VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT as a SOURCE stage
	// orders nothing, so a transition specified with it may legally be scheduled
	// before the acquire semaphore's wait retires.
	void transitionSwapchainImage(VkCommandBuffer cmd,
	                              VkImageLayout oldLayout, VkImageLayout newLayout,
	                              VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
	                              VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

	void drainDeletionQueue();       // entries with retireFrame <= m_CompletedFrame
	void drainDeletionQueueFully();  // vkDeviceWaitIdle + destroy everything

private:
	GLFWwindow* m_NativeWindow = nullptr;

	VkInstance m_Instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
	VkDevice m_Device = VK_NULL_HANDLE;
	VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
	VkQueue m_PresentQueue = VK_NULL_HANDLE;
	VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
	VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;

	VmaAllocator m_Allocator = VK_NULL_HANDLE;
	VkPhysicalDeviceLimits m_Limits{};

	// Swapchain resources. There is no m_Framebuffers: dynamic rendering consumes
	// m_SwapchainImageViews[m_ImageIndex] directly.
	std::vector<VkImage> m_SwapchainImages;
	std::vector<VkImageView> m_SwapchainImageViews;
	VkFormat m_SwapchainImageFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D m_SwapchainExtent{};

	// Layout STATE, not pass state. Both are reset at the top of every successful
	// beginFrame(): acquired swapchain contents are undefined, which is what lets
	// the frame's first barrier discard rather than preserve.
	VkImageLayout m_SwapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bool m_SwapchainImageWritten = false;
	bool m_RenderingBlockOpen = false;

	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;
	uint32_t m_GraphicsQueueFamily = 0;

	VkCommandPool m_CommandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_CommandBuffers;

	// Sized FRAMES_IN_FLIGHT, indexed by m_CurrentFrame. Safe because beginFrame
	// waited m_InFlightFences[m_CurrentFrame], which proves the submit that waited
	// on this semaphore completed.
	std::vector<VkFence> m_InFlightFences;
	std::vector<VkSemaphore> m_ImageAvailableSemaphores;

	// Sized >= swapchain image count, indexed by m_ImageIndex at BOTH the submit
	// signal and the present wait. A present's semaphore wait is retired by the
	// presentation engine, and the only signal the application gets that this
	// happened is that vkAcquireNextImageKHR handed the corresponding IMAGE back
	// -- so reuse must be gated on the image index, never on a free-running
	// counter. (vkAcquireNextImageKHR is not required to round-robin, and MAILBOX
	// routinely does not.) This is the canonical pattern; it is an inference from
	// acquire semantics rather than an explicit spec guarantee. The airtight tool
	// is VK_EXT_swapchain_maintenance1's VkSwapchainPresentFenceInfoEXT, which is
	// not on MoltenVK. Do not depend on it.
	std::vector<VkSemaphore> m_RenderFinishedSemaphores;

	uint32_t m_CurrentFrame = 0;
	uint32_t m_ImageIndex = 0;

	FrameContext m_Frame;
	bool     m_FrameActive    = false;
	uint64_t m_FrameNumber    = 0;   // number of the frame about to be recorded
	uint64_t m_CompletedFrame = 0;   // highest frame number known complete on the GPU

	// Deferred destruction
	struct PendingDelete {
		uint64_t                     retireFrame = 0;
		VkBuffer                     buffer      = VK_NULL_HANDLE;
		VkImage                      image       = VK_NULL_HANDLE;
		VkImageView                  view        = VK_NULL_HANDLE;
		VmaAllocation                allocation  = VK_NULL_HANDLE;
		std::vector<VkDescriptorSet> sets;
	};
	std::vector<PendingDelete> m_DeletionQueue;

	VulkanStagingArena m_Staging;
	std::unordered_map<SamplerDesc, VkSampler> m_Samplers;

	std::unique_ptr<VulkanTexture> m_DummyTexture;
	std::unique_ptr<VulkanBuffer>  m_DummyStorageBuffer;
	std::unique_ptr<VulkanBuffer>  m_DummyUniformBuffer;

	VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

	vkb::Instance m_VkbInstance;
	vkb::PhysicalDevice m_VkbPhysicalDevice;
	vkb::Device m_VkbDevice;
	vkb::Swapchain m_VkbSwapchain;

	bool m_EnableValidationLayers = true; // Disable in release builds
	bool m_SwapchainRecreated = false;    // Flag for ImGui to detect swapchain recreation
	bool m_VSync = false;
	bool m_PresentModeDirty = false;      // setVSync() called mid-frame
	bool m_WarnedInFrameBlockingUpload = false;

	static inline VulkanContext* s_Instance = nullptr;

	// Bound-resource registries (deleted with the accessors above, in Part 3)
	std::unordered_map<uint32_t, BoundStorageBuffer> m_BoundStorageBuffers;
	std::unordered_map<uint32_t, BoundUniformBuffer> m_BoundUniformBuffers;
	std::unordered_map<uint32_t, BoundStorageImage> m_BoundStorageImages;
	std::unordered_map<uint32_t, BoundSampledImage> m_BoundSampledImages;
};

}
