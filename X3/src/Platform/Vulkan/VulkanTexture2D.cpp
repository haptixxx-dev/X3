#include "VulkanTexture2D.h"
#include "VulkanContext.h"
#include "Core/Log.h"

namespace X3
{

VulkanTexture2D::VulkanTexture2D(const unsigned char* data, int width, int height, int textureUnit)
	: m_Width(width), m_Height(height), m_TextureUnit(textureUnit) {

	size_t imageSize = width * height * 4; // RGBA
	createImage(data, imageSize);
	createImageView();
	createSampler();

	LOG_ENGINE_INFO("Created Vulkan Texture2D: {}x{}", width, height);
}

VulkanTexture2D::~VulkanTexture2D() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkDevice device = context->getDevice();

	if (m_Sampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, m_Sampler, nullptr);
	}
	if (m_ImageView != VK_NULL_HANDLE) {
		vkDestroyImageView(device, m_ImageView, nullptr);
	}
	if (m_Image != VK_NULL_HANDLE) {
		vmaDestroyImage(context->getAllocator(), m_Image, m_Allocation);
	}
}

void VulkanTexture2D::ChangeTextureUnit(int textureUnit) {
	m_TextureUnit = textureUnit;
	// Register this texture with VulkanContext for descriptor set binding
	auto context = VulkanContext::Get();
	if (context && m_ImageView != VK_NULL_HANDLE && m_Sampler != VK_NULL_HANDLE) {
		context->registerSampledImage(textureUnit, m_ImageView, m_Sampler);
	}
}

void VulkanTexture2D::createImage(const void* data, size_t dataSize) {
	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanTexture2D: No active Vulkan context!");
		return;
	}

	// Create staging buffer
	VkBuffer stagingBuffer;
	VmaAllocation stagingAllocation;

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = dataSize;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
	                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo allocationInfo;
	if (vmaCreateBuffer(context->getAllocator(), &bufferInfo, &allocInfo,
	                    &stagingBuffer, &stagingAllocation, &allocationInfo) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create staging buffer!");
		return;
	}

	// Copy data to staging buffer
	memcpy(allocationInfo.pMappedData, data, dataSize);

	// Create image
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = m_Width;
	imageInfo.extent.height = m_Height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	VmaAllocationCreateInfo imageAllocInfo{};
	imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	imageAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

	if (vmaCreateImage(context->getAllocator(), &imageInfo, &imageAllocInfo,
	                   &m_Image, &m_Allocation, nullptr) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create image!");
		vmaDestroyBuffer(context->getAllocator(), stagingBuffer, stagingAllocation);
		return;
	}

	// Use single-time command buffer for image upload to avoid interfering with frame rendering
	VkCommandBuffer cmd = context->beginSingleTimeCommands();

	// Transition to transfer dst
	transitionImageLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	// Copy buffer to image
	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {static_cast<uint32_t>(m_Width), static_cast<uint32_t>(m_Height), 1};

	vkCmdCopyBufferToImage(cmd, stagingBuffer, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// Transition to shader read
	transitionImageLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// End single-time commands (submits and waits for completion)
	context->endSingleTimeCommands(cmd);

	// Now safe to destroy staging buffer since GPU work is complete
	vmaDestroyBuffer(context->getAllocator(), stagingBuffer, stagingAllocation);
}

void VulkanTexture2D::createImageView() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = m_Image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	if (vkCreateImageView(context->getDevice(), &viewInfo, nullptr, &m_ImageView) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create image view!");
	}
}

void VulkanTexture2D::createSampler() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;

	if (vkCreateSampler(context->getDevice(), &samplerInfo, nullptr, &m_Sampler) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create texture sampler!");
	}
}

void VulkanTexture2D::transitionImageLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout) {
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = m_Image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;

	VkPipelineStageFlags sourceStage;
	VkPipelineStageFlags destinationStage;

	if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		barrier.srcAccessMask = 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	} else {
		LOG_ENGINE_ERROR("Unsupported layout transition!");
		return;
	}

	vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}
