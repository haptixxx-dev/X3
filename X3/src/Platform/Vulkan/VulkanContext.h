#pragma once

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include "Renderer/IRenderingContext.h"

namespace X3
{

class VulkanContext : public IRenderingContext {
public:
	VulkanContext(GLFWwindow* window);
	~VulkanContext();

	void init() override;
	void swapBuffers() override;

	static void setWindowHints();

	// Vulkan-specific getters
	VkInstance getInstance() const { return m_Instance; }
	VkPhysicalDevice getPhysicalDevice() const { return m_PhysicalDevice; }
	VkDevice getDevice() const { return m_Device; }
	VkQueue getGraphicsQueue() const { return m_GraphicsQueue; }
	VkSurfaceKHR getSurface() const { return m_Surface; }
	VkCommandPool getCommandPool() const { return m_CommandPool; }
	VkRenderPass getRenderPass() const { return m_RenderPass; }
	VkRenderPass getOverlayRenderPass() const { return m_OverlayRenderPass; }
	VmaAllocator getAllocator() const { return m_Allocator; }
	VkDescriptorPool getDescriptorPool() const { return m_DescriptorPool; }
	uint32_t getGraphicsQueueFamily() const { return m_GraphicsQueueFamily; }

	// Resource binding registry for descriptor sets
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

	// Singleton access to the current Vulkan context
	static VulkanContext* Get() { return s_Instance; }

	// Frame management
	uint32_t getCurrentFrame() const { return m_CurrentFrame; }
	VkCommandBuffer getCurrentCommandBuffer() const { return m_CommandBuffers[m_CurrentFrame]; }

	// Render loop functions
	bool beginFrame();
	void endFrame();
	void ensureFrameStarted(); // Call this before rendering if frame might not be started

	// Single-time command buffer for resource initialization (use outside of frame rendering)
	VkCommandBuffer beginSingleTimeCommands();
	void endSingleTimeCommands(VkCommandBuffer cmd);

	// Render pass management
	bool isRenderPassActive() const { return m_RenderPassActive; }
	void beginRenderPass(); // Restart render pass if ended (e.g., after blitImageToSwapchain)
	void beginOverlayRenderPass(); // End current render pass (if any) and start overlay render pass

	// Swapchain management
	void recreateSwapchain();

	// Compute image presentation (for RuntimeLayer)
	// Ends the render pass, blits the source image to swapchain, and prepares for present
	void blitImageToSwapchain(VkImage sourceImage, VkImageLayout currentLayout,
	                          uint32_t srcWidth, uint32_t srcHeight,
	                          glm::ivec4 viewport, glm::ivec2 windowSize);

	// Get swapchain extent for viewport calculations
	VkExtent2D getSwapchainExtent() const { return m_SwapchainExtent; }
	uint32_t getSwapchainImageCount() const { return static_cast<uint32_t>(m_SwapchainImages.size()); }
	uint32_t getMinImageCount() const { return 2; } // Minimum for double buffering
	uint32_t getMaxFramesInFlight() const { return MAX_FRAMES_IN_FLIGHT; }

	// Swapchain recreation notification (for ImGui to update its resources)
	bool wasSwapchainRecreated() const { return m_SwapchainRecreated; }
	void clearSwapchainRecreatedFlag() { m_SwapchainRecreated = false; }

private:
	void createInstance();
	void pickPhysicalDevice();
	void createLogicalDevice();
	void createSurface();
	void createSwapchain();
	void createAllocator();
	void createRenderPass();
	void createFramebuffers();
	void createCommandPool();
	void createCommandBuffers();
	void createSyncObjects();
	void createDescriptorPool();
	void cleanup();
	void cleanupSwapchain();

private:
	GLFWwindow* m_NativeWindow = nullptr;

	VkInstance m_Instance = VK_NULL_HANDLE;
	VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
	VkDevice m_Device = VK_NULL_HANDLE;
	VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
	VkQueue m_PresentQueue = VK_NULL_HANDLE;
	VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
	VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;

	// VMA allocator
	VmaAllocator m_Allocator = VK_NULL_HANDLE;

	// Swapchain resources
	std::vector<VkImage> m_SwapchainImages;
	std::vector<VkImageView> m_SwapchainImageViews;
	std::vector<VkFramebuffer> m_Framebuffers;
	VkFormat m_SwapchainImageFormat;
	VkExtent2D m_SwapchainExtent;

	// Render passes
	VkRenderPass m_RenderPass = VK_NULL_HANDLE;
	VkRenderPass m_OverlayRenderPass = VK_NULL_HANDLE; // For ImGui after blit (preserves content)

	// Descriptor pool for ImGui and other resources
	VkDescriptorPool m_DescriptorPool = VK_NULL_HANDLE;

	// Queue family indices
	uint32_t m_GraphicsQueueFamily = 0;

	// Command buffers
	VkCommandPool m_CommandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> m_CommandBuffers;

	// Synchronization objects (double buffering)
	static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
	// Per-frame resources (indexed by current frame)
	std::vector<VkFence> m_InFlightFences;
	// Per-swapchain-image resources (indexed by swapchain image count)
	// We need one semaphore pair per swapchain image to avoid reuse issues
	std::vector<VkSemaphore> m_ImageAvailableSemaphores;
	std::vector<VkSemaphore> m_RenderFinishedSemaphores;
	uint32_t m_CurrentFrame = 0;
	uint32_t m_CurrentSemaphoreIndex = 0; // Cycles through swapchain images
	uint32_t m_ImageIndex = 0;

	// Debug messenger for validation layers
	VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

	// Store vk-bootstrap wrapper objects for proper API usage
	vkb::Instance m_VkbInstance;
	vkb::PhysicalDevice m_VkbPhysicalDevice;
	vkb::Device m_VkbDevice;
	vkb::Swapchain m_VkbSwapchain;

	bool m_EnableValidationLayers = true; // Disable in release builds
	bool m_RenderPassActive = false; // Track if render pass is currently active
	bool m_FirstFrame = true; // Track if this is the first frame (for deferred beginFrame)
	bool m_SwapchainRecreated = false; // Flag for ImGui to detect swapchain recreation

	// Singleton instance for resource access
	static inline VulkanContext* s_Instance = nullptr;

	// Resource binding registries
	std::unordered_map<uint32_t, BoundStorageBuffer> m_BoundStorageBuffers;
	std::unordered_map<uint32_t, BoundUniformBuffer> m_BoundUniformBuffers;
	std::unordered_map<uint32_t, BoundStorageImage> m_BoundStorageImages;
	std::unordered_map<uint32_t, BoundSampledImage> m_BoundSampledImages;
};

}
