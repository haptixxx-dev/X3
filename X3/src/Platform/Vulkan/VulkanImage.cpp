#include "Platform/Vulkan/VulkanImage.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Core/Log.h"

#include <cassert>
#include <cstring>

namespace X3
{

namespace
{
	uint32_t bytesPerPixel(VkFormat format)
	{
		switch (format) {
			case VK_FORMAT_R8_UNORM:
			case VK_FORMAT_R8_SRGB:              return 1;
			case VK_FORMAT_R8G8_UNORM:           return 2;
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB:
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB:        return 4;
			case VK_FORMAT_R16G16B16A16_SFLOAT:  return 8;
			case VK_FORMAT_R32G32B32A32_SFLOAT:  return 16;
			default:
				assert(false && "bytesPerPixel: unhandled format");
				return 4;
		}
	}
}

// =============================================================================
// VulkanImage
// =============================================================================

VulkanImage::VulkanImage(VulkanContext& ctx, const ImageDesc& desc)
{
	// Deliberately submits NOTHING: no beginSingleTimeCommands, no
	// vkQueueWaitIdle, no initial transition. The image is left in UNDEFINED and
	// the first transition(frame, GENERAL, ...) inside the frame command buffer
	// does the work; UNDEFINED -> GENERAL is always legal.
	m_Ctx = &ctx;
	allocate(desc);
}

void VulkanImage::allocate(const ImageDesc& desc)
{
	assert(m_Ctx && "VulkanImage::allocate without a context");
	assert(desc.width > 0 && desc.height > 0 && "zero-sized image");

	VkImageCreateInfo ii{};
	ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ii.imageType     = VK_IMAGE_TYPE_2D;
	ii.format        = desc.format;
	ii.extent        = { desc.width, desc.height, 1 };
	ii.mipLevels     = desc.mipLevels ? desc.mipLevels : 1;
	ii.arrayLayers   = 1;
	ii.samples       = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
	ii.usage         = desc.usage;
	ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;

	X3_VK_CHECK(vmaCreateImage(m_Ctx->getAllocator(), &ii, &ai, &m_Image, &m_Allocation, nullptr));

	VkImageViewCreateInfo vi{};
	vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image    = m_Image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format   = desc.format;
	vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, ii.mipLevels, 0, 1 };

	X3_VK_CHECK(vkCreateImageView(m_Ctx->getDevice(), &vi, nullptr, &m_View));

	m_Extent     = { desc.width, desc.height };
	m_Format     = desc.format;
	m_Layout     = VK_IMAGE_LAYOUT_UNDEFINED;
	m_LastAccess = 0;
	m_LastStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	++m_Generation;
}

VulkanImage::~VulkanImage()
{
	if (m_Ctx && (m_Image != VK_NULL_HANDLE || m_View != VK_NULL_HANDLE))
		m_Ctx->deferDestroy(m_Image, m_Allocation, m_View);
}

