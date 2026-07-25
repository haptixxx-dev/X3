#pragma once

// =============================================================================
// VulkanImage.h -- VulkanImage (storage image) and VulkanTexture (sampled).
// Replaces IImage2D / VulkanImage2D and ITexture2D / VulkanTexture2D.
//
// FILE SUBSUMPTION -- read before following any include list in MERGED-3.
// THIS HEADER SUBSUMES `VulkanTexture.h`. MERGED-3 §3.2.5 presents VulkanTexture
// in a file of its own and §3.7.2's edit table tells Renderer.h to include
// `Platform/Vulkan/VulkanTexture.h`. That file does not exist and will not be
// created: splitting them buys nothing (they share ImageDesc/TextureDesc, the
// same deferDestroy() route and the same ownership prose) and costs a header
// whose only content is one class. Every `#include "Platform/Vulkan/VulkanTexture.h"`
// in the spec means THIS file. Renderer.h's include list is therefore
// VulkanBuffer.h, VulkanImage.h, VulkanDescriptors.h, VulkanComputePipeline.h --
// four headers, not five, and no FrameContext.h either (VulkanTypes.h subsumes
// that one; see the note at the top of it).
//
// Neither class has a Bind() or an Unbind(), and neither registers itself
// anywhere. The global bound-resource registry on VulkanContext is deleted; a
// resource reaches a shader only by being handed to a DescriptorWriter at a call
// site that knows which set it is writing (VulkanDescriptors.h).
//
// Consumed elsewhere, recorded here so the seam is not lost:
//     void VulkanContext::blitImageToSwapchain(const FrameContext& frame,
//                                              VulkanImage& src,
//                                              glm::ivec4 viewport,
//                                              glm::ivec2 windowSize);
// That is the adjudicated signature. It is declared on VulkanContext, not here,
// and it takes the frame explicitly rather than reaching for ambient
// current-frame state. Like every other VulkanContext member this layer depends
// on, it is written down in VulkanContextInterface.h -- today's VulkanContext.h
// still has the old VkImage/VkImageLayout/width/height form and Phase 1
// replaces it.
// =============================================================================

#include "Platform/Vulkan/VulkanTypes.h"

namespace X3
{

class VulkanContext;

// -----------------------------------------------------------------------------
// Storage image. Owns VkImage + VmaAllocation + one default VkImageView, and
// tracks its own layout and last access/stage so barriers are derivable rather
// than hand-specified at every call site.
//
// Ownership: exclusive, move-only. The destructor and recreate() both hand the
// old (VkImage, VmaAllocation, VkImageView) triple to
// VulkanContext::deferDestroy(VkImage, VmaAllocation, VkImageView); they are
// destroyed only after FRAMES_IN_FLIGHT frames retire, because the previous
// frame's command buffer and the editor's previous ImGui pass may still
// reference them. Nothing here calls vmaDestroyImage or vkDestroyImageView
// directly. Renderer owns these by value (std::array<VulkanImage,
// FRAMES_IN_FLIGHT>) and destroys them in Renderer::Shutdown().
//
// Move semantics: exactly THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h. Two
// clauses of it matter more here than anywhere else, because
// `m_Frames[i] = VulkanImage{};` is the documented shutdown mechanism for this
// very class:
//   * the displaced (VkImage, VmaAllocation, VkImageView) triple goes to
//     ctx.deferDestroy(), exactly as the destructor would route it -- the editor
//     may still hold an ImGui descriptor set pointing at that view;
//   * id() and generation() move to the destination and the SOURCE gets a fresh
//     id, so no two live images share a key in the ImGui descriptor map.
//
// The constructor deliberately does NOT submit anything: no
// beginSingleTimeCommands, no vkQueueWaitIdle, no initial layout transition, and
// no pixel-upload path. The image is left in VK_IMAGE_LAYOUT_UNDEFINED and the
// first transition(frame, VK_IMAGE_LAYOUT_GENERAL, ...) inside the frame command
// buffer does the work; UNDEFINED -> GENERAL is always legal.
// -----------------------------------------------------------------------------
class VulkanImage
{
public:
	VulkanImage() = default;
	VulkanImage(VulkanContext& ctx, const ImageDesc& desc);
	~VulkanImage();

	VulkanImage(VulkanImage&&) noexcept;
	VulkanImage& operator=(VulkanImage&&) noexcept;
	VulkanImage(const VulkanImage&)            = delete;
	VulkanImage& operator=(const VulkanImage&) = delete;

