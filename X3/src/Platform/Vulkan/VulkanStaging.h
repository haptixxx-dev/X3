#pragma once

// =============================================================================
// VulkanStaging.h -- the per-frame staging arena.
//
// This is THE upload mechanism for the whole engine (ADJUDICATION.md, "Uploads":
// Part 3's ctx.stage() / StagingAlloc wins). VulkanContext::getUploadCommandBuffer(),
// endUploadRecording(), flushUploadsBlocking(), m_UploadPool, m_UploadCmd and
// m_UploadCmdRecording do not exist and must not be reintroduced.
//
// THE WAIT-IDLE RULE, in its CORRECTED form (ADJUDICATION.md, "Wait-idle
// post-condition" -- that ruling was revised after this header was first
// written, and this paragraph replaces what stood here before).
//
// The rule distinguishes WHEN a queue wait may happen, not WHETHER one may
// exist. The earlier, wrong version said no upload path performs a queue wait
// and endSingleTimeCommands is deleted outright. That is not implementable:
// ImGui's font upload and the context's dummy resources are created before any
// frame exists, so something must upload outside a frame, and VulkanTexture's
// out-of-frame constructor would have had no legal body.
//
//   * IN-FRAME UPLOADS NEVER WAIT. They call ctx.stage(frame, ...), memcpy into
//     the returned StagingAlloc, and record vkCmdCopyBuffer /
//     vkCmdCopyBufferToImage into frame.cmd(). They are retired by the frame
//     fence, they are ordered with the dispatch that reads them by an ordinary
//     barrier, and they submit nothing. Everything in this file serves this
//     path. The per-call vkQueueWaitIdle in the old endSingleTimeCommands is
//     what Phase 1 removes from here.
//   * OUT-OF-FRAME UPLOADS MAY WAIT, and are confined to initialisation and
//     teardown: ImGui fonts, dummy resources, and nothing else. Exactly one
//     blocking submit-and-wait helper survives to serve them --
//     VulkanContext::beginSingleTimeCommands / endSingleTimeCommands -- and it
//     asserts !m_FrameActive, so it cannot be reached from inside a frame.
//
// Final permitted wait-idle set in this directory after Phase 1:
// endSingleTimeCommands (out-of-frame uploads only), recreateSwapchain,
// cleanup, and drainDeletionQueueFully. The verification gate is NOT "grep finds
// zero wait-idle sites" -- that would fail against a design that legitimately
// has four. It is: none of them is reachable from a frame, which the
// !m_FrameActive assert enforces at runtime.
//
// A caller never touches this class directly. It calls
//
//     StagingAlloc a = ctx.stage(frame, bytes, alignment);
//
// which forwards to the one VulkanStagingArena that VulkanContext owns.
// =============================================================================

#include "Platform/Vulkan/VulkanTypes.h"

#include <array>
#include <vector>

namespace X3
{

class VulkanContext;

// -----------------------------------------------------------------------------
// One carve-out of a host-visible, persistently mapped staging block.
//
// Lifetime: valid until the end of the frame it was allocated from, and no
// longer. `ptr` is written by the CPU immediately; `buffer` + `offset` name the
// copy source of a vkCmdCopyBuffer / vkCmdCopyBufferToImage recorded into
// frame.cmd(). The block it points into is NOT destroyed or replaced while any
// frame that references it can still be in flight -- blocks live until
// VulkanContext::cleanup(). Never store a StagingAlloc past the frame.
//
// `buffer` is returned per call, not per frame, precisely because one frame's
// copies may source from different blocks once the arena has grown.
//
// THE ONE LIFETIME RULE IS NOW CHECKABLE. "Valid until the end of the frame it
// was allocated from" was, until this revision, unenforceable prose: the struct
// carried no record of WHICH frame that was, so a StagingAlloc smuggled into a
// member and reused next frame pointed at a live, mapped, correctly-aligned
// range that beginFrame() had already handed back to the bump allocator -- the
// copy would read whatever the new frame had since written there. No validation
// layer models that; it is a host-side aliasing bug that reads as intermittent
// garbage in one texture.
//   `frameNumber` is that record. It is a DEBUG-ONLY member: it exists only when
// NDEBUG is not defined, so release builds pay nothing (the struct stays 24
// bytes) while debug builds carry the stamp allocate() writes. Every consumer
// calls assertBelongsTo(frame) before using ptr/buffer/offset, which is one line
// at each of the three or four upload sites and turns "never store a
// StagingAlloc past the frame" from a comment into an abort at the first misuse.
//   Because the member is conditional, a single build configuration must be
// consistent about NDEBUG across every TU that names this struct -- which it is:
// all of them are in this one engine target and CMake sets NDEBUG per-config.
//
// NOTE ON valid(): IT IS GONE, DELIBERATELY. It used to return `ptr != nullptr`
// and it was unreachable by contract -- allocate() throws through X3_VK_CHECK on
// failure and never returns an invalid allocation, so the only object that could
// ever have failed the test was a default-constructed one, which nothing is
// allowed to keep (that is the lifetime rule above). An accessor that can only
// be false in a state the type forbids invites `if (a.valid())` guards at call
// sites, and a guard that can never fail is worse than no guard: it reads as
// though failure is a handled case, so nobody writes the handling. The failure
// mode is an exception, not a sentinel. Test nothing; use it.
// -----------------------------------------------------------------------------
struct StagingAlloc
{
	VkBuffer     buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	std::byte*   ptr    = nullptr;

#ifndef NDEBUG
	// FrameContext::number() of the frame this was carved from. Written by
	// VulkanStagingArena::allocate(); never written by anyone else.
	uint64_t frameNumber = 0;
#endif

