#pragma once

// =============================================================================
// VulkanBuffer.h -- device-local static buffer + host-visible per-frame ring.
//
// THE PER-FRAME RING CONTRACT, stated once for the whole layer.
//
//   How many copies?  Exactly FRAMES_IN_FLIGHT (= 2). One VkBuffer, not N: the
//   slots live inside a single VMA allocation, so there is one allocation, one
//   persistent map pointer, one destroy, and the descriptor for slot i is just
//   { buffer, i * m_Stride, sizePerFrame() }.
//
//   Slot layout.  Slot i occupies [i*m_Stride, i*m_Stride + sizePerFrame()).
//   The stride is sizePerFrame() rounded up to
//       max(kind == Uniform ? limits().minUniformBufferOffsetAlignment
//                           : limits().minStorageBufferOffsetAlignment,
//           limits().nonCoherentAtomSize,
//           VkDeviceSize(1))
//   and the effective per-frame size is clamped to at least kMinBufferSize (16)
//   bytes, so a zero-element SSBO still yields a legal descriptor
//   (VUID-VkBufferCreateInfo-size-00912, VUID-VkDescriptorBufferInfo-range-00341).
//   The stride is PRIVATE -- see the next paragraph.
//
//   How a caller writes for the current frame:
//       m_CameraUBO.writeStruct(frame, cam);                    // whole struct
//       m_EntityLookupSSBO.ensureCapacity(frame, bytes);        // may realloc
//       m_EntityLookupSSBO.write(frame, data, bytes);           // sized write
//   write() computes m_Mapped + frame.index()*m_Stride + offsetInSlot, memcpys,
//   and vmaFlushAllocation()s exactly that range. There is no overload, no
//   accessor and no arithmetic anywhere in this header that can name a slot
//   other than frame.index(). The frame index is never read from a global; it
//   arrives only as a const FrameContext&.
//
//   THAT CLAIM IS ENFORCED, AND USED NOT TO BE. It was false for as long as
//   mapped() returned a bare `std::byte*` while stride() was public: the
//   perfectly ordinary-looking `ring.mapped(frame)[ring.stride() + k]` reached
//   into the OTHER frame's slot -- a slot whose fence has not been waited, which
//   is precisely the race the whole ring contract exists to prevent, and the one
//   expression the headline claim promised was unwritable. Two changes make the
//   claim true instead of aspirational:
//     * mapped(frame) returns std::span<std::byte> of exactly sizePerFrame()
//       bytes, based at the slot. Indexing past the slot is out of the span's
//       bounds, caught by libstdc++'s hardened operator[] in debug
//       (_GLIBCXX_ASSERTIONS) and by subspan()/at() everywhere.
//     * the stride is private. Nothing a caller can name yields the distance to
//       another slot, so the arithmetic cannot be reconstructed. It was only
//       ever public for diagnostics, and diagnostics can have a printf in the
//       .cpp that owns the member.
//   The remaining escape is std::span::data(), which is inherent to any mapped
//   pointer and is the same escape a memcpy already has; what is gone is the
//   *accidental* form, where the offending expression looks like normal use.
//
//   Why writing slot frame.index() needs no synchronisation: beginFrame() waited
//   that slot's fence before producing the FrameContext (see VulkanTypes.h), so
//   the GPU has provably finished every command that read it.
//
//   WHAT HAPPENS IF A CALLER WRITES THE SAME SLOT TWICE IN ONE FRAME.
//   It is legal and it is last-write-wins -- but it is almost always a bug, and
//   it is not "the second write is ignored":
//     * The CPU memcpys over the first write. No tearing, no synchronisation
//       needed, no validation error.
//     * Any vkCmdDispatch already recorded into frame.cmd() that reads this slot
//       has NOT executed yet -- command buffers execute at submit, not at record
//       time. It will therefore observe the SECOND write's bytes, not the first
//       write's, even though it was recorded before it. Two dispatches in one
//       frame cannot see two different values of the same ring slot.
//     * If two different values must coexist in one frame, they need two
//       separate offsets in the slot (write(frame, ..., offsetInSlot)) or two
//       separate rings. Ask for either; do not sequence two writes and hope.
//     * Debug builds assert on the size bound only, not on the double write --
//       write-then-overwrite is legitimate for accumulate-and-flush callers.
//
// The same one-slot-per-frame reasoning governs VulkanDescriptorSetRing in
// VulkanDescriptors.h, where a second write in one frame is NOT benign.
// =============================================================================