VulkanImage::VulkanImage(VulkanImage&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Image(other.m_Image)
	, m_Allocation(other.m_Allocation)
	, m_View(other.m_View)
	, m_Extent(other.m_Extent)
	, m_Format(other.m_Format)
	, m_Layout(other.m_Layout)
	, m_LastAccess(other.m_LastAccess)
	, m_LastStage(other.m_LastStage)
	, m_Id(other.m_Id)
	, m_Generation(other.m_Generation)
{
	other.m_Ctx        = nullptr;
	other.m_Image      = VK_NULL_HANDLE;
	other.m_Allocation = VK_NULL_HANDLE;
	other.m_View       = VK_NULL_HANDLE;
	other.m_Extent     = {};
	other.m_Format     = VK_FORMAT_UNDEFINED;
	other.m_Layout     = VK_IMAGE_LAYOUT_UNDEFINED;
	other.m_LastAccess = 0;
	other.m_LastStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	other.m_Generation = 0;
	// Identity does NOT survive as a duplicate: the source gets a fresh id so no
	// two live objects share a cache key.
	other.m_Id         = nextResourceId();
}

VulkanImage& VulkanImage::operator=(VulkanImage&& other) noexcept
{
	if (this == &other)
		return *this;

	if (m_Ctx && (m_Image != VK_NULL_HANDLE || m_View != VK_NULL_HANDLE))
		m_Ctx->deferDestroy(m_Image, m_Allocation, m_View);

	assert((m_Ctx == nullptr || other.m_Ctx == nullptr || m_Ctx == other.m_Ctx) &&
	       "cross-context move: the deferred destroy would go on the wrong queue");

	m_Ctx        = other.m_Ctx;
	m_Image      = other.m_Image;
	m_Allocation = other.m_Allocation;
	m_View       = other.m_View;
	m_Extent     = other.m_Extent;
	m_Format     = other.m_Format;
	m_Layout     = other.m_Layout;
	m_LastAccess = other.m_LastAccess;
	m_LastStage  = other.m_LastStage;
	m_Id         = other.m_Id;
	m_Generation = other.m_Generation;

	other.m_Ctx        = nullptr;
	other.m_Image      = VK_NULL_HANDLE;
	other.m_Allocation = VK_NULL_HANDLE;
	other.m_View       = VK_NULL_HANDLE;
	other.m_Extent     = {};
	other.m_Format     = VK_FORMAT_UNDEFINED;
	other.m_Layout     = VK_IMAGE_LAYOUT_UNDEFINED;
	other.m_LastAccess = 0;
	other.m_LastStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	other.m_Generation = 0;
	other.m_Id         = nextResourceId();
	return *this;
}

void VulkanImage::recreate(const FrameContext& frame, const ImageDesc& desc)
{
	VulkanContext& ctx = frame.context();
	assert((m_Ctx == nullptr || m_Ctx == &ctx) && "an image cannot migrate between contexts");

	if (m_Image != VK_NULL_HANDLE || m_View != VK_NULL_HANDLE)
		ctx.deferDestroy(m_Image, m_Allocation, m_View);

	m_Ctx        = &ctx;
	m_Image      = VK_NULL_HANDLE;
	m_Allocation = VK_NULL_HANDLE;
	m_View       = VK_NULL_HANDLE;

	// id() is UNCHANGED; generation() is incremented by allocate().
	allocate(desc);
}

void VulkanImage::transition(const FrameContext& frame, VkImageLayout newLayout,
                             VkAccessFlags dstAccess, VkPipelineStageFlags dstStage)
{
	assert(m_Image != VK_NULL_HANDLE && "transition() of an unallocated image");

	// THE BARRIER ELISION RULE (VulkanTypes.h): read-after-read with an
	// already-covered mask is the ONLY free case. Anything touching a write --
	// on either side -- records.
	if (newLayout == m_Layout
	    && !isWriteAccess(m_LastAccess)
	    && !isWriteAccess(dstAccess)
	    && (dstAccess & ~m_LastAccess) == 0) {
		return;
	}

	VkImageMemoryBarrier b{};
	b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout           = m_Layout;
	b.newLayout           = newLayout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image               = m_Image;
	b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
	b.srcAccessMask       = m_LastAccess;
	b.dstAccessMask       = dstAccess;

	vkCmdPipelineBarrier(frame.cmd(), m_LastStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);

	m_Layout     = newLayout;
	m_LastAccess = dstAccess;
	m_LastStage  = dstStage;
}

void VulkanImage::barrier(const FrameContext& frame,
                          VkAccessFlags dstAccess, VkPipelineStageFlags dstStage)
{
	assert(m_Image != VK_NULL_HANDLE && "barrier() on an unallocated image");

	VkImageMemoryBarrier b{};
	b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout           = m_Layout;
	b.newLayout           = m_Layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image               = m_Image;
	b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
	b.srcAccessMask       = m_LastAccess;
	b.dstAccessMask       = dstAccess;

	vkCmdPipelineBarrier(frame.cmd(), m_LastStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);

	m_LastAccess = dstAccess;
	m_LastStage  = dstStage;
}

// =============================================================================
// VulkanTexture
// =============================================================================

void VulkanTexture::create(VulkanContext& ctx, const TextureDesc& desc)
{
	assert(desc.mipLevels == 1 &&
	       "Phase 1 pins TextureDesc::mipLevels to 1: nothing in this layer "
	       "generates mip content, so a second level would sample uninitialised "
	       "device memory");

	m_Ctx = &ctx;

	VkImageCreateInfo ii{};
	ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ii.imageType     = VK_IMAGE_TYPE_2D;
	ii.format        = desc.format;
	ii.extent        = { desc.width, desc.height, 1 };
	ii.mipLevels     = 1;
	ii.arrayLayers   = 1;
	ii.samples       = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
	ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;

	X3_VK_CHECK(vmaCreateImage(ctx.getAllocator(), &ii, &ai, &m_Image, &m_Allocation, nullptr));

	VkImageViewCreateInfo vi{};
	vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image    = m_Image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format   = desc.format;
	vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	X3_VK_CHECK(vkCreateImageView(ctx.getDevice(), &vi, nullptr, &m_View));

	// BORROWED, never owned. The cache destroys it in VulkanContext::cleanup(),
	// after everything that borrowed from it. Never vkDestroySampler here.
	m_Sampler = ctx.getSampler(desc.sampler);
	m_Extent  = { desc.width, desc.height };
}

void VulkanTexture::recordUpload(VkCommandBuffer cmd, VkBuffer srcBuffer, VkDeviceSize srcOffset)
{
	VkImageMemoryBarrier toDst{};
	toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
	toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toDst.image               = m_Image;
	toDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	toDst.srcAccessMask       = 0;
	toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &toDst);

	VkBufferImageCopy region{};
	region.bufferOffset      = srcOffset;
	region.bufferRowLength   = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageOffset       = { 0, 0, 0 };
	region.imageExtent       = { m_Extent.width, m_Extent.height, 1 };
	vkCmdCopyBufferToImage(cmd, srcBuffer, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// dstStage names COMPUTE as well as FRAGMENT: the skybox is sampled from a
	// COMPUTE shader, and a read issued from COMPUTE_SHADER is not in the
	// visibility scope of a barrier whose dstStageMask names only FRAGMENT_SHADER
	// -- a read-after-write hazard, not a layout error.
	VkImageMemoryBarrier toRead{};
	toRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toRead.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toRead.image               = m_Image;
	toRead.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	toRead.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	toRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &toRead);
}

