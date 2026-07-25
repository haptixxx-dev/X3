#pragma once

// =============================================================================
// VulkanDescriptors.h -- descriptor set layout, the per-frame descriptor set
// ring, and the writer that fills a set.
//
// THE PER-FRAME DESCRIPTOR SET RING CONTRACT.
//
//   How many copies?  Exactly FRAMES_IN_FLIGHT (= 2) sets per (pipeline, set
//   index), sharing one VkDescriptorSetLayout, allocated in ONE
//   vkAllocateDescriptorSets with descriptorSetCount = FRAMES_IN_FLIGHT and the
//   same layout handle repeated. Allocation happens once, at pipeline-creation
//   time, never lazily inside dispatch.
//
//   How a caller writes for the current frame:
//       DescriptorWriter w(ctx, ring, frame);      // layout comes from the ring
//       w.storageBuffer(6, m_LightSSBO, frame)
//        .sampledImage(1, skyboxOrDummy)
//        .flush();
//       pipeline.dispatch(frame, sets, gx, gy, gz);
//   (`pipeline` is a VulkanComputePipeline; setLayout(uint32_t) and dispatch()
//   are declared in VulkanComputePipeline.h, which owns the layouts these rings
//   are allocated from.)
//   ring.get(frame) is the only way to name a set, so a caller cannot write a
//   set the GPU may still be reading. Sets are written once per frame in
//   Renderer::Draw, immediately before dispatch, after every buffer write and
//   image transition has been recorded. Every binding declared by the layout
//   must be written every frame -- flush() asserts completeness -- which is why
//   ctx.dummyTexture() / dummyStorageBuffer() / dummyUniformBuffer() exist: an
//   absent skybox or an empty light list still writes its binding.
//
//   Why this is the fix and not a refactor: the old code allocated ONE set and
//   called vkUpdateDescriptorSets on it every dispatch, while frame N-1's
//   command buffer was still pending with that set bound. That is
//   VUID-vkUpdateDescriptorSets-None-03047 (the layouts carry neither
//   UPDATE_AFTER_BIND nor UPDATE_UNUSED_WHILE_PENDING), and it also fed frame
//   N-1's dispatch frame N's data, because descriptors are consumed at
//   execution time. Set i is now only ever written while frame i's fence is
//   signalled.
//
//   WHAT HAPPENS IF A CALLER WRITES THE SAME SET TWICE IN ONE FRAME.
//   Unlike a ring buffer slot, this is NOT benign. Two cases:
//     * Second flush() BEFORE any vkCmdBindDescriptorSets naming that set:
//       harmless, last-write-wins. Wasteful, and the completeness assert makes
//       it pointless -- the first writer already wrote every binding.
//     * Second flush() AFTER the set has been bound and a dispatch recorded:
//       the recorded dispatch has not executed, so it will observe the SECOND
//       write, and the earlier binding is disturbed -- Vulkan requires the set
//       to be re-bound after the update before any subsequent command that uses
//       it. In practice this shows up as one dispatch silently reading another
//       dispatch's resources, or as a validation error, depending on driver.
//     * It is never legal to write a set belonging to a frame other than
//       frame.index(); that set may be bound in a pending command buffer, which
//       is the VUID above.
//   The rule, therefore: ONE DescriptorWriter per (set, frame), constructed and
//   flushed before the first bind of that set in the frame. If two dispatches in
//   one frame need different resources at the same binding, they need two set
//   rings, not two writes.
// =============================================================================

#include "Platform/Vulkan/VulkanTypes.h"

#include <array>
#include <cassert>
#include <exception>
#include <span>
#include <vector>

