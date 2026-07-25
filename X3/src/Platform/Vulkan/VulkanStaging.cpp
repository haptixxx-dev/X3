#include "Platform/Vulkan/VulkanStaging.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Core/Log.h"

#include <algorithm>
#include <cassert>

namespace X3
{

namespace
{
	VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a)
	{
		assert(a != 0 && (a & (a - 1)) == 0 && "alignment must be a power of two");
		return (v + a - 1) & ~(a - 1);
	}

	VkDeviceSize nextPow2(VkDeviceSize v)
	{
		VkDeviceSize p = 1;
		while (p < v) p <<= 1;
		return p;
	}
}

VulkanStagingArena::VulkanStagingArena(VulkanContext& ctx)
	: m_Ctx(&ctx)
{
}

VulkanStagingArena::~VulkanStagingArena()
{
#ifndef NDEBUG
	for (const auto& blocks : m_Blocks)
		assert(blocks.empty() &&
		       "VulkanStagingArena destroyed without destroy() -- the blocks it owns "
		       "must be freed while the device and allocator are still alive");
#endif
}

VulkanStagingArena::VulkanStagingArena(VulkanStagingArena&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Blocks(std::move(other.m_Blocks))
	, m_Cursor(other.m_Cursor)
{
	other.m_Ctx = nullptr;
	for (auto& blocks : other.m_Blocks)
		blocks.clear();
	other.m_Cursor = {};
}

VulkanStagingArena& VulkanStagingArena::operator=(VulkanStagingArena&& other) noexcept
{
	if (this == &other)
		return *this;

	// THE deviation from the move-assignment contract, stated in the header:
	// blocks never go through deferDestroy(), because by construction no block is
	// ever destroyed while a frame is in flight. So move-assigning onto an arena
	// that still owns blocks is a programming error, not a deferred destroy.
#ifndef NDEBUG
	for (const auto& blocks : m_Blocks)
		assert(blocks.empty() &&
		       "move-assignment onto a VulkanStagingArena that still owns blocks");
#endif

	m_Ctx    = other.m_Ctx;
	m_Blocks = std::move(other.m_Blocks);
	m_Cursor = other.m_Cursor;

	other.m_Ctx = nullptr;
	for (auto& blocks : other.m_Blocks)
		blocks.clear();
	other.m_Cursor = {};
	return *this;
}

StagingAlloc VulkanStagingArena::allocate(const FrameContext& frame, VkDeviceSize size,
                                          VkDeviceSize alignment)
{
	assert(m_Ctx && "staging arena used before it was given a context");
	assert(alignment != 0 && (alignment & (alignment - 1)) == 0 &&
	       "alignment must be a power of two");

	const uint32_t f = frame.index();
	assert(f < FRAMES_IN_FLIGHT);

	if (size == 0)
		size = 1;   // a zero-byte carve-out has no valid ptr; give it one byte

	auto&     blocks = m_Blocks[f];
	uint32_t& cursor = m_Cursor[f];

	// Walk the existing blocks from the cursor; a block that cannot fit this
	// allocation is retired for the rest of the frame.
	while (cursor < blocks.size()) {
		Block& b = blocks[cursor];
		const VkDeviceSize off = alignUp(b.used, alignment);
		if (off + size <= b.size) {
			b.used = off + size;
			StagingAlloc a;
			a.buffer = b.buffer;
			a.offset = off;
			a.ptr    = b.mapped + off;
#ifndef NDEBUG
			a.frameNumber = frame.number();
#endif
			return a;
		}
		++cursor;
	}

	// Append a new block. Blocks are NEVER destroyed or replaced until destroy():
	// an already-recorded vkCmdCopyBuffer names a block's VkBuffer by handle, and
	// that handle must stay live.
	const VkDeviceSize blockSize = std::max(kDefaultBlockSize, nextPow2(size + alignment));

	VkBufferCreateInfo bi{};
	bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size        = blockSize;
	bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
	         | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	Block b{};
	VmaAllocationInfo allocInfo{};
	X3_VK_CHECK(vmaCreateBuffer(m_Ctx->getAllocator(), &bi, &ai,
	                            &b.buffer, &b.allocation, &allocInfo));
	b.mapped = static_cast<std::byte*>(allocInfo.pMappedData);
	b.size   = blockSize;
	b.used   = size;

	LOG_ENGINE_INFO("Staging arena (frame slot {}) grew: block {} of {} bytes",
		f, blocks.size(), blockSize);

	blocks.push_back(b);
	cursor = static_cast<uint32_t>(blocks.size()) - 1;

	StagingAlloc a;
	a.buffer = b.buffer;
	a.offset = 0;
	a.ptr    = b.mapped;
#ifndef NDEBUG
	a.frameNumber = frame.number();
#endif
	return a;
}

void VulkanStagingArena::reset(uint32_t frameIndex)
{
	assert(frameIndex < FRAMES_IN_FLIGHT);
	m_Cursor[frameIndex] = 0;
	for (auto& b : m_Blocks[frameIndex])
		b.used = 0;
}

void VulkanStagingArena::destroy()
{
	if (!m_Ctx)
		return;
	VmaAllocator allocator = m_Ctx->getAllocator();
	for (auto& blocks : m_Blocks) {
		for (auto& b : blocks) {
			if (b.buffer != VK_NULL_HANDLE)
				vmaDestroyBuffer(allocator, b.buffer, b.allocation);
		}
		blocks.clear();
	}
	m_Cursor = {};
}

VkDeviceSize VulkanStagingArena::bytesUsed(uint32_t frameIndex) const
{
	VkDeviceSize total = 0;
	for (const auto& b : m_Blocks[frameIndex])
		total += b.used;
	return total;
}

VkDeviceSize VulkanStagingArena::bytesReserved(uint32_t frameIndex) const
{
	VkDeviceSize total = 0;
	for (const auto& b : m_Blocks[frameIndex])
		total += b.size;
	return total;
}

uint32_t VulkanStagingArena::blockCount(uint32_t frameIndex) const
{
	return static_cast<uint32_t>(m_Blocks[frameIndex].size());
}

}