#include "Platform/Vulkan/VulkanTypes.h"

#include <span>
#include <type_traits>

namespace X3
{

class VulkanContext;

// -----------------------------------------------------------------------------
// Minimum size of any allocation made by this file, and of any per-frame slot.
// MERGED-3 §3.3 requires it: `stride = alignUp(max(sizePerFrame, 16), A)`, and
// the same clamp applies to VulkanBuffer's total size.
//
// It is not decoration. VUID-VkBufferCreateInfo-size-00912 makes
// vkCreateBuffer(size = 0) invalid outright, and VUID-VkDescriptorBufferInfo-
// range-00341 makes a descriptor with range = 0 invalid, so a zero-sized buffer
// cannot even be described. MERGED-4 already constructs a VulkanBuffer with
// size 0 -- an empty mesh/index/node set on a scene with no geometry is normal,
// not an error case -- so without the clamp the first empty scene is a hard
// validation failure at startup rather than a degenerate-but-legal descriptor.
// 16 rather than 1 because it also satisfies the std140/std430 base alignment of
// a vec4, so a shader that reads element 0 of an empty array reads addressable,
// aligned, zeroed memory.
//
// BOTH the constructor and ensureCapacity() apply it, on every path:
//     VulkanBuffer:     actual size   = alignUp(max(requested, kMinBufferSize), A)
//     VulkanRingBuffer: sizePerFrame() == max(requested, kMinBufferSize)
//                       stride         = alignUp(sizePerFrame(), A)
// so capacity() / sizePerFrame() report the CLAMPED value, never the requested
// one, and `ensureCapacity(frame, 0)` on a fresh buffer is a no-op rather than a
// shrink to zero.
inline constexpr VkDeviceSize kMinBufferSize = 16;

// -----------------------------------------------------------------------------
// Device-local buffer whose contents change rarely (mesh / BVH node / index /
// vertex). Uploads are staged through the context's per-frame staging arena and
// recorded into the frame command buffer, so they are ordered with the dispatch
// that reads them and need no queue wait.
//
// Ownership: owns (VkBuffer, VmaAllocation) exclusively; move-only. Both the
// destructor and a reallocating ensureCapacity() hand the old pair to
// VulkanContext::deferDestroy(VkBuffer, VmaAllocation), which stamps it with the
// current frameNumber() and frees it only once completedFrame() has caught up --
// i.e. after FRAMES_IN_FLIGHT frames retire. Nothing in this class ever calls
// vmaDestroyBuffer directly, because the buffer may still be referenced by a
// command buffer in flight. Renderer owns these by value and destroys them in
// Renderer::Shutdown(), which runs from RenderLayer::onDetach() after
// vkDeviceWaitIdle.
//
// Move semantics: exactly THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h. In
// particular `m_MeshBufferSSBO = VulkanBuffer{};` disposes of the displaced
// (VkBuffer, VmaAllocation) through ctx.deferDestroy(), never inline.
// -----------------------------------------------------------------------------
class VulkanBuffer
{
public:
	VulkanBuffer() = default;

	// `extraUsage` is ORed into the flags derived from `kind` and the mandatory
	// VK_BUFFER_USAGE_TRANSFER_DST_BIT. It exists so one allocation can serve
	// both compute (STORAGE_BUFFER) and raster (VERTEX_BUFFER / INDEX_BUFFER);
	// it defaults to 0 and is harmless when unused. Stored alongside m_Kind so
	// ensureCapacity() carries it through on reallocation.
	//
	// `size` is clamped: the allocation is max(size, kMinBufferSize) aligned up,
	// so size == 0 is legal and yields a 16-byte buffer. MERGED-4 relies on this.
	VulkanBuffer(VulkanContext& ctx, BufferKind kind, VkDeviceSize size,
	             const char* debugName, VkBufferUsageFlags extraUsage = 0);

	~VulkanBuffer();

	VulkanBuffer(VulkanBuffer&&) noexcept;
	VulkanBuffer& operator=(VulkanBuffer&&) noexcept;
	VulkanBuffer(const VulkanBuffer&)            = delete;
	VulkanBuffer& operator=(const VulkanBuffer&) = delete;

