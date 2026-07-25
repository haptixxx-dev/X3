#pragma once

// =============================================================================
// VulkanContextInterface.h -- THE TARGET CONTRACT for VulkanContext.
//
// WHAT THIS FILE IS.
// Every header in the Vulkan-native resource layer -- VulkanTypes.h,
// VulkanBuffer.h, VulkanImage.h, VulkanDescriptors.h, VulkanStaging.h,
// VulkanComputePipeline.h -- forward-declares `class VulkanContext;` and then
// documents calls to members of it. Until this file existed, NOT ONE of those
// members was declared anywhere in the tree: not `stage()`, not either
// `deferDestroy()` overload, not `deferFreeDescriptorSets()`, not `getSampler()`,
// not `limits()`, not `dummyTexture()` / `dummyStorageBuffer()` /
// `dummyUniformBuffer()`, not `currentFrame()`, not `frameNumber()` /
// `completedFrame()`, not the `const FrameContext*` return of `beginFrame()`,
// and not the adjudicated `blitImageToSwapchain()` signature. The layer compiled
// only because a forward declaration plus a comment costs nothing to compile.
// That is exactly the orchestration failure ADJUDICATION.md opens with -- four
// parts written in parallel against a shared API surface that did not exist --
// reproduced at the level of a single class. This file is the surface, written
// down once, before anyone implements against it.
//
// WHY IT IS NOT AN EDIT TO VulkanContext.h.
// VulkanContext.h describes TODAY's implementation: swapBuffers(),
// ensureFrameStarted(), getCurrentFrame() returning a bare uint32_t,
// MAX_FRAMES_IN_FLIGHT, the four global bound-resource registries, the
// VkImage/VkImageLayout/width/height form of blitImageToSwapchain, and a base
// class (IRenderingContext) that Part 1 deletes. All of it still compiles and
// still runs the engine. Phase 1 REWRITES that header; it does not accrete onto
// it. Editing it now would produce a class that is neither the current one nor
// the target one -- declarations with no definitions, a link error for every TU,
// and no way to bisect. So the target lives here, the current lives there, and
// VulkanContext.h CONVERGES ON THIS FILE over Phase 1:
//
//   Part 1 (OpenGL deletion)     drops `: public IRenderingContext`, swapBuffers().
//   Part 2 (lifecycle/sync)      adds beginFrame()/endFrame()/present(),
//                                currentFrame(), the dynamic-rendering block,
//                                deletes ensureFrameStarted()/m_FirstFrame/
//                                getCurrentFrame()/getMinImageCount().
//   Part 3 (resource layer)      adds stage(), the deferred-destruction queue,
//                                getSampler(), limits(), the dummy resources,
//                                frameNumber()/completedFrame(); deletes
//                                MAX_FRAMES_IN_FLIGHT, getMaxFramesInFlight()
//                                and the bound-resource registries.
//   Part 3 (call-site migration) replaces blitImageToSwapchain's signature with
//                                the adjudicated FrameContext form.
//
// When the last of those lands, VulkanContext.h's public section is the block
// below and THIS FILE IS DELETED. It is scaffolding with a defined end, not a
// permanent second header. Until then: if the two disagree, this file is right
// and VulkanContext.h has not caught up yet.
//
// HOW TO USE IT.
// The declarations below are inside `namespace X3::contract`, on a class named
// VulkanContextPhase1, for one reason: `X3::VulkanContext` already exists with a
// different body, and a second declaration of it would be a redefinition error
// in any TU that sees both. The names, types, argument orders, const-ness and
// default arguments below are the contract; the enclosing class name is not.
// Transcribe the members into VulkanContext's public section verbatim, changing
// nothing but the class they sit in.
//
// This file declares no storage, defines no function, and is included by no
// other header in the layer. It is compiled (it is part of the all-headers
// verification TU) so that the contract is syntax-checked rather than prose --
// a signature that does not parse is caught here rather than in Phase 1.
//
// A NOTE ON FRIENDSHIP. FrameContext's members are private and its only friend
// is `class VulkanContext` (VulkanTypes.h). The mirror class below therefore
// could not fill in a FrameContext even if it had a body -- which is correct and
// intended: the real VulkanContext is the only thing that may mint one, and
// FrameContext is non-copyable and non-movable so nothing else can even hold one
// by value.
// =============================================================================