namespace X3
{

class VulkanContext;
class VulkanBuffer;
class VulkanRingBuffer;
class VulkanImage;
class VulkanTexture;

// -----------------------------------------------------------------------------
// Owns one VkDescriptorSetLayout and keeps its binding table, which
// DescriptorWriter uses for capacity reservation AND completeness validation.
// In Phase 3 this table becomes Slang-reflection-generated.
//
// Ownership: exclusive, move-only. The destructor calls vkDestroyDescriptorSetLayout
// directly and does NOT go through the deferred-destruction queue: a layout is
// only ever destroyed with its owning VulkanComputePipeline, and pipelines are
// destroyed in Renderer::Shutdown() after vkDeviceWaitIdle. VulkanComputePipeline
// owns these by value.
//
// Move semantics: THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h, with the same
// deviation the ownership note above describes -- the displaced
// VkDescriptorSetLayout is destroyed INLINE with vkDestroyDescriptorSetLayout,
// not deferred. That is sound only because of when it happens: a layout is
// displaced only when its owning pipeline is reassigned or destroyed, and both
// happen in Renderer::Shutdown() after vkDeviceWaitIdle. It is also why a layout
// must never be reassigned mid-frame; VulkanComputePipeline's move-assignment
// carries the same restriction for the same reason.
//
// The binding table moves with the handle. Every VulkanDescriptorSetRing and
// every DescriptorWriter holds a POINTER to a layout, so moving one out from
// under them dangles those pointers -- another reason the only legal time is
// shutdown.
// -----------------------------------------------------------------------------
class VulkanDescriptorSetLayout
{
public:
	VulkanDescriptorSetLayout() = default;
	VulkanDescriptorSetLayout(VulkanContext& ctx,
	                          std::span<const DescriptorBindingDesc> bindings);
	~VulkanDescriptorSetLayout();

	VulkanDescriptorSetLayout(VulkanDescriptorSetLayout&&) noexcept;
	VulkanDescriptorSetLayout& operator=(VulkanDescriptorSetLayout&&) noexcept;
	VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout&)            = delete;
	VulkanDescriptorSetLayout& operator=(const VulkanDescriptorSetLayout&) = delete;

	VkDescriptorSetLayout handle() const { return m_Layout; }
	bool                  valid()  const { return m_Layout != VK_NULL_HANDLE; }

	std::span<const DescriptorBindingDesc> bindings() const { return m_Bindings; }

	// nullptr if the binding is not declared by this layout.
	const DescriptorBindingDesc* find(uint32_t binding) const;

	// Sum of `count` over bindings of image / buffer descriptor types.
	// DescriptorWriter reserves exactly these, which is why it takes a layout
	// rather than a caller-supplied maxWrites/maxImageInfos pair.
	uint32_t imageDescriptorCount()  const;
	uint32_t bufferDescriptorCount() const;

private:
	VulkanContext*                     m_Ctx    = nullptr;
	VkDescriptorSetLayout              m_Layout = VK_NULL_HANDLE;
	std::vector<DescriptorBindingDesc> m_Bindings;
};

// -----------------------------------------------------------------------------
// FRAMES_IN_FLIGHT descriptor sets sharing one layout. Read the ring contract at
// the top of this file.
//
// Ownership: owns FRAMES_IN_FLIGHT VkDescriptorSets allocated from the context's
// shared m_DescriptorPool. The destructor calls
// VulkanContext::deferFreeDescriptorSets(m_Sets), which vkFreeDescriptorSets
// them only after FRAMES_IN_FLIGHT frames retire -- freeing a set that a pending
// command buffer still has bound is undefined behaviour. This is what makes
// VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT on that pool load-bearing.
// Move-only. Renderer owns these by value, one per (pipeline, set index).
//
// It also REMEMBERS THE LAYOUT IT WAS ALLOCATED FROM, as a borrowed pointer.
// That is what lets DescriptorWriter take a (ring, frame) pair instead of an
// independent (layout, set) pair -- see the writer's constructor comments. The
// layout is owned by the VulkanComputePipeline that created it and outlives every
// ring allocated from it (rings die in Renderer::Shutdown() with the pipelines).
//
// Move semantics: THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h. The displaced
// sets go to VulkanContext::deferFreeDescriptorSets(), never to
// vkFreeDescriptorSets directly; the layout pointer moves with them.
// -----------------------------------------------------------------------------
class VulkanDescriptorSetRing
{
public:
	VulkanDescriptorSetRing() = default;
	VulkanDescriptorSetRing(VulkanContext& ctx, const VulkanDescriptorSetLayout& layout);
	~VulkanDescriptorSetRing();

	VulkanDescriptorSetRing(VulkanDescriptorSetRing&&) noexcept;
	VulkanDescriptorSetRing& operator=(VulkanDescriptorSetRing&&) noexcept;
	VulkanDescriptorSetRing(const VulkanDescriptorSetRing&)            = delete;
	VulkanDescriptorSetRing& operator=(const VulkanDescriptorSetRing&) = delete;

	// The only way to name a set. There is no get(uint32_t) and there will not
	// be one: an index that is not frame.index() is always a bug.
	VkDescriptorSet get(const FrameContext& frame) const { return m_Sets[frame.index()]; }

	// The layout these sets were allocated from. Never null on a valid ring;
	// borrowed, owned by the VulkanComputePipeline that declared it.
	const VulkanDescriptorSetLayout& layout() const
	{
		assert(m_Layout != nullptr && "descriptor set ring was never allocated");
		return *m_Layout;
	}