	// Grows (never shrinks) to at least max(size, kMinBufferSize),
	// geometrically (next power of two). On reallocation the old (VkBuffer,
	// VmaAllocation) go to ctx.deferDestroy() and are freed only after
	// FRAMES_IN_FLIGHT frames retire. Returns true if reallocation happened --
	// the caller's signal that any descriptor referencing this buffer must be
	// rewritten before the next dispatch. (All sets are rewritten every frame in
	// Phase 1, so the return value is for asserts and future dirty tracking.)
	//
	// EXISTING CONTENTS DO NOT SURVIVE REALLOCATION. When this returns true the
	// new allocation is UNINITIALISED: no vkCmdCopyBuffer from the old buffer is
	// recorded, and none will be added. Two reasons, both deliberate. First,
	// every caller of ensureCapacity() in this engine follows it immediately with
	// an upload() of the full new contents -- growth happens because the data
	// changed -- so a copy would be pure waste of the exact bandwidth the growth
	// was needed for. Second, a preserving copy would have to be recorded into
	// frame.cmd() and barriered against the upload that follows it, which is more
	// synchronisation than the no-op it would be preserving.
	//   The caller's obligation, therefore: if this returns true, EVERY byte the
	// shader will read must be written before the next dispatch. Do not treat
	// ensureCapacity() as "grow and keep going" for a buffer only partially
	// rewritten per frame; for that pattern, size the buffer up front.
	//   When it returns false nothing was touched and the contents are exactly as
	// they were, which is what makes the common no-growth frame free.
	bool ensureCapacity(const FrameContext& frame, VkDeviceSize size);

	// ctx.stage() + memcpy + vkCmdCopyBuffer into frame.cmd(), followed by a
	// VkBufferMemoryBarrier TRANSFER_WRITE -> SHADER_READ at
	// TRANSFER -> COMPUTE_SHADER. Asserts dstOffset + size <= capacity().
	// Never blocks, never submits, never waits a queue.
	void upload(const FrameContext& frame, const void* data,
	            VkDeviceSize size, VkDeviceSize dstOffset = 0);

	VkBuffer           handle()     const { return m_Buffer; }
	VkDeviceSize       capacity()   const { return m_Capacity; }
	BufferKind         kind()       const { return m_Kind; }
	VkBufferUsageFlags extraUsage() const { return m_ExtraUsage; }
	bool               valid()      const { return m_Buffer != VK_NULL_HANDLE; }

	VkDescriptorBufferInfo descriptor(VkDeviceSize offset = 0,
	                                  VkDeviceSize range  = VK_WHOLE_SIZE) const;

private:
	VulkanContext*     m_Ctx        = nullptr;
	VkBuffer           m_Buffer     = VK_NULL_HANDLE;
	VmaAllocation      m_Allocation = VK_NULL_HANDLE;
	VkDeviceSize       m_Capacity   = 0;
	BufferKind         m_Kind       = BufferKind::Storage;
	VkBufferUsageFlags m_ExtraUsage = 0;
	const char*        m_DebugName  = nullptr;
};

// -----------------------------------------------------------------------------
// Host-visible, persistently mapped, FRAMES_IN_FLIGHT slots inside ONE VkBuffer.
// Read the ring contract at the top of this file before using it.
//
// VMA flags: HOST_ACCESS_SEQUENTIAL_WRITE | MAPPED. Never HOST_ACCESS_RANDOM --
// that only existed for the deleted ReadData() path.
//
// Ownership: identical to VulkanBuffer. Owns one (VkBuffer, VmaAllocation);
// move-only; destructor and reallocating ensureCapacity() route the old handles
// through VulkanContext::deferDestroy(). The persistent map pointer belongs to
// VMA and is invalidated by the same deferred free, so no caller may cache the
// result of mapped() across frames.
//
// Move semantics: exactly THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h; the
// mapped pointer moves with the allocation it belongs to and the moved-from
// object's m_Mapped becomes nullptr.
// -----------------------------------------------------------------------------
class VulkanRingBuffer
{
public:
	VulkanRingBuffer() = default;
	VulkanRingBuffer(VulkanContext& ctx, BufferKind kind,
	                 VkDeviceSize sizePerFrame, const char* debugName);
	~VulkanRingBuffer();

	VulkanRingBuffer(VulkanRingBuffer&&) noexcept;
	VulkanRingBuffer& operator=(VulkanRingBuffer&&) noexcept;
	VulkanRingBuffer(const VulkanRingBuffer&)            = delete;
	VulkanRingBuffer& operator=(const VulkanRingBuffer&) = delete;