#include "Platform/Vulkan/VulkanStaging.h"   // StagingAlloc, VulkanStagingArena
#include "Platform/Vulkan/VulkanTypes.h"     // FrameContext, SamplerDesc, FRAMES_IN_FLIGHT

#include <cstdint>
#include <span>

namespace X3
{

// The real class. Declared here only so the contract can be talked about; its
// definition stays in VulkanContext.h.
class VulkanContext;

// Returned by const& from the dummy-resource accessors. Incomplete types are
// sufficient for every declaration below, and keeping them incomplete is what
// stops this file from pulling VulkanBuffer.h / VulkanImage.h into everything
// that wants to read the contract.
class VulkanBuffer;
class VulkanImage;
class VulkanTexture;

namespace contract
{

// -----------------------------------------------------------------------------
// THE POST-PHASE-1 VulkanContext PUBLIC SURFACE, exactly as the resource layer
// calls it. Not instantiable, not implemented, not inherited from -- a
// declaration block that happens to be syntax-checked.
//
// Each member is annotated with the header(s) that depend on it, so that a
// proposal to change one can be checked against its callers without a grep.
// -----------------------------------------------------------------------------
class VulkanContextPhase1
{
	VulkanContextPhase1() = delete;   // documentation, never an object

public:
	// =========================================================================
	// 1. FRAME LIFECYCLE
	//    Depended on by: VulkanTypes.h (the whole FrameContext synchronisation
	//    argument), VulkanBuffer.h, VulkanImage.h, VulkanDescriptors.h,
	//    VulkanStaging.h, VulkanComputePipeline.h -- i.e. all of them.
	// =========================================================================

	// Waits m_InFlightFences[m_CurrentFrame]; publishes m_CompletedFrame; drains
	// the deletion queue; resets frame m_CurrentFrame's staging arena; acquires a
	// swapchain image; resets and begins m_CommandBuffers[m_CurrentFrame]. Opens
	// NO rendering block (MERGED-0: that is what makes top-level vkCmdDispatch
	// legal again).
	//
	// Returns nullptr if the swapchain was out of date -- it has already been
	// recreated and the caller must skip the entire frame, including endFrame()
	// and present(). There is no retry loop.
	//
	// THE RETURN TYPE IS `const FrameContext*` AND THAT IS LOAD-BEARING. It is a
	// pointer to the context's single FrameContext member, not a value: a value
	// return is impossible anyway now that FrameContext's copy and move
	// operations are deleted, and that deletion is the mechanism that makes
	// "never cache a frame context" a compile error rather than a comment. The
	// caller holds the pointer for exactly one iteration of Application::run.
	//
	// The synchronisation guarantee attached to a non-null return: the fence
	// waited above was signalled by the submit of the previous use of this slot,
	// so every command that referenced slot index() has provably completed. That
	// single sentence is the justification for every per-frame ring in the
	// engine.
	const FrameContext* beginFrame();

	// Guarantees the acquired swapchain image ends in PRESENT_SRC_KHR, ends the
	// command buffer, resets the frame fence and submits. Precondition: a
	// successful beginFrame(). Clears m_FrameActive.
	void endFrame();

	// vkQueuePresentKHR; advances m_CurrentFrame and m_FrameNumber TOGETHER, at
	// the very end; on OUT_OF_DATE/SUBOPTIMAL calls recreateSwapchain().
	void present();

	// Non-null only between beginFrame() and endFrame().
	//
	// This exists for exactly three callers -- ImGuiContext::EndFrame,
	// RuntimeLayer::onUpdate, RenderLayer::onUpdate -- because ILayer::onUpdate()
	// takes no parameter and changing that signature across five layers is churn
	// Phase 1 does not need. Each null-checks and returns early. It is NOT a
	// general-purpose accessor: every function below those three takes
	// `const FrameContext&` as its first parameter. `getCurrentFrame()` returning
	// a bare uint32_t (VulkanContext.h:58) is DELETED, and deleting it is what
	// stops the frame index leaking back into ad-hoc call sites.
	//
	//     const FrameContext* currentFrame() const { return m_FrameActive ? &m_Frame : nullptr; }
	const FrameContext* currentFrame() const;

	// Monotonic frame counter since init. Incremented in present(), never in
	// beginFrame(). Equals frame.number() for the duration of a frame.
	// Depended on by: the deferred-destruction stamp; the editor's ImGui
	// descriptor retirement list (MERGED-3 §3.6.2).
	// ADJUDICATION.md: this name, not getFrameNumber().
	uint64_t frameNumber() const;

