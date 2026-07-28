// =============================================================================
// VulkanBuffer and VulkanRingBuffer -- implementation.
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

// =============================================================================
// VulkanRingBuffer
//
// FRAMES_IN_FLIGHT slots inside ONE VkBuffer, host-visible and persistently
// mapped. Slot i starts at i * m_Stride, where m_Stride is sizePerFrame() rounded
// up to the device's minimum offset alignment for this buffer kind -- the
// descriptor's offset must satisfy that alignment, so the stride is where it has
// to be applied.
// =============================================================================

namespace
{
	// Offset alignment the descriptor for a slot must satisfy. Read from the
	// context's CACHED limits, not from a per-call
	// vkGetPhysicalDeviceProperties -- that is a syscall-shaped mistake in an
	// allocation path.
	VkDeviceSize offsetAlignmentFor(VulkanContext& ctx, BufferKind kind)
	{
		const VkPhysicalDeviceLimits& limits = ctx.limits();
		const VkDeviceSize a = (kind == BufferKind::Uniform)
			? limits.minUniformBufferOffsetAlignment
			: limits.minStorageBufferOffsetAlignment;
		// A reported alignment of 0 means "no requirement"; alignUp would divide
		// the world by zero on it.
		return a == 0 ? 1 : a;
	}
}

VulkanRingBuffer::VulkanRingBuffer(VulkanContext& ctx, BufferKind kind,
                                   VkDeviceSize sizePerFrame, const char* debugName)
	: m_Ctx(&ctx)
	, m_Kind(kind)
	, m_DebugName(debugName)
{
	// Allocation itself is ensureCapacity()'s job, but the constructor has no
	// FrameContext to hand it -- and does not need one, because nothing is
	// deferred on a first allocation. allocateSlots() is the shared body.
	allocateSlots(ctx, sizePerFrame);
}

void VulkanRingBuffer::allocateSlots(VulkanContext& ctx, VkDeviceSize sizePerFrame)
{
	// The clamp is applied to the SLOT, so sizePerFrame() reports the clamped
	// value and a zero-element SSBO still yields a legal descriptor
	// (VUID-VkDescriptorBufferInfo-range-00341).
	const VkDeviceSize slot   = std::max(sizePerFrame, kMinBufferSize);
	const VkDeviceSize stride = alignUp(slot, offsetAlignmentFor(ctx, m_Kind));

	VkBufferCreateInfo bi{};
	bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size        = stride * FRAMES_IN_FLIGHT;
	bi.usage       = usageFor(m_Kind, 0);
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	// SEQUENTIAL_WRITE, never HOST_ACCESS_RANDOM: that flag only existed for the
	// deleted ReadData() path and it costs write-combining on some drivers.
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
	         | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VkBuffer          buffer     = VK_NULL_HANDLE;
	VmaAllocation     allocation = VK_NULL_HANDLE;
	VmaAllocationInfo allocInfo{};
	X3_VK_CHECK(vmaCreateBuffer(ctx.getAllocator(), &bi, &ai, &buffer, &allocation, &allocInfo));

	m_Buffer       = buffer;
	m_Allocation   = allocation;
	m_Mapped       = static_cast<std::byte*>(allocInfo.pMappedData);
	m_SizePerFrame = slot;
	m_Stride       = stride;

	assert(m_Mapped != nullptr && "ring buffer allocation is not host-mapped");
}

VulkanRingBuffer::~VulkanRingBuffer()
{
	if (m_Ctx && m_Buffer != VK_NULL_HANDLE)
		m_Ctx->deferDestroy(m_Buffer, m_Allocation);
}

