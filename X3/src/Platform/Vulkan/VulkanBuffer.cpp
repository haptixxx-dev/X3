// =============================================================================
// VulkanBuffer -- implementation.
//
// SCOPE NOTE. Only VulkanBuffer is implemented here. VulkanRingBuffer, declared
// in the same header, is Part 3's (the resource-layer migration): nothing
// instantiates it yet, and a header declaration with no definition costs nothing
// until something does. VulkanBuffer itself is implemented now because
// VulkanContext's dummyStorageBuffer()/dummyUniformBuffer() are members by
// value and cannot exist without it.
// =============================================================================

#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Core/Log.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace X3
{

namespace
{
	VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a)
	{
		return (v + a - 1) & ~(a - 1);
	}

	VkDeviceSize nextPow2(VkDeviceSize v)
	{
		VkDeviceSize p = 1;
		while (p < v) p <<= 1;
		return p;
	}

	VkBufferUsageFlags usageFor(BufferKind kind, VkBufferUsageFlags extra)
	{
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra;
		usage |= (kind == BufferKind::Uniform) ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
		                                       : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		return usage;
	}
}

VulkanBuffer::VulkanBuffer(VulkanContext& ctx, BufferKind kind, VkDeviceSize size,
                           const char* debugName, VkBufferUsageFlags extraUsage)
	: m_Ctx(&ctx)
	, m_Kind(kind)
	, m_ExtraUsage(extraUsage)
	, m_DebugName(debugName)
{
	// The clamp is not decoration: VUID-VkBufferCreateInfo-size-00912 makes
	// vkCreateBuffer(size = 0) invalid outright and VUID-VkDescriptorBufferInfo-
	// range-00341 makes a descriptor with range = 0 invalid, so a zero-sized
	// buffer cannot even be described. An empty mesh set is normal, not an error.
	const VkDeviceSize actual = alignUp(std::max(size, kMinBufferSize), kMinBufferSize);

	VkBufferCreateInfo bi{};
	bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size        = actual;
	bi.usage       = usageFor(kind, extraUsage);
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	// No HOST_ACCESS_* flag: VMA_MEMORY_USAGE_AUTO then places this in
	// device-local memory. Contents arrive through upload() (staging + copy).
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;

	X3_VK_CHECK(vmaCreateBuffer(m_Ctx->getAllocator(), &bi, &ai, &m_Buffer, &m_Allocation, nullptr));
	m_Capacity = actual;
}

VulkanBuffer::~VulkanBuffer()
{
	// Never vmaDestroyBuffer directly: a command buffer in flight may still name
	// this handle. VUID-vkDestroyBuffer-buffer-00922.
	if (m_Ctx && (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE))
		m_Ctx->deferDestroy(m_Buffer, m_Allocation);
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Buffer(other.m_Buffer)
	, m_Allocation(other.m_Allocation)
	, m_Capacity(other.m_Capacity)
	, m_Kind(other.m_Kind)
	, m_ExtraUsage(other.m_ExtraUsage)
	, m_DebugName(other.m_DebugName)
{
	other.m_Ctx        = nullptr;
	other.m_Buffer     = VK_NULL_HANDLE;
	other.m_Allocation = VK_NULL_HANDLE;
	other.m_Capacity   = 0;
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept
{
	if (this == &other)
		return *this;

	// Dispose of what we hold by exactly the route the destructor would.
	if (m_Ctx && (m_Buffer != VK_NULL_HANDLE || m_Allocation != VK_NULL_HANDLE))
		m_Ctx->deferDestroy(m_Buffer, m_Allocation);

	assert((m_Ctx == nullptr || other.m_Ctx == nullptr || m_Ctx == other.m_Ctx) &&
	       "cross-context move: the deferred destroy would go on the wrong queue");

	m_Ctx        = other.m_Ctx;
	m_Buffer     = other.m_Buffer;
	m_Allocation = other.m_Allocation;
	m_Capacity   = other.m_Capacity;
	m_Kind       = other.m_Kind;
	m_ExtraUsage = other.m_ExtraUsage;
	m_DebugName  = other.m_DebugName;

	other.m_Ctx        = nullptr;
	other.m_Buffer     = VK_NULL_HANDLE;
	other.m_Allocation = VK_NULL_HANDLE;
	other.m_Capacity   = 0;
	return *this;
}

bool VulkanBuffer::ensureCapacity(const FrameContext& frame, VkDeviceSize size)
{
	VulkanContext& ctx = frame.context();
	assert((m_Ctx == nullptr || m_Ctx == &ctx) && "buffer cannot migrate between contexts");

	const VkDeviceSize wanted = alignUp(std::max(size, kMinBufferSize), kMinBufferSize);
	if (m_Buffer != VK_NULL_HANDLE && wanted <= m_Capacity)
		return false;   // nothing touched; contents are exactly as they were

	const VkDeviceSize actual = std::max(nextPow2(wanted), kMinBufferSize);

	VkBufferCreateInfo bi{};
	bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size        = actual;
	bi.usage       = usageFor(m_Kind, m_ExtraUsage);
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;

	VkBuffer      buffer     = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	X3_VK_CHECK(vmaCreateBuffer(ctx.getAllocator(), &bi, &ai, &buffer, &allocation, nullptr));

	// EXISTING CONTENTS DO NOT SURVIVE: no vkCmdCopyBuffer is recorded, by
	// design. Every caller follows a true return with an upload() of the full new
	// contents.
	if (m_Ctx)
		m_Ctx->deferDestroy(m_Buffer, m_Allocation);

	m_Ctx        = &ctx;
	m_Buffer     = buffer;
	m_Allocation = allocation;
	m_Capacity   = actual;
	return true;
}

void VulkanBuffer::upload(const FrameContext& frame, const void* data,
                          VkDeviceSize size, VkDeviceSize dstOffset)
{
	if (size == 0)
		return;
	assert(m_Buffer != VK_NULL_HANDLE && "upload() to a buffer that was never allocated");
	assert(dstOffset + size <= m_Capacity && "upload() past the end of the buffer");

	VulkanContext& ctx = frame.context();
	StagingAlloc staging = ctx.stage(frame, size);
	staging.assertBelongsTo(frame);
	std::memcpy(staging.ptr, data, static_cast<size_t>(size));

	VkBufferCopy region{};
	region.srcOffset = staging.offset;
	region.dstOffset = dstOffset;
	region.size      = size;
	vkCmdCopyBuffer(frame.cmd(), staging.buffer, m_Buffer, 1, &region);

	VkBufferMemoryBarrier b{};
	b.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	b.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.buffer              = m_Buffer;
	b.offset              = dstOffset;
	b.size                = size;

	vkCmdPipelineBarrier(frame.cmd(),
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, nullptr, 1, &b, 0, nullptr);
}

VkDescriptorBufferInfo VulkanBuffer::descriptor(VkDeviceSize offset, VkDeviceSize range) const
{
	assert(m_Buffer != VK_NULL_HANDLE && "descriptor() of an unallocated buffer");
	VkDescriptorBufferInfo info{};
	info.buffer = m_Buffer;
	info.offset = offset;
	info.range  = range;
	return info;
}

}