	// Defer-destroys the current image/view and allocates new ones.
	// id() is UNCHANGED; generation() is incremented. Any descriptor or ImGui
	// registration referencing this image must be refreshed -- compare
	// generation(), not id().
	//
	// THIS IS THE PRIMARY CREATION PATH, NOT ONLY A RESIZE PATH, and it is legal
	// on a default-constructed image whose m_Ctx is null. Renderer holds
	// `std::array<VulkanImage, FRAMES_IN_FLIGHT> m_Frames;`, which is
	// default-constructed with no context and no desc, and every element gets its
	// first real allocation from a recreate() call inside Renderer::Draw when the
	// viewport size is first known. There is no other way in: the two-argument
	// constructor cannot be used on an array element without a move-assignment,
	// and a move-assignment is a heavier contract than "fill in the empty one".
	//
	// WHERE THE CONTEXT COMES FROM, precisely: frame.context(). Not a singleton,
	// not a stored pointer, not an argument -- the FrameContext already carries
	// the context that produced it, which is the same object that owns the
	// command buffer these barriers are recorded into and the deletion queue the
	// old handles go onto. Taking it from anywhere else would allow those three
	// to disagree.
	//
	// AND IT LATCHES. On the first successful recreate(), m_Ctx is set to
	// &frame.context() and never changes again for the life of the object. Every
	// later call asserts `m_Ctx == nullptr || m_Ctx == &frame.context()`; an
	// image cannot migrate between contexts, because its deferred destroy is
	// queued on the context it latched and freeing an allocation on a different
	// VmaAllocator is undefined. The same latch governs the destructor: an image
	// that never latched (default-constructed, never recreated) owns nothing and
	// its destructor does nothing, which is exactly why the null m_Ctx is a legal
	// state rather than a half-built one.
	void recreate(const FrameContext& frame, const ImageDesc& desc);