	bool valid() const { return m_Sets[0] != VK_NULL_HANDLE; }

private:
	VulkanContext*                                m_Ctx    = nullptr;
	const VulkanDescriptorSetLayout*              m_Layout = nullptr;
	std::array<VkDescriptorSet, FRAMES_IN_FLIGHT> m_Sets{};
};

// -----------------------------------------------------------------------------
// Accumulates the writes for ONE descriptor set and flushes them in a single
// vkUpdateDescriptorSets. Owns its VkDescriptor*Info storage, which is why the
// old shader class's keep-alive info maps disappear.
//
// Ownership: owns nothing Vulkan-side. It is a scratch object -- stack-allocate
// one per set per frame, flush it, let it die. It borrows the layout and the
// context by pointer and must not outlive either, which a stack lifetime inside
// Renderer::Draw guarantees.
//
// Contiguous-info-block invariant (this is why the constructor takes the layout):
//   1. No reallocation. The constructor reserves layout.imageDescriptorCount()
//      image infos and layout.bufferDescriptorCount() buffer infos, both derived
//      from sum(binding.count) over the layout. Every append asserts
//      size() < capacity(). A caller-supplied maxWrites could disagree with the
//      layout, and a 128-element array binding would then silently reallocate
//      the vector, dangling every previously taken pImageInfo.
//   2. No interleaving. The `count` infos for one binding are appended by a
//      single uninterrupted loop, so [infoBase, infoBase + count) is contiguous
//      by construction.
//   3. Deferred pointer resolution. pImageInfo / pBufferInfo are computed in
//      flush() as data() + infoBase, never captured at append time.
// -----------------------------------------------------------------------------
class DescriptorWriter
{
public:
	// THE PREFERRED CONSTRUCTOR, and the one Renderer::Draw uses. It takes the
	// RING, so the layout and the destination set cannot disagree: the layout is
	// ring.layout() and the set is ring.get(frame), both read out of one object
	// that allocated the second from the first.
	//
	// It exists because the three-argument form below takes `layout` and `dst` as
	// INDEPENDENT arguments, and a mismatched pair compiles:
	//
	//     DescriptorWriter w(ctx, pipeline.setLayout(2), rings[0].get(frame));
	//                                             ^                ^ set 0
	//
	// That is not a hypothetical typo; §3.7.6 writes three of these in a row,
	// differing only by the index, and the failure is silent in the worst way.
	// flush()'s completeness assert validates against the layout it was GIVEN, so
	// it would confirm that every binding of set 2's layout was written -- into
	// set 0's descriptor set. If the two layouts happen to be
	// binding-compatible, validation says nothing either, and the dispatch reads
	// the wrong resources. The assert does not just fail to catch the error, it
	// actively reports success.
	//
	// This form makes the pair unrepresentable rather than merely discouraged.
	// It does not contradict ADJUDICATION.md: the canonical three-argument form
	// survives below, unchanged, and this one is defined in terms of it.
	DescriptorWriter(VulkanContext& ctx,
	                 const VulkanDescriptorSetRing& ring,
	                 const FrameContext& frame);

	// The canonical constructor of ADJUDICATION.md ("Take Part 3's form"),
	// retained verbatim. Capacity is derived from the layout, NOT from a
	// caller-supplied number; the four-argument maxWrites/maxImageInfos form is
	// void.
	//
	// USE THIS ONE ONLY WHERE THERE IS NO RING: the migration bridge of §3.8,
	// which writes sets that still come from the old code, and any future path
	// that owns a bare VkDescriptorSet. It cannot be made mismatch-proof, and
	// this is the honest statement of why: a VkDescriptorSet is an opaque handle
	// with no queryable layout. Vulkan records the layout at allocation time and
	// exposes no way to read it back -- there is no vkGetDescriptorSetLayout --
	// so neither this constructor nor flush() can verify that `dst` was allocated
	// from `layout`. Nothing short of tracking the association ourselves closes
	// it, which is precisely what VulkanDescriptorSetRing does and why the
	// constructor above is preferred everywhere a ring exists. When this form is
	// unavoidable, the layout and the set must come from adjacent lines of the
	// same expression, never from two indexed lookups.
	DescriptorWriter(VulkanContext& ctx,
	                 const VulkanDescriptorSetLayout& layout,
	                 VkDescriptorSet dst);