	// Same contract as VulkanBuffer::ensureCapacity, but the argument is the
	// PER-FRAME slot size; the underlying allocation is FRAMES_IN_FLIGHT * stride.
	// sizePerFrame == 0 is legal and yields a kMinBufferSize (16) byte slot.
	//
	// EXISTING CONTENTS DO NOT SURVIVE REALLOCATION, and here that is stronger
	// than for VulkanBuffer: reallocation discards EVERY slot, not just the
	// current frame's, because the whole VkBuffer is replaced. The other frame's
	// slot is not copied forward and must not be assumed to hold anything.
	//   That is harmless for the intended use and dangerous for one pattern.
	// Harmless: a ring is written in full every frame by design (§3.4, "all sets
	// are rewritten every frame"), so the slot the GPU will read is always the
	// one just written. Dangerous: a caller that writes a ring only on the frames
	// where its data changes -- an "update the light SSBO when lights move" cache
	// -- will read stale-or-garbage on the frame after a growth. Do not do that
	// with a ring; write it every frame, or use a VulkanBuffer with an explicit
	// upload.
	//   Note also that the OLD allocation stays alive, and correctly so: it is
	// deferred, and a pending command buffer may still be reading the other
	// slot from it. What is gone is the CPU's ability to see those bytes, not
	// the bytes.
	bool ensureCapacity(const FrameContext& frame, VkDeviceSize sizePerFrame);

	// memcpy into slot frame.index(), then vmaFlushAllocation over exactly that
	// range. Asserts offsetInSlot + size <= sizePerFrame().
	void write(const FrameContext& frame, const void* data,
	           VkDeviceSize size, VkDeviceSize offsetInSlot = 0);

	template <typename T>
	void writeStruct(const FrameContext& frame, const T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		write(frame, &value, sizeof(T));
	}

	// Slot frame.index(), for scatter writes, AS A BOUNDED SPAN of exactly
	// sizePerFrame() bytes based at that slot -- not a bare pointer. The caller
	// must call flush(frame) afterwards. Empty span iff !valid(). Not to be
	// cached: the next ensureCapacity() may reallocate underneath it.
	//
	// The span is the enforcement of the ring contract's headline claim; see the
	// long note at the top of this file. `mapped(frame)[k]` for k >= size() is
	// out of bounds (hardened operator[] traps it in debug builds, at()/subspan()
	// in all builds) instead of quietly landing in the other frame's slot, and
	// there is no accessor left that yields the distance between slots.
	std::span<std::byte> mapped(const FrameContext& frame);
	void                 flush(const FrameContext& frame);

	// Descriptor aimed at frame.index()'s slot:
	// offset = index*m_Stride, range = sizePerFrame() (NOT VK_WHOLE_SIZE).
	VkDescriptorBufferInfo descriptor(const FrameContext& frame) const;

	VkBuffer     handle()       const { return m_Buffer; }
	VkDeviceSize sizePerFrame() const { return m_SizePerFrame; }
	BufferKind   kind()         const { return m_Kind; }
	bool         valid()        const { return m_Buffer != VK_NULL_HANDLE; }

	// NO stride() ACCESSOR, AND THERE WILL NOT BE ONE. m_Stride is the distance
	// from one frame's slot to the next; exposing it is exactly what let a caller
	// address a slot that is not frame.index(). descriptor(), write() and
	// mapped() all need it, and all three are members. A diagnostic that wants to
	// print the stride belongs in VulkanBuffer.cpp, which can see the member.
private:
	// vmaCreateBuffer of FRAMES_IN_FLIGHT slots plus the persistent map, and the
	// only place m_Stride is computed. Shared by the constructor and a growing
	// ensureCapacity(); it does NOT dispose of the previous allocation, because
	// the constructor has none and ensureCapacity() must defer rather than
	// destroy.
	void allocateSlots(VulkanContext& ctx, VkDeviceSize sizePerFrame);

	VulkanContext* m_Ctx          = nullptr;
	VkBuffer       m_Buffer       = VK_NULL_HANDLE;
	VmaAllocation  m_Allocation   = VK_NULL_HANDLE;
	std::byte*     m_Mapped       = nullptr;
	VkDeviceSize   m_SizePerFrame = 0;
	VkDeviceSize   m_Stride       = 0;
	BufferKind     m_Kind         = BufferKind::Storage;
	const char*    m_DebugName    = nullptr;
};

}