	// Highest frame number whose submission is known complete, published from the
	// fence wait at the top of beginFrame():
	//     if (m_FrameNumber >= FRAMES_IN_FLIGHT)
	//         m_CompletedFrame = m_FrameNumber - FRAMES_IN_FLIGHT;
	// 0 until FRAMES_IN_FLIGHT frames have run. Take this formulation, not the
	// equivalent-looking `m_FrameNumber - retireFrame >= FRAMES_IN_FLIGHT`: that
	// one underflows on uint64_t during the first two frames and destroys
	// resources one frame early.
	// ADJUDICATION.md: this name, not getCompletedFrame().
	uint64_t completedFrame() const;

	// True between beginFrame() and endFrame(). Exists for the asserts that other
	// classes in the layer need and cannot express otherwise:
	// beginSingleTimeCommands asserts !frameActive(), and that assert -- not a
	// grep for wait-idle sites -- is the gate ADJUDICATION.md asks for.
	bool frameActive() const;

	// True between beginSwapchainRendering() and endSwapchainRendering().
	// VulkanComputePipeline::dispatch() asserts !renderingBlockOpen(), which is
	// the standing guard against VUID-vkCmdDispatch-renderpass returning once
	// something starts drawing geometry again.
	bool renderingBlockOpen() const;

	// =========================================================================
	// 2. PER-FRAME STAGING -- THE upload mechanism
	//    Depended on by: VulkanBuffer.h (VulkanBuffer::upload), VulkanImage.h
	//    (VulkanTexture's in-frame constructor), VulkanStaging.h (which this
	//    forwards to).
	// =========================================================================

	// Carves `size` bytes out of frame.index()'s staging arena and returns a
	// StagingAlloc valid until the end of that frame. Forwards to the one
	// VulkanStagingArena the context owns by value. Throws through X3_VK_CHECK on
	// failure; never returns an invalid allocation, which is why StagingAlloc has
	// no valid(). `alignment` must be a power of two.
	//
	// NEVER WAITS, NEVER SUBMITS. The caller memcpys into alloc.ptr and records
	// vkCmdCopyBuffer / vkCmdCopyBufferToImage into frame.cmd(); the frame fence
	// retires it. This is the in-frame half of the corrected wait-idle rule --
	// see the long note at the top of VulkanStaging.h.
	//
	// The struct is X3::StagingAlloc from VulkanStaging.h. MERGED-3 §3.2.8 shows
	// it nested inside VulkanContext; it is NOT nested, for the same reason
	// VulkanStagingArena is not: the arena owns the concept and the context
	// forwards to it.
	StagingAlloc stage(const FrameContext& frame, VkDeviceSize size,
	                   VkDeviceSize alignment = VulkanStagingArena::kDefaultAlignment);

	// =========================================================================
	// 3. DEFERRED DESTRUCTION
	//    Depended on by: VulkanBuffer.h (both classes' destructors and
	//    reallocating ensureCapacity), VulkanImage.h (both classes' destructors
	//    and VulkanImage::recreate), VulkanDescriptors.h
	//    (VulkanDescriptorSetRing's destructor), and every move-assignment in the
	//    layer (THE MOVE-ASSIGNMENT CONTRACT, VulkanTypes.h).
	//
	//    All three stamp the entry with retireFrame = m_FrameNumber and destroy
	//    it only once retireFrame <= m_CompletedFrame -- i.e. after
	//    FRAMES_IN_FLIGHT frames retire. drainDeletionQueue() runs at the top of
	//    beginFrame(), immediately after the fence wait, before anything else.
	//
	//    This is the fix for VUID-vkDestroyBuffer-buffer-00922 at Renderer.cpp:
	//    258, 270, 282, 295, 318, 332, 346, where buffers were destroyed inline
	//    while frame N-1's command buffer still named them.
	// =========================================================================

	// Buffer + its VMA allocation. Either may be VK_NULL_HANDLE (a
	// default-constructed resource deferring nothing), in which case this is a
	// no-op rather than an error -- move-assignment onto an empty object is
	// normal.
	void deferDestroy(VkBuffer buffer, VmaAllocation allocation);

	// Image + its VMA allocation + its view, as one entry, because they must die
	// together and in that order. Any of the three may be VK_NULL_HANDLE.
	void deferDestroy(VkImage image, VmaAllocation allocation, VkImageView view);

