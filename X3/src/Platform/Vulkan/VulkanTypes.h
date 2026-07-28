#pragma once

// =============================================================================
// VulkanTypes.h -- the canonical shared vocabulary of the Vulkan-native
// resource layer. Every other header in this layer includes this one and
// nothing else from the layer that it does not strictly need.
//
// FILE SUBSUMPTION -- read before following any include list in MERGED-3.
// This file subsumes the two placeholder headers the merged spec described
// separately: `VulkanCommon.h` and `FrameContext.h`. NEITHER FILE EXISTS AND
// NEITHER WILL BE CREATED. There is exactly one definition of FRAMES_IN_FLIGHT,
// one FrameContext, and one copy of every descriptor struct, and they are all
// here. Do not re-declare any of them elsewhere; ADJUDICATION.md exists because
// four parts each invented their own. Consequently:
//   * MERGED-3 §3.7.2 tells Renderer.h to include "FrameContext.h" and every
//     §3.2.x code block opens with `#include "Platform/Vulkan/VulkanCommon.h"`
//     and/or `"Platform/Vulkan/FrameContext.h"`. Every one of those include
//     lines resolves to THIS header instead. A literal transcription of those
//     spec include lists will not compile.
//   * MERGED-3 §3.7.2 also tells Renderer.h to include
//     "Platform/Vulkan/VulkanTexture.h", which likewise does not exist:
//     VulkanImage.h subsumes it (VulkanImage + VulkanTexture live in that one
//     header). See the corresponding note at the top of VulkanImage.h.
//
// Binding decisions honoured here (ADJUDICATION.md):
//   * FRAMES_IN_FLIGHT is the only frames-in-flight constant. VulkanContext's
//     MAX_FRAMES_IN_FLIGHT (VulkanContext.h:146) and getMaxFramesInFlight()
//     (VulkanContext.h:88) are deleted when the context is migrated.
//   * Frame accessors on VulkanContext are frameNumber() / completedFrame().
//     getFrameNumber() / getCompletedFrame() do not exist.
//   * SamplerDesc gains maxLod; TextureDesc already carries mipLevels.
//
// The VulkanContext members every header in this layer calls -- stage(),
// deferDestroy(), deferFreeDescriptorSets(), getSampler(), limits(),
// dummyTexture()/dummyStorageBuffer()/dummyUniformBuffer(), currentFrame(),
// beginFrame(), frameNumber(), completedFrame(), blitImageToSwapchain() -- are
// all declared in VulkanContext.h, which is now the contract itself. The
// separate VulkanContextInterface.h that held the target shape during the
// migration is deleted.
// =============================================================================

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace X3
{

class VulkanContext;

// -----------------------------------------------------------------------------
// THE frames-in-flight constant. Everything indexed by frame slot -- command
// buffers, fences, ring-buffer slots, descriptor sets, staging arenas -- is
// sized by this and only this. Never MAX_FRAMES_IN_FLIGHT.
// -----------------------------------------------------------------------------
inline constexpr uint32_t FRAMES_IN_FLIGHT = 2;

// Logs file/line and throws std::runtime_error on non-VK_SUCCESS.
// Defined in VulkanContext.cpp; construction failure in this layer is fatal
// rather than silently producing a half-built object with a null handle.
void VkCheck(VkResult result, const char* expr, const char* file, int line);

// THE MACRO IS NAMED X3_VK_CHECK, NOT VK_CHECK, AND THAT IS DELIBERATE.
// This header is included by VulkanBuffer.h / VulkanImage.h / VulkanDescriptors.h,
// which Renderer.h includes by value, which the editor's panels include. An
// unguarded `#define VK_CHECK` therefore reaches ImGui, vk-bootstrap, VMA and
// every future third-party Vulkan helper -- all of which conventionally define a
// macro of exactly that name. A macro collision on a name that generic produces
// an error inside someone else's header with no indication of where it came
// from. `VK_CHECK` is not defined by this layer at all; do not add a
// compatibility alias for it, guarded or otherwise, because a guarded alias
// silently binds to whoever defined it first.
#define X3_VK_CHECK(x) ::X3::VkCheck((x), #x, __FILE__, __LINE__)

// -----------------------------------------------------------------------------
// Process-wide monotonic resource identity.
//
// Every VulkanImage and VulkanTexture takes its id() from here as a DEFAULT
// MEMBER INITIALISER, so an id is assigned by every constructor including the
// defaulted default constructor. That matters because
// `std::array<VulkanImage, FRAMES_IN_FLIGHT> m_Frames;` in Renderer is
// default-constructed and the editor's ImGui descriptor map is keyed on id()
// (MERGED-3 §3.6.2): if default construction left id() == 0, both elements of
// that array would collide on key 0 and the two frames would share one
// VkDescriptorSet pointing at whichever view was registered last.
//
// Starts at 1, so 0 is never a valid id and can still be used as a "never seen"
// sentinel by a cache. Never reused, never truncated, never a raw handle.
// -----------------------------------------------------------------------------
inline uint64_t nextResourceId()
{
	static std::atomic<uint64_t> s_NextResourceId{1};
	return s_NextResourceId.fetch_add(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// THE BARRIER ELISION RULE, stated once for the whole layer.
//
// VulkanImage::transition() may skip recording a barrier only when skipping it
// cannot hide a hazard. The rule as originally written -- "no-op if already in
// newLayout with a superset access mask" -- is WRONG, and wrong in the direction
// that produces silent corruption rather than a validation error:
//
//   * WAW. m_LastAccess = SHADER_WRITE, dstAccess = SHADER_WRITE. The new mask
//     is a subset of the old (it is equal to it), so the old rule elides -- and
//     two dispatches writing the same image are then unordered.
//   * RAW. m_LastAccess = SHADER_WRITE (superset), dstAccess = SHADER_READ
//     (subset). The old rule elides, and the reader may observe pre-write data.
//   * WAR. m_LastAccess = SHADER_READ, dstAccess = SHADER_WRITE. Not a subset,
//     so the old rule happens to record a barrier -- by luck, not by design.
//
// A subset test answers "is this access already visible?", which is only the
// right question when nothing has been written. So:
//
//   ELIDE  if and only if  newLayout == m_Layout
//                    AND   !isWriteAccess(m_LastAccess)
//                    AND   !isWriteAccess(dstAccess)
//                    AND   (dstAccess & ~m_LastAccess) == 0
//
//   RECORD a barrier in every other case, including the layout-unchanged case.
//
// Read-after-read with an already-covered mask is the only genuinely free case,
// and it is the only one elided. Anything touching a write -- on either side --
// records. This is also why barrier() exists as a separate entry point: the
// accumulation read-modify-write is layout-unchanged AND write-on-both-sides,
// which the rule above correctly refuses to elide, and barrier() states that
// intent at the call site instead of relying on the elision test to fail.
// -----------------------------------------------------------------------------
constexpr bool isWriteAccess(VkAccessFlags access)
{
	constexpr VkAccessFlags kWriteBits =
		VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	return (access & kWriteBits) != 0;
}

// -----------------------------------------------------------------------------
// THE MOVE-ASSIGNMENT CONTRACT, stated once for every RAII class in this layer
// (VulkanBuffer, VulkanRingBuffer, VulkanImage, VulkanTexture,
// VulkanDescriptorSetRing, VulkanDescriptorSetLayout, VulkanComputePipeline,
// VulkanStagingArena). Each class points here rather than restating it.
//
// This is load-bearing, not pedantry: `m_Frames[i] = VulkanImage{};` is the
// documented shutdown/reset mechanism, so move-assignment is a destruction path
// and its handling of the DISPLACED handles is exactly as important as the
// destructor's.
//
//   1. Move-CONSTRUCTION transfers every handle and every tracked field from the
//      source, then leaves the source in the default-constructed state:
//      all handles VK_NULL_HANDLE, m_Ctx nullptr, sizes/extents zero. The
//      moved-from object is destructible and reassignable, and its destructor
//      does nothing because it owns nothing.
//
//   2. Move-ASSIGNMENT first disposes of *this*'s current contents by exactly
//      the same route its destructor would -- i.e. it hands them to
//      VulkanContext::deferDestroy(...) (or deferFreeDescriptorSets(...) for a
//      descriptor-set ring), never to vmaDestroyBuffer / vmaDestroyImage /
//      vkDestroyImageView / vkFreeDescriptorSets directly -- and only then takes
//      the source's handles. The displaced handles may still be named by a
//      command buffer in flight, which is the entire reason the deferred queue
//      exists; disposing of them inline would reintroduce
//      VUID-vkDestroyBuffer-buffer-00922 at the one call site nobody thinks of
//      as a destruction.
//
//   3. Self-move-assignment (`a = std::move(a)`) is a no-op, checked with
//      `if (this == &other) return *this;` as the first statement.
//
//   4. The context pointer moves with the handles. If *this* had a null m_Ctx
//      (default-constructed) it adopts the source's; if *this* had a non-null
//      m_Ctx it must equal the source's, and debug builds assert that -- two
//      contexts in one process is not a supported configuration and a
//      cross-context move would defer the destroy onto the wrong deletion queue.
//
//   5. Identity does NOT survive as a duplicate. For the id()-bearing classes
//      (VulkanImage, VulkanTexture) the destination adopts the source's id() and
//      generation(), and the source is given a FRESH id from nextResourceId().
//      Two live objects therefore never share an id, so the ImGui descriptor map
//      of MERGED-3 §3.6.2 cannot alias two images onto one entry. A cache
//      holding the destination's old id sees a key that no longer exists and
//      re-registers, which is correct; a cache holding the source's old id now
//      finds it on the destination, which is also correct -- the pixels moved
//      there.
//
//   6. Move-assignment is `noexcept`. deferDestroy() only appends to a vector
//      the context reserves; a throw during a move would leave two objects
//      owning one handle with no way to say which.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Descs
// -----------------------------------------------------------------------------

// Selects the base usage flags and the offset alignment of a buffer.
// Uniform -> VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT + minUniformBufferOffsetAlignment
// Storage -> VK_BUFFER_USAGE_STORAGE_BUFFER_BIT + minStorageBufferOffsetAlignment
enum class BufferKind : uint8_t
{
	Uniform,
	Storage
};

// Storage / attachment image. `usage` is explicit and replaces the old
// Image2DType enum entirely; there is no implicit usage derivation.
struct ImageDesc
{
	uint32_t          width     = 0;
	uint32_t          height    = 0;
	VkFormat          format    = VK_FORMAT_R32G32B32A32_SFLOAT;
	VkImageUsageFlags usage     = 0;
	uint32_t          mipLevels = 1;
	const char*       debugName = nullptr;
};

// Key of the context-owned sampler cache (VulkanContext::getSampler). Samplers
// are never created per texture: maxSamplerAllocationCount is as low as 4000 on
// some drivers, and one VkSampler may legally be repeated across every element
// of a combined-image-sampler array.
//
// maxLod exists because VulkanTexture2D.cpp:174 hardcoded 0.0f, which silently
// disabled mips even when they existed. The default is VK_LOD_CLAMP_NONE.
//
// VK_LOD_CLAMP_NONE IS SAFE ONLY BECAUSE PHASE 1 PINS TextureDesc::mipLevels TO
// 1 (see TextureDesc below). An unclamped maxLod against a view with a single
// mip level cannot sample past that level -- the implementation clamps to
// levelCount-1 = 0 -- so the default is a no-op today and becomes correct
// automatically on the day Phase 2 adds mip generation. What it must NOT be
// paired with is mipLevels > 1 and no generation pass, which is exactly the
// combination TextureDesc now rejects.
struct SamplerDesc
{
	VkFilter             filter      = VK_FILTER_LINEAR;
	VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	VkSamplerMipmapMode  mipmapMode  = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	float                maxLod      = VK_LOD_CLAMP_NONE;

	bool operator==(const SamplerDesc&) const = default;
};

// Immutable sampled texture. mipLevels is already here; do not add it again.
//
// PHASE 1 DECISION: mipLevels MUST BE 1. Both VulkanTexture constructors assert
// `desc.mipLevels == 1` and there is no path that relaxes it in this phase.
//
// The reason is that mipLevels is only half of a feature. Nothing in this layer
// generates mip content: the in-frame constructor stages `pixels` and records a
// single vkCmdCopyBufferToImage into mip 0, and the out-of-frame constructor
// does the same on the blocking helper. There is no vkCmdBlitImage chain, no
// per-level layout ladder, and no format-feature check for
// VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT (which mip generation by
// blit requires and which VK_FORMAT_R8G8B8A8_SRGB is not guaranteed to advertise
// on every device). So `mipLevels = 4` would allocate four levels, fill one, and
// -- with SamplerDesc::maxLod defaulting to VK_LOD_CLAMP_NONE -- sample
// uninitialised device memory at any minified footprint. Not a validation error,
// not reproducible, and it looks like a shading bug.
//
// The alternatives were: (a) declare a generation path here, or (b) constrain
// the field. (b) is taken. Generation belongs with Phase 2's material-texture
// array, where mips actually pay for themselves and where the blit chain,
// the format-feature query and the fallback for formats that fail it can be
// designed as one piece. Until then the field exists so that the struct does not
// change shape in Phase 2, and it is pinned so it cannot lie in Phase 1.
struct TextureDesc
{
	uint32_t    width     = 0;
	uint32_t    height    = 0;
	VkFormat    format    = VK_FORMAT_R8G8B8A8_SRGB;
	uint32_t    mipLevels = 1;   // Phase 1: must be 1; asserted by both ctors.
	SamplerDesc sampler   {};
	const char* debugName = nullptr;
};

// One row of a descriptor set layout's binding table. `count > 1` declares an
// array binding, which DescriptorWriter::sampledImageArray writes whole.
// In Phase 3 this table becomes Slang-reflection-generated.
struct DescriptorBindingDesc
{
	uint32_t           binding = 0;
	VkDescriptorType   type    = VK_DESCRIPTOR_TYPE_MAX_ENUM;
	uint32_t           count   = 1;
	VkShaderStageFlags stages  = VK_SHADER_STAGE_COMPUTE_BIT;
};

// -----------------------------------------------------------------------------
// FrameContext -- the single object that threads the frame index
// -----------------------------------------------------------------------------
// Non-owning view of the in-progress frame. Valid ONLY between
// VulkanContext::beginFrame() and VulkanContext::endFrame(). Never stored by a
// resource, never cached across frames, never copied into a member. Obtainable
// ONLY from VulkanContext (beginFrame() returns const FrameContext*, and
// currentFrame() returns it again for layers whose onUpdate() takes no
// parameter); everything below those two points takes const FrameContext& as
// its first parameter.
//
// Why holding one is a synchronisation guarantee, not a convenience: beginFrame()
// executes vkWaitForFences on m_InFlightFences[m_CurrentFrame] before it returns
// this object, and that fence was signalled by the submit of the previous use of
// the same slot (and is created signalled at first use). So while a caller holds
// a FrameContext, the GPU has provably finished every command that referenced
// slot index(). The CPU may overwrite that slot with no further synchronisation.
// That is the entire basis of the per-frame ring contract in VulkanBuffer.h and
// of the "write set index() only" rule in VulkanDescriptors.h. Caching a
// FrameContext across frames is forbidden, not discouraged: it silently converts
// a proof into a guess.
//
// Ownership: VulkanContext holds exactly one FrameContext as a value member. It
// owns no Vulkan handles and destroys nothing.
//
// THE "NEVER CACHE A FRAME CONTEXT" RULE IS ENFORCED, NOT DOCUMENTED.
// FrameContext is non-copyable AND non-movable. Every one of the four
// copy/move operations is deleted below, so none of these compiles:
//
//     struct Evil { FrameContext stale; };          // ill-formed: no way to fill it
//     FrameContext saved = *ctx.currentFrame();     // ill-formed
//     m_Frame = frame;                              // ill-formed
//     void f(FrameContext frame);                   // uncallable
//     std::vector<FrameContext> v; v.push_back(f);  // ill-formed
//
// A caching bug is therefore a compile error at the line that would have cached
// it, rather than a stale index discovered three frames later as a flicker. The
// only ways to name one are `const FrameContext*` from
// VulkanContext::beginFrame() / currentFrame() and `const FrameContext&`
// threaded down as a parameter, which is exactly the surface the layer wants.
//
// If something legitimately needs to pass a frame along, it passes
// `const FrameContext&`. There is no legitimate need to own one; VulkanContext
// owns the single instance and it is a private member filled through friendship.
// The deleted copy constructor is a user-declared constructor, which is why the
// default constructor is re-declared explicitly -- VulkanContext's `FrameContext
// m_Frame;` member needs it.
class FrameContext
{
public:
	FrameContext(const FrameContext&)            = delete;
	FrameContext& operator=(const FrameContext&) = delete;
	FrameContext(FrameContext&&)                 = delete;
	FrameContext& operator=(FrameContext&&)      = delete;

	// 0 .. FRAMES_IN_FLIGHT-1. Indexes every per-frame ring in the engine.
	uint32_t index() const { return m_Index; }

	// The primary command buffer for this frame, already in the recording state.
	// Every upload, barrier and dispatch for this frame is recorded into it.
	VkCommandBuffer cmd() const { return m_Cmd; }

	// Monotonic frame counter since context init. Matches
	// VulkanContext::frameNumber() for the duration of the frame, and is what
	// the deferred-destruction queue stamps its entries with.
	uint64_t number() const { return m_Number; }

	VulkanContext& context() const { return *m_Context; }

private:
	friend class VulkanContext;

	// Private, so a caller cannot default-construct one either. Deleting the
	// copy and move operations alone was not enough: with a public default
	// constructor, `struct Evil { FrameContext stale; };` still compiled and
	// `e.stale.index()` read a zero-initialised frame. VulkanContext reaches
	// this through the friendship above for its single `FrameContext m_Frame`.
	FrameContext() = default;

	VulkanContext*  m_Context = nullptr;
	VkCommandBuffer m_Cmd     = VK_NULL_HANDLE;
	uint32_t        m_Index   = 0;
	uint64_t        m_Number  = 0;
};

}

// Required by VulkanContext's std::unordered_map<SamplerDesc, VkSampler> sampler
// cache. Defined inline (not merely declared) so that instantiating the map does
// not produce an undefined symbol.
template <>
struct std::hash<X3::SamplerDesc>
{
	std::size_t operator()(const X3::SamplerDesc& d) const noexcept
	{
		std::size_t h = static_cast<std::size_t>(d.filter);
		h = h * 31u + static_cast<std::size_t>(d.addressMode);
		h = h * 31u + static_cast<std::size_t>(d.mipmapMode);
		h = h * 31u + std::hash<float>{}(d.maxLod);
		return h;
	}
};