VulkanTexture::VulkanTexture(VulkanContext& ctx, const FrameContext& frame,
                             const TextureDesc& desc, const void* pixels)
{
	// THE IN-FRAME CONSTRUCTOR. It never waits: stage() + frame.cmd(), retired by
	// the frame fence.
	create(ctx, desc);

	const VkDeviceSize bytes =
		VkDeviceSize(desc.width) * desc.height * bytesPerPixel(desc.format);

	StagingAlloc staging = ctx.stage(frame, bytes);
	staging.assertBelongsTo(frame);
	std::memcpy(staging.ptr, pixels, static_cast<size_t>(bytes));

	recordUpload(frame.cmd(), staging.buffer, staging.offset);
}

VulkanTexture::VulkanTexture(VulkanContext& ctx, const TextureDesc& desc, const void* pixels)
{
	// THE OUT-OF-FRAME CONSTRUCTOR. It BLOCKS, and that is legal, and it is legal
	// only here: ADJUDICATION.md's corrected wait-idle ruling confines waiting
	// uploads to initialisation and teardown (ImGui fonts, the dummy resources).
	create(ctx, desc);

	const VkDeviceSize bytes =
		VkDeviceSize(desc.width) * desc.height * bytesPerPixel(desc.format);

	VkBufferCreateInfo bi{};
	bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size        = bytes;
	bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
	         | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VkBuffer          staging      = VK_NULL_HANDLE;
	VmaAllocation     stagingAlloc = VK_NULL_HANDLE;
	VmaAllocationInfo stagingInfo{};
	X3_VK_CHECK(vmaCreateBuffer(ctx.getAllocator(), &bi, &ai, &staging, &stagingAlloc, &stagingInfo));
	std::memcpy(stagingInfo.pMappedData, pixels, static_cast<size_t>(bytes));

	VkCommandBuffer cmd = ctx.beginSingleTimeCommands();
	recordUpload(cmd, staging, 0);
	ctx.endSingleTimeCommands(cmd);

	// The queue is idle by the time endSingleTimeCommands returns, so this one
	// destroy may be immediate rather than deferred.
	vmaDestroyBuffer(ctx.getAllocator(), staging, stagingAlloc);
}

VulkanTexture::~VulkanTexture()
{
	if (m_Ctx && (m_Image != VK_NULL_HANDLE || m_View != VK_NULL_HANDLE))
		m_Ctx->deferDestroy(m_Image, m_Allocation, m_View);
	// m_Sampler is borrowed from the context's cache; do NOT destroy it.
}

VulkanTexture::VulkanTexture(VulkanTexture&& other) noexcept
	: m_Ctx(other.m_Ctx)
	, m_Image(other.m_Image)
	, m_Allocation(other.m_Allocation)
	, m_View(other.m_View)
	, m_Sampler(other.m_Sampler)
	, m_Extent(other.m_Extent)
	, m_Id(other.m_Id)
{
	other.m_Ctx        = nullptr;
	other.m_Image      = VK_NULL_HANDLE;
	other.m_Allocation = VK_NULL_HANDLE;
	other.m_View       = VK_NULL_HANDLE;
	other.m_Sampler    = VK_NULL_HANDLE;
	other.m_Extent     = {};
	other.m_Id         = nextResourceId();
}

VulkanTexture& VulkanTexture::operator=(VulkanTexture&& other) noexcept
{
	if (this == &other)
		return *this;

	if (m_Ctx && (m_Image != VK_NULL_HANDLE || m_View != VK_NULL_HANDLE))
		m_Ctx->deferDestroy(m_Image, m_Allocation, m_View);

	assert((m_Ctx == nullptr || other.m_Ctx == nullptr || m_Ctx == other.m_Ctx) &&
	       "cross-context move: the deferred destroy would go on the wrong queue");

	m_Ctx        = other.m_Ctx;
	m_Image      = other.m_Image;
	m_Allocation = other.m_Allocation;
	m_View       = other.m_View;
	m_Sampler    = other.m_Sampler;   // borrowed; the displaced one is simply dropped
	m_Extent     = other.m_Extent;
	m_Id         = other.m_Id;

	other.m_Ctx        = nullptr;
	other.m_Image      = VK_NULL_HANDLE;
	other.m_Allocation = VK_NULL_HANDLE;
	other.m_View       = VK_NULL_HANDLE;
	other.m_Sampler    = VK_NULL_HANDLE;
	other.m_Extent     = {};
	other.m_Id         = nextResourceId();
	return *this;
}

VkDescriptorImageInfo VulkanTexture::descriptor() const
{
	// Hardcoding the layout is correct here and ONLY here: this class is
	// immutable, has exactly one layout for its whole life, and tracks none.
	VkDescriptorImageInfo info{};
	info.sampler     = m_Sampler;
	info.imageView   = m_View;
	info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	return info;
}

}