	// Aborts in debug if this allocation belongs to a different frame than the
	// one about to record the copy. No-op, and no cost, in release.
	void assertBelongsTo([[maybe_unused]] const FrameContext& frame) const
	{
#ifndef NDEBUG
		assert(ptr != nullptr &&
		       "StagingAlloc used before it was allocated");
		assert(frameNumber == frame.number() &&
		       "StagingAlloc outlived its frame -- the arena slot it points into "
		       "has been reset and reissued; allocate a new one, never store one");
#endif
	}
};

// -----------------------------------------------------------------------------
// Bump allocator over a LIST of host-visible blocks per frame in flight.
//
// Per-frame contract, exactly:
//   * FRAMES_IN_FLIGHT independent arenas, one per frame slot. Slot i is only
//     ever carved from while frame i's fence is signalled, which beginFrame()
//     guarantees before it hands out a FrameContext (see VulkanTypes.h).
//   * reset(i) is called by VulkanContext::beginFrame() immediately after the
//     fence wait and the deletion-queue drain. It zeroes the cursor and every
//     block's `used`. It does NOT free blocks.
//   * allocate() aligns the current block's `used` up, carves if it fits,
//     otherwise advances to the next existing block, otherwise appends a new
//     block of max(kDefaultBlockSize, nextPow2(size)) and logs once at INFO.
//   * Blocks are never destroyed or replaced until destroy(). This is what makes
//     growth safe: an already-recorded vkCmdCopyBuffer names a block's VkBuffer
//     by handle, and that handle stays live.
//   * Writing twice into one frame's arena is normal and independent: every
//     allocate() returns a fresh, non-overlapping range. There is no reuse
//     within a frame.
//
// Ownership: VulkanContext owns exactly one VulkanStagingArena by value. The
// arena owns every (VkBuffer, VmaAllocation) block it created and destroys them
// in destroy(), called from VulkanContext::cleanup() AFTER
// drainDeletionQueueFully(). Blocks do NOT go through the deferred-destruction
// queue -- they outlive every frame by construction, so there is nothing to
// defer. Move-only; the destructor asserts the arena was destroy()ed while the
// device was still alive.
//
// Move semantics: THE MOVE-ASSIGNMENT CONTRACT in VulkanTypes.h, with one
// deviation stated here because this is the class that owns the exception.
// Blocks never go through deferDestroy() -- the deletion queue is for handles a
// pending command buffer may name, and by construction no block is ever
// destroyed while any frame is in flight. So move-assignment onto an arena that
// still owns blocks is not a deferred destroy but a PROGRAMMING ERROR: it would
// have to destroy blocks a recorded vkCmdCopyBuffer still names. It asserts that
// *this* owns no blocks (i.e. is default-constructed or destroy()ed) and only
// then takes the source's. In practice the only move that ever happens is the
// initial `m_Staging = VulkanStagingArena(*this);` in VulkanContext's
// constructor.
// -----------------------------------------------------------------------------
class VulkanStagingArena
{
public:
	static constexpr VkDeviceSize kDefaultBlockSize = 8ull * 1024ull * 1024ull;
	static constexpr VkDeviceSize kDefaultAlignment = 16;

	VulkanStagingArena() = default;
	explicit VulkanStagingArena(VulkanContext& ctx);
	~VulkanStagingArena();

	VulkanStagingArena(VulkanStagingArena&&) noexcept;
	VulkanStagingArena& operator=(VulkanStagingArena&&) noexcept;
	VulkanStagingArena(const VulkanStagingArena&)            = delete;
	VulkanStagingArena& operator=(const VulkanStagingArena&) = delete;

	// Carves `size` bytes out of frame.index()'s arena, growing it if needed.
	// Never returns an invalid StagingAlloc: allocation failure throws through
	// X3_VK_CHECK, which is why StagingAlloc has no valid() to test.
	// `alignment` must be a power of two.
	//
	// In debug builds it stamps the result's `frameNumber` with frame.number();
	// that stamp is the only thing that makes the "valid until the end of this
	// frame" rule checkable, via StagingAlloc::assertBelongsTo().
	StagingAlloc allocate(const FrameContext& frame, VkDeviceSize size,
	                      VkDeviceSize alignment = kDefaultAlignment);

	// Called by VulkanContext::beginFrame() only, after the fence wait for
	// `frameIndex`. Offsets only; blocks are retained.
	void reset(uint32_t frameIndex);

	// Destroys every block of every frame slot. Called by
	// VulkanContext::cleanup() while the device and allocator are still valid.
	void destroy();

	// Diagnostics: high-water reporting for the "arena grew" INFO log.
	VkDeviceSize bytesUsed(uint32_t frameIndex) const;
	VkDeviceSize bytesReserved(uint32_t frameIndex) const;
	uint32_t     blockCount(uint32_t frameIndex) const;

private:
	struct Block
	{
		VkBuffer      buffer     = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		std::byte*    mapped     = nullptr;
		VkDeviceSize  size       = 0;
		VkDeviceSize  used       = 0;
	};

	VulkanContext*                                 m_Ctx = nullptr;
	std::array<std::vector<Block>, FRAMES_IN_FLIGHT> m_Blocks{};
	std::array<uint32_t, FRAMES_IN_FLIGHT>           m_Cursor{};
};

}