	// Records a VkImageMemoryBarrier into frame.cmd() from the tracked
	// (layout, lastAccess, lastStage) to (newLayout, dstAccess, dstStage), then
	// updates the tracked state. The stage is explicit: the consumer in this
	// engine is a COMPUTE shader, and the old code's hardcoded FRAGMENT_SHADER
	// stages were wrong.
	//
	// ELISION: governed by THE BARRIER ELISION RULE in VulkanTypes.h, which is
	// NOT the "no-op if already in newLayout with a superset access mask" rule
	// this comment used to state. That rule elided the WAW case
	// (SHADER_WRITE -> SHADER_WRITE, an equal and therefore "superset" mask) and
	// the RAW case (SHADER_WRITE -> SHADER_READ, a strict subset), which are the
	// two hazards that actually occur here and neither of which the validation
	// layers report as an error. Concretely: the same image is written by the
	// path tracer and then read by nothing else in-frame, but on a resize the
	// TRANSFER_WRITE of a clear followed by a SHADER_READ would have been elided
	// outright.
	//   The rule in force: elide only when the layout is unchanged AND neither
	// the tracked access nor the requested access contains any write bit AND the
	// requested access is already covered. Read-after-read only. Everything else
	// records a barrier, including every same-layout case involving a write --
	// for which barrier() below is the explicit, self-documenting spelling.
	void transition(const FrameContext& frame, VkImageLayout newLayout,
	                VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

	// Same-layout execution + memory barrier. REQUIRED for the accumulation
	// read-modify-write across frames (PathTracing.comp does imageLoad then
	// imageStore on the same image), which transition() would skip because the
	// layout is unchanged.
	void barrier(const FrameContext& frame,
	             VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

	VkImage       handle()     const { return m_Image; }
	VkImageView   view()       const { return m_View; }
	VkImageLayout layout()     const { return m_Layout; }
	VkExtent2D    extent()     const { return m_Extent; }
	VkFormat      format()     const { return m_Format; }
	glm::ivec2    dimensions() const { return { int(m_Extent.width), int(m_Extent.height) }; }
	bool          valid()      const { return m_Image != VK_NULL_HANDLE; }

	// Stable object identity from a process-wide atomic counter
	// (nextResourceId(), VulkanTypes.h). Replaces IImage2D::GetID() as a cache
	// key: never truncates, never a raw handle, never reused, never 0. Keying the
	// editor's ImGui descriptor map on this is what bounds that map at
	// FRAMES_IN_FLIGHT entries forever, regardless of how many resolution changes
	// occur.
	//
	// VALID FROM CONSTRUCTION, FOR EVERY CONSTRUCTOR INCLUDING THE DEFAULT ONE.
	// m_Id is a default member initialiser, not something the two-argument
	// constructor assigns, and that is the fix for a real defect: Renderer's
	// `std::array<VulkanImage, FRAMES_IN_FLIGHT> m_Frames;` is
	// default-constructed and only later filled in by recreate(), so with
	// `m_Id = 0` both elements would have carried id 0 -- the map of §3.6.2 would
	// have had one entry serving two images and the editor would have sampled
	// whichever view was registered most recently, alternating every frame.
	//
	// Unchanged by recreate() (that is what generation() is for) and unchanged by
	// a move-INTO (the destination adopts the source's id; the source is given a
	// fresh one -- see THE MOVE-ASSIGNMENT CONTRACT). So id() is stable for as
	// long as a cache can observe it, and is unique across all live objects.
	uint64_t id() const { return m_Id; }

	// Incremented by every successful recreate(). A cache keyed on id() must
	// also store generation() and refresh when it differs. 0 means "never
	// allocated": on a default-constructed image, id() is already valid and
	// generation() is 0, which a cache may use to skip registration entirely.
	uint64_t generation() const { return m_Generation; }

	// { VK_NULL_HANDLE, view(), layout() } -- DERIVED FROM THE TRACKED LAYOUT,
	// not hardcoded to VK_IMAGE_LAYOUT_GENERAL.
	//
	// The hardcoded form was a lie waiting to be told: this object tracks
	// m_Layout precisely because it moves, and blitImageToSwapchain takes it
	// through GENERAL -> TRANSFER_SRC_OPTIMAL -> GENERAL every runtime frame. A
	// descriptor written from the middle of that round trip claimed GENERAL for
	// an image in TRANSFER_SRC_OPTIMAL, which is VUID-VkWriteDescriptorSet-
	// descriptorType-04152 and, in practice, a read of undefined contents.
	// Returning m_Layout makes the descriptor honest, and the assert makes the
	// call-site ordering error loud rather than latent -- a storage image
	// descriptor must be GENERAL, so if the tracked layout is anything else the
	// bug is that the caller forgot to transition back, and it is caught here
	// instead of at the dispatch.
	VkDescriptorImageInfo storageDescriptor() const
	{
		assert(m_Layout == VK_IMAGE_LAYOUT_GENERAL &&
		       "storage image descriptor written while the image is not in GENERAL -- "
		       "transition(frame, VK_IMAGE_LAYOUT_GENERAL, ...) before writing the set");
		return VkDescriptorImageInfo{ VK_NULL_HANDLE, m_View, m_Layout };
	}

private:
	VulkanContext*       m_Ctx        = nullptr;
	VkImage              m_Image      = VK_NULL_HANDLE;
	VmaAllocation        m_Allocation = VK_NULL_HANDLE;
	VkImageView          m_View       = VK_NULL_HANDLE;
	VkExtent2D           m_Extent     {};
	VkFormat             m_Format     = VK_FORMAT_UNDEFINED;
	VkImageLayout        m_Layout     = VK_IMAGE_LAYOUT_UNDEFINED;
	VkAccessFlags        m_LastAccess = 0;
	VkPipelineStageFlags m_LastStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	uint64_t             m_Id         = nextResourceId();   // valid in EVERY ctor
	uint64_t             m_Generation = 0;
};

// -----------------------------------------------------------------------------
// Immutable sampled texture: image + view + a sampler BORROWED from the
// context's sampler cache.
//
// Ownership: owns (VkImage, VmaAllocation, VkImageView) exclusively and routes
// them through VulkanContext::deferDestroy() in the destructor -- move-only,
// same rule as VulkanImage. It does NOT own m_Sampler: that handle comes from
// VulkanContext::getSampler(const SamplerDesc&) and is destroyed with the
// context's sampler cache in VulkanContext::cleanup(), after everything that
// borrows from it. Never call vkDestroySampler here.
//
// Layout: left in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL after construction
// and never transitioned again; the TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL
// barrier must target VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
// VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, because the skybox is sampled from a
// compute shader. This is why descriptor() may hardcode the layout where
// VulkanImage::storageDescriptor() may not: an immutable texture has exactly one
// layout for its whole life and no member tracking one, whereas a storage image
// has a tracked layout that moves.
//
// Move semantics: exactly THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h. The
// displaced (VkImage, VmaAllocation, VkImageView) go to ctx.deferDestroy(); the
// displaced m_Sampler is simply dropped, because it was borrowed from the
// context's cache and the cache still owns it -- do not defer it and do not
// destroy it.
// -----------------------------------------------------------------------------
class VulkanTexture
{
public:
	VulkanTexture() = default;

	// THE IN-FRAME CONSTRUCTOR. This is the one every asset load uses, and it
	// NEVER WAITS -- ADJUDICATION.md, "Wait-idle post-condition": in-frame uploads
	// go through ctx.stage() into the frame's command buffer and are retired by
	// the frame fence. `pixels` must be width*height*bytesPerPixel(format) bytes
	// and is copied immediately into the frame staging arena. The copy and the
	// UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY_OPTIMAL transitions are
	// recorded into frame.cmd(): no vkQueueWaitIdle, no mid-frame submit, no
	// beginSingleTimeCommands. The per-call queue wait in the old
	// endSingleTimeCommands is what Phase 1 removes from THIS path.
	// Asserts desc.mipLevels == 1 (see TextureDesc in VulkanTypes.h).
	VulkanTexture(VulkanContext& ctx, const FrameContext& frame,
	              const TextureDesc& desc, const void* pixels);

	// THE OUT-OF-FRAME CONSTRUCTOR. It BLOCKS, and that is legal, and it is
	// legal only here.
	//
	// ADJUDICATION.md's wait-idle ruling was corrected after this header was
	// first written, and the corrected rule distinguishes WHEN a wait may happen,
	// not WHETHER one may exist at all. The first version deleted
	// endSingleTimeCommands outright; that is not implementable, because ImGui's
	// font upload and the context's own dummy resources are created before any
	// frame exists, and this constructor would then have had no legal body. So:
	//   * in-frame uploads never wait (the constructor above);
	//   * out-of-frame uploads may wait, and are CONFINED TO INITIALISATION AND
	//     TEARDOWN: ImGui fonts, dummy resources, and nothing else;
	//   * exactly ONE blocking submit-and-wait helper survives to serve them --
	//     beginSingleTimeCommands / endSingleTimeCommands on VulkanContext.
	//
	// This constructor is that path's only user in this layer. It uses
	// beginSingleTimeCommands/endSingleTimeCommands and therefore performs a
	// vkQueueWaitIdle before returning.
	//
	// IT IS UNREACHABLE FROM A FRAME, structurally and by assert. It takes no
	// FrameContext -- there is no way to reach it from code that has one and no
	// way for it to record into a frame command buffer -- and
	// beginSingleTimeCommands asserts `!m_FrameActive`, so calling it between
	// beginFrame() and endFrame() fires in debug rather than stalling the
	// pipeline in release. That assert, not a grep, is the gate: MERGED-3 §3.2.8
	// says the check is "none of the permitted wait sites is reachable from a
	// frame", and a grep for zero wait-idle sites would fail against a design
	// that legitimately has four (this helper, recreateSwapchain, cleanup,
	// drainDeletionQueueFully).
	//
	// Callers, exhaustively: VulkanContext's dummy texture, built during init;
	// and the ImGui font upload at ImGuiContext.cpp:192-197 (which uses the
	// helper directly rather than this constructor). Anything else that wants to
	// create a texture is inside a frame and uses the constructor above.
	// Asserts desc.mipLevels == 1, and asserts that no frame is active.
	VulkanTexture(VulkanContext& ctx, const TextureDesc& desc, const void* pixels);

	~VulkanTexture();

	VulkanTexture(VulkanTexture&&) noexcept;
	VulkanTexture& operator=(VulkanTexture&&) noexcept;
	VulkanTexture(const VulkanTexture&)            = delete;
	VulkanTexture& operator=(const VulkanTexture&) = delete;

	VkImage     handle()  const { return m_Image; }
	VkImageView view()    const { return m_View; }
	VkSampler   sampler() const { return m_Sampler; }
	VkExtent2D  extent()  const { return m_Extent; }
	bool        valid()   const { return m_Image != VK_NULL_HANDLE; }

	// Same rules as VulkanImage::id(): from nextResourceId(), never 0, valid
	// after every constructor including the default one, unique across live
	// objects (a move gives the source a fresh id). A default-constructed
	// VulkanTexture -- which is what Renderer's `VulkanTexture m_SkyboxTexture;`
	// is until a skybox is loaded -- therefore has a usable key immediately.
	uint64_t    id()      const { return m_Id; }

	// { sampler(), view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }.
	// Hardcoding the layout is correct here and only here: this class is
	// immutable, has exactly one layout for its whole life, and tracks none.
	VkDescriptorImageInfo descriptor() const;

private:
	VulkanContext* m_Ctx        = nullptr;
	VkImage        m_Image      = VK_NULL_HANDLE;
	VmaAllocation  m_Allocation = VK_NULL_HANDLE;
	VkImageView    m_View       = VK_NULL_HANDLE;
	VkSampler      m_Sampler    = VK_NULL_HANDLE;   // borrowed from ctx cache
	VkExtent2D     m_Extent     {};
	uint64_t       m_Id         = nextResourceId();   // valid in EVERY ctor
};

}