VulkanRingBuffer::VulkanRingBuffer(VulkanRingBuffer&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Buffer(other.m_Buffer)
	, m_Allocation(other.m_Allocation)
	, m_Mapped(other.m_Mapped)
	, m_SizePerFrame(other.m_SizePerFrame)
	, m_Stride(other.m_Stride)
	, m_Kind(other.m_Kind)
	, m_DebugName(other.m_DebugName)
{
	other.m_Ctx          = nullptr;
	other.m_Buffer       = VK_NULL_HANDLE;
	other.m_Allocation   = VK_NULL_HANDLE;
	other.m_Mapped       = nullptr;
	other.m_SizePerFrame = 0;
	other.m_Stride       = 0;
}

VulkanRingBuffer& VulkanRingBuffer::operator=(VulkanRingBuffer&& other) noexcept
{
	if (this == &other)
		return *this;

	assert((m_Ctx == nullptr || other.m_Ctx == nullptr || m_Ctx == other.m_Ctx) &&
	       "cross-context move: the deferred destroy would go on the wrong queue");

	if (m_Ctx && m_Buffer != VK_NULL_HANDLE)
		m_Ctx->deferDestroy(m_Buffer, m_Allocation);

	m_Ctx          = other.m_Ctx;
	m_Buffer       = other.m_Buffer;
	m_Allocation   = other.m_Allocation;
	m_Mapped       = other.m_Mapped;
	m_SizePerFrame = other.m_SizePerFrame;
	m_Stride       = other.m_Stride;
	m_Kind         = other.m_Kind;
	m_DebugName    = other.m_DebugName;

	other.m_Ctx          = nullptr;
	other.m_Buffer       = VK_NULL_HANDLE;
	other.m_Allocation   = VK_NULL_HANDLE;
	other.m_Mapped       = nullptr;
	other.m_SizePerFrame = 0;
	other.m_Stride       = 0;
	return *this;
}

bool VulkanRingBuffer::ensureCapacity(const FrameContext& frame, VkDeviceSize sizePerFrame)
{
	VulkanContext& ctx = frame.context();
	assert((m_Ctx == nullptr || m_Ctx == &ctx) && "ring buffer cannot migrate between contexts");

	const VkDeviceSize wanted = std::max(sizePerFrame, kMinBufferSize);
	if (m_Buffer != VK_NULL_HANDLE && wanted <= m_SizePerFrame)
		return false;   // nothing touched

	// Grow in powers of two so a scene that adds one light per frame does not
	// reallocate every frame.
	const VkDeviceSize slot = std::max(nextPow2(wanted), kMinBufferSize);

	// EVERY slot is discarded, not just this frame's -- the whole VkBuffer is
	// replaced. That is safe only because a ring is written in full every frame by
	// design; a caller that writes only on change would read garbage on the frame
	// after a growth. The OLD allocation stays alive on the deferred queue, and
	// correctly so: a pending command buffer may still be reading the other slot.
	VkBuffer      oldBuffer     = m_Buffer;
	VmaAllocation oldAllocation = m_Allocation;

	m_Ctx = &ctx;
	allocateSlots(ctx, slot);

	if (oldBuffer != VK_NULL_HANDLE)
		ctx.deferDestroy(oldBuffer, oldAllocation);

	return true;
}

void VulkanRingBuffer::write(const FrameContext& frame, const void* data,
                             VkDeviceSize size, VkDeviceSize offsetInSlot)
{
	assert(m_Buffer != VK_NULL_HANDLE && "write() to a ring buffer that was never allocated");
	assert(offsetInSlot + size <= m_SizePerFrame && "write() past the end of the frame's slot");
	if (size == 0)
		return;

	// THE FRAME-SLOT INVARIANT in one line: the destination is frame.index() and
	// nothing else. beginFrame() waited this slot's fence, so the GPU has provably
	// finished with it and no further synchronisation is needed for the memcpy.
	const VkDeviceSize base = VkDeviceSize(frame.index()) * m_Stride + offsetInSlot;
	std::memcpy(m_Mapped + base, data, static_cast<size_t>(size));

	// Flush exactly the written range. VMA no-ops this on coherent memory, so it
	// is not worth branching on the memory property flags here.
	vmaFlushAllocation(m_Ctx->getAllocator(), m_Allocation, base, size);
}

std::span<std::byte> VulkanRingBuffer::mapped(const FrameContext& frame)
{
	if (m_Buffer == VK_NULL_HANDLE)
		return {};
	// BOUNDED to exactly this frame's slot. The bound is the enforcement of the
	// ring contract: indexing past it is out of bounds rather than quietly landing
	// in the other frame's slot.
	return std::span<std::byte>(m_Mapped + VkDeviceSize(frame.index()) * m_Stride,
	                            static_cast<size_t>(m_SizePerFrame));
}

void VulkanRingBuffer::flush(const FrameContext& frame)
{
	if (m_Buffer == VK_NULL_HANDLE)
		return;
	vmaFlushAllocation(m_Ctx->getAllocator(), m_Allocation,
	                   VkDeviceSize(frame.index()) * m_Stride, m_SizePerFrame);
}

VkDescriptorBufferInfo VulkanRingBuffer::descriptor(const FrameContext& frame) const
{
	assert(m_Buffer != VK_NULL_HANDLE && "descriptor() of an unallocated ring buffer");
	VkDescriptorBufferInfo info{};
	info.buffer = m_Buffer;
	info.offset = VkDeviceSize(frame.index()) * m_Stride;
	// sizePerFrame(), NOT VK_WHOLE_SIZE: a whole-size range would let the shader
	// read straight through into the other frame's slot.
	info.range  = m_SizePerFrame;
	return info;
}

}