	// A WRITER THAT WAS NEVER FLUSHED IS A DIAGNOSABLE ERROR, NOT SILENCE.
	//
	// The completeness assert used to live only inside flush(), so the one
	// failure it could not report was flush() never being called: an early
	// `return` between the last binding and the flush, or a `continue` in the
	// per-set loop of §3.7.6. The descriptor set then keeps whatever it held --
	// which, on a FRAMES_IN_FLIGHT = 2 ring, is exactly two frames' worth of
	// stale resources: last-frame-but-one's buffer handles, possibly already
	// freed by the deletion queue, and last-frame-but-one's image views. The
	// dispatch reads them, and the symptom is a one-frame-stale or corrupt image
	// with no validation output at all.
	//
	// The destructor closes it. Every write path funnels into flush(), flush()
	// sets m_Flushed as its final act, and this fires if the object dies without
	// it. The uncaught_exceptions() guard is there so that unwinding out of a
	// half-built writer -- X3_VK_CHECK throwing from an ensureCapacity() in the
	// middle of a write chain -- reports the original exception rather than
	// aborting on a secondary assert that is merely a consequence of it.
	~DescriptorWriter()
	{
		assert((m_Flushed || std::uncaught_exceptions() > 0) &&
		       "DescriptorWriter destroyed without flush() -- the descriptor set "
		       "still holds the resources written FRAMES_IN_FLIGHT frames ago");
	}

	DescriptorWriter(const DescriptorWriter&)            = delete;
	DescriptorWriter& operator=(const DescriptorWriter&) = delete;
	DescriptorWriter(DescriptorWriter&&)                 = delete;
	DescriptorWriter& operator=(DescriptorWriter&&)      = delete;

	DescriptorWriter& uniformBuffer(uint32_t binding, const VulkanRingBuffer& buffer, const FrameContext& frame);
	DescriptorWriter& uniformBuffer(uint32_t binding, const VulkanBuffer& buffer);
	DescriptorWriter& storageBuffer(uint32_t binding, const VulkanRingBuffer& buffer, const FrameContext& frame);
	DescriptorWriter& storageBuffer(uint32_t binding, const VulkanBuffer& buffer);
	DescriptorWriter& storageImage (uint32_t binding, const VulkanImage& image);
	DescriptorWriter& sampledImage (uint32_t binding, const VulkanTexture& texture);

	// Array write, owned by this layer -- Phase 2 consumes it and must not
	// define its own. Writes layout.find(binding)->count descriptors in ONE
	// VkWriteDescriptorSet; textures.size() must equal that count exactly,
	// because there is no PARTIALLY_BOUND and an unwritten element is undefined
	// behaviour on access. A null or !valid() element becomes
	// ctx.dummyTexture().descriptor(); there are no holes.
	DescriptorWriter& sampledImageArray(uint32_t binding,
	                                    std::span<const VulkanTexture* const> textures);

	// Migration bridge only. Deleted in the final cleanup commit.
	DescriptorWriter& raw(uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo& info);
	DescriptorWriter& raw(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo& info);

	// Resolves the deferred info pointers, asserts (debug only) that every
	// binding in the layout was written exactly once with the declared type and
	// descriptorCount, then issues one vkUpdateDescriptorSets. No arguments:
	// the destination set and the layout were fixed at construction.
	//
	// ITS LAST STATEMENT IS `m_Flushed = true;`. That is not bookkeeping -- it is
	// what the destructor assert above tests, and it must be set on the success
	// path only, after vkUpdateDescriptorSets has been issued. Calling flush()
	// twice on one writer asserts: the first call consumed the pending writes and
	// the second would issue an empty, incomplete update.
	void flush();

private:
	struct PendingWrite
	{
		uint32_t         binding  = 0;
		VkDescriptorType type     = VK_DESCRIPTOR_TYPE_MAX_ENUM;
		uint32_t         count    = 0;
		uint32_t         infoBase = 0;   // index into m_ImageInfos or m_BufferInfos
		bool             isImage  = false;
	};

	VulkanContext*                      m_Ctx    = nullptr;
	VkDevice                            m_Device = VK_NULL_HANDLE;
	const VulkanDescriptorSetLayout*    m_Layout = nullptr;
	VkDescriptorSet                     m_Dst    = VK_NULL_HANDLE;
	std::vector<VkDescriptorBufferInfo> m_BufferInfos;
	std::vector<VkDescriptorImageInfo>  m_ImageInfos;
	std::vector<PendingWrite>           m_Pending;
	bool                                m_Flushed = false;   // set by flush(); read by ~DescriptorWriter
};

}