	// vkFreeDescriptorSets, deferred. The span is COPIED into the queue entry;
	// the caller's storage (VulkanDescriptorSetRing's std::array) does not
	// outlive the call. Freeing a set that a pending command buffer still has
	// bound is undefined behaviour, which is what makes
	// VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT on m_DescriptorPool
	// load-bearing rather than incidental.
	void deferFreeDescriptorSets(std::span<const VkDescriptorSet> sets);

	// =========================================================================
	// 4. SHARED RESOURCES
	// =========================================================================

	// Cached, context-owned sampler. Depended on by: VulkanImage.h
	// (VulkanTexture borrows one and must never destroy it), and the editor's
	// ViewportPanel (which stops creating its own).
	//
	// Samplers are never created per texture: maxSamplerAllocationCount is as low
	// as 4000 on some drivers, and one VkSampler may legally be repeated across
	// every element of a combined-image-sampler array. The cache is
	// std::unordered_map<SamplerDesc, VkSampler>, which is why VulkanTypes.h
	// defines std::hash<SamplerDesc> inline and gives SamplerDesc a defaulted
	// operator==. Destroyed in cleanup(), AFTER everything that borrowed from it.
	VkSampler getSampler(const SamplerDesc& desc);

	// The always-write rule's escape hatch. Depended on by:
	// VulkanDescriptors.h -- flush() asserts that EVERY binding declared by the
	// layout was written, so an absent skybox or an empty light list still has to
	// write something, and these are that something. A null or !valid() element
	// of sampledImageArray() also becomes dummyTexture().descriptor().
	//
	// Created during init(), out of frame, through the one surviving blocking
	// helper. Destroyed in cleanup(), after drainDeletionQueueFully().
	const VulkanTexture& dummyTexture()       const;   // 1x1 opaque black, SRGB
	const VulkanBuffer&  dummyStorageBuffer() const;   // 256 B device-local, zeroed
	const VulkanBuffer&  dummyUniformBuffer() const;   // 256 B device-local, zeroed

	// =========================================================================
	// 5. CACHED DEVICE LIMITS
	//    Depended on by: VulkanBuffer.h -- the ring stride is
	//        alignUp(max(sizePerFrame, kMinBufferSize),
	//                max(kind == Uniform ? limits().minUniformBufferOffsetAlignment
	//                                    : limits().minStorageBufferOffsetAlignment,
	//                    limits().nonCoherentAtomSize, VkDeviceSize(1)))
	//    and that expression is evaluated per ring, per reallocation.
	//
	//    Cached in pickPhysicalDevice: VulkanContext.cpp:90-92 already calls
	//    vkGetPhysicalDeviceProperties and throws the result away. Store .limits
	//    instead. A per-call vkGetPhysicalDeviceProperties in an allocation path
	//    is a syscall-shaped mistake.
	// =========================================================================
	const VkPhysicalDeviceLimits& limits() const;

	// =========================================================================
	// 6. PRESENTATION
	// =========================================================================

	// THE ADJUDICATED SIGNATURE (ADJUDICATION.md, "blitImageToSwapchain"). It
	// takes the frame explicitly rather than reaching for ambient current-frame
	// state, which is the habit the whole resource layer exists to break, and it
	// takes a VulkanImage& rather than a loose (VkImage, VkImageLayout, width,
	// height) tuple so that the layout it transitions through stays tracked.
	//
	// Obtains extent from src.extent() and the current layout from src.layout()
	// instead of RuntimeLayer.cpp:185's hardcoded VK_IMAGE_LAYOUT_GENERAL, and
	// calls src.transition(...) twice so the tracked layout is accurate across
	// the GENERAL -> TRANSFER_SRC_OPTIMAL -> GENERAL round trip. Opens no
	// rendering block: vkCmdBlitImage and vkCmdClearColorImage are transfer
	// commands and must be recorded outside one.
	//
	// THE Y-FLIP PACKING IS CARRIED OVER BYTE FOR BYTE. VulkanContext.cpp:905-906
	// and RuntimeLayer::CalculateViewportCoordinates() (RuntimeLayer.cpp:306-376)
	// are a matched pair -- the viewport arrives in the OpenGL glBlitFramebuffer
	// convention (x, y, x+w, y+h) with a bottom-left origin and the subtraction
	// converts it to Vulkan's top-left origin. Changing either half in isolation
	// flips the runtime image. The VK_FILTER_LINEAR at :911 stays too.
	//
	// Sole caller repo-wide: RuntimeLayer.cpp:183. The editor never blits; its
	// compute output reaches the screen as an ImGui-sampled texture.
	void blitImageToSwapchain(const FrameContext& frame, VulkanImage& src,
	                          glm::ivec4 viewport, glm::ivec2 windowSize);

	// =========================================================================
	// 7. THE ONE SURVIVING BLOCKING HELPER
	//
	//    ADJUDICATION.md's wait-idle ruling, in its CORRECTED form: in-frame
	//    uploads never wait (stage() + frame.cmd()); out-of-frame uploads MAY
	//    wait and are confined to initialisation and teardown -- ImGui fonts,
	//    dummy resources, and nothing else. This pair is that path, and it is the
	//    only one. The original ruling deleted it outright and was wrong: nothing
	//    could then have uploaded the font atlas or the dummy texture, both of
	//    which are built before any frame exists, and VulkanTexture's out-of-frame
	//    constructor would have had no legal body.
	//
	//    Final permitted wait-idle set after Phase 1: endSingleTimeCommands,
	//    recreateSwapchain, cleanup, drainDeletionQueueFully. The verification
	//    gate is NOT "grep finds zero wait sites" -- it is "none of them is
	//    reachable from a frame", enforced by the assert below.
	// =========================================================================

	// Allocates and begins a one-shot command buffer on m_CommandPool.
	// ASSERTS !frameActive(). That assert is the gate.
	VkCommandBuffer beginSingleTimeCommands();

	// Ends, submits, and vkQueueWaitIdles, then frees the command buffer.
	// Callers, exhaustively: VulkanTexture's out-of-frame constructor, the dummy
	// resources, and the ImGui font upload at ImGuiContext.cpp:192-197.
	void endSingleTimeCommands(VkCommandBuffer cmd);

	// =========================================================================
	// 8. RAW HANDLES the resource layer's .cpp files need
	//    (all of these already exist on VulkanContext today, unchanged; listed
	//    so the contract is complete and so nobody deletes one during the
	//    registry purge)
	// =========================================================================
	VkDevice         getDevice()             const;
	VkPhysicalDevice getPhysicalDevice()     const;
	VmaAllocator     getAllocator()          const;   // every vmaCreate*/vmaDestroy*
	VkDescriptorPool getDescriptorPool()     const;   // VulkanDescriptorSetRing, ImGui
	VkQueue          getGraphicsQueue()      const;
	uint32_t         getGraphicsQueueFamily() const;
	VkExtent2D       getSwapchainExtent()    const;
	VkFormat         getSwapchainImageFormat() const; // VkPipelineRenderingCreateInfo
	void             recreateSwapchain();

	// =========================================================================
	// 9. WHAT IS GONE, so that a half-finished migration is recognisable
	//
	//   getCurrentFrame() -> uint32_t         (VulkanContext.h:58)  DELETED
	//   getMaxFramesInFlight()                (VulkanContext.h:88)  DELETED
	//   MAX_FRAMES_IN_FLIGHT                  (VulkanContext.h:146) DELETED --
	//       FRAMES_IN_FLIGHT (VulkanTypes.h) is the only such constant.
	//   getMinImageCount()                    (VulkanContext.h:87)  DELETED
	//   swapBuffers()                         (VulkanContext.h:21)  DELETED
	//   ensureFrameStarted() / m_FirstFrame   (VulkanContext.h:64)  DELETED
	//   isRenderPassActive() / beginRenderPass() / beginOverlayRenderPass() /
	//   getRenderPass() / getOverlayRenderPass()                    DELETED --
	//       dynamic rendering; there are no VkRenderPass objects left.
	//   registerStorageBuffer/UniformBuffer/StorageImage/SampledImage and the
	//   four getBound*() maps           (VulkanContext.h:38-52)     DELETED --
	//       the global bound-resource registry. A resource reaches a shader only
	//       through a DescriptorWriter at a call site that knows its set.
	//   blitImageToSwapchain(VkImage, VkImageLayout, w, h, ...)     REPLACED by
	//       the FrameContext form in section 6.
	//   getUploadCommandBuffer() / endUploadRecording() /
	//   flushUploadsBlocking() / m_UploadPool / m_UploadCmd /
	//   m_UploadCmdRecording                                        NEVER ADDED --
	//       ADJUDICATION.md voids them; stage() is the upload mechanism.
	//   : public IRenderingContext                                  DELETED with
	//       the factory layer (Part 1).
	// =========================================================================
};

} // namespace contract
} // namespace X3
