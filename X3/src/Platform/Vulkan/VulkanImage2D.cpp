#include "VulkanImage2D.h"
#include "VulkanContext.h"
#include "Core/Log.h"

namespace X3
{

int VulkanImage2D::s_NextID = 1;

VulkanImage2D::VulkanImage2D(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType)
	: m_Width(width), m_Height(height), m_ImageUnit(imageUnit), m_ImageType(imageType), m_Dimensions(width, height) {

	if (width <= 0 || height <= 0) {
		LOG_ENGINE_CRITICAL("VulkanImage2D: Invalid image dimensions {}x{}", width, height);
		return;
	}

	m_ID = s_NextID++;

	createImage(data);
	createImageView();

	// Transition to GENERAL layout for storage image access
	// Use single-time command buffer to avoid interfering with frame rendering
	auto context = VulkanContext::Get();
	if (context) {
		VkCommandBuffer cmd = context->beginSingleTimeCommands();
		transitionToGeneral(cmd);
		context->endSingleTimeCommands(cmd);
	}

	LOG_ENGINE_INFO("Created Vulkan Image2D: {}x{} (ID: {}, Unit: {})", width, height, m_ID, imageUnit);
}

VulkanImage2D::~VulkanImage2D() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkDevice device = context->getDevice();

	if (m_ImageView != VK_NULL_HANDLE) {
		vkDestroyImageView(device, m_ImageView, nullptr);
	}
	if (m_Image != VK_NULL_HANDLE) {
		vmaDestroyImage(context->getAllocator(), m_Image, m_Allocation);
	}
}

void VulkanImage2D::ChangeImageUnit(int imageUnit) {
	m_ImageUnit = imageUnit;
	// Register this image with VulkanContext for descriptor set binding
	auto context = VulkanContext::Get();
	if (context && m_ImageView != VK_NULL_HANDLE) {
		context->registerStorageImage(imageUnit, m_ImageView);
	}
}

void VulkanImage2D::createImage(unsigned char* data) {
	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanImage2D: No active Vulkan context!");
		return;
	}

	// Create image with storage bit for compute shader access
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = m_Width;
	imageInfo.extent.height = m_Height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT; // RGBA32F to match OpenGL
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

	VmaAllocationCreateInfo imageAllocInfo{};
	imageAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	imageAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

	if (vmaCreateImage(context->getAllocator(), &imageInfo, &imageAllocInfo,
	                   &m_Image, &m_Allocation, nullptr) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("VulkanImage2D: Failed to create image!");
		return;
	}

	// If data is provided, upload it using a single-time command buffer
	if (data) {
		size_t dataSize = m_Width * m_Height * 4; // RGBA bytes from input

		// Create staging buffer
		VkBuffer stagingBuffer;
		VmaAllocation stagingAllocation;

		VkBufferCreateInfo bufferInfo{};
		bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferInfo.size = m_Width * m_Height * 16; // RGBA32F = 16 bytes per pixel
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

		VmaAllocationInfo allocationInfo;
		if (vmaCreateBuffer(context->getAllocator(), &bufferInfo, &allocInfo,
		                    &stagingBuffer, &stagingAllocation, &allocationInfo) != VK_SUCCESS) {
			LOG_ENGINE_ERROR("VulkanImage2D: Failed to create staging buffer!");
			return;
		}

		// Convert RGBA8 to RGBA32F
		float* floatData = static_cast<float*>(allocationInfo.pMappedData);
		for (int i = 0; i < m_Width * m_Height * 4; i++) {
			floatData[i] = data[i] / 255.0f;
		}

		// Use single-time command buffer for data upload
		VkCommandBuffer cmd = context->beginSingleTimeCommands();

		// Transition to transfer dst
		transitionImageLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

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

		m_CurrentLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

		// End single-time commands (submits and waits)
		context->endSingleTimeCommands(cmd);

		// Cleanup staging buffer after GPU is done
		vmaDestroyBuffer(context->getAllocator(), stagingBuffer, stagingAllocation);
	}
}

void VulkanImage2D::createImageView() {
	auto context = VulkanContext::Get();
	if (!context) return;

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = m_Image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;

	if (vkCreateImageView(context->getDevice(), &viewInfo, nullptr, &m_ImageView) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("VulkanImage2D: Failed to create image view!");
	}
}

void VulkanImage2D::transitionImageLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout,
                                          VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                          VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
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
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;

	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	m_CurrentLayout = newLayout;
}

void VulkanImage2D::transitionToGeneral(VkCommandBuffer cmd) {
	if (m_CurrentLayout == VK_IMAGE_LAYOUT_GENERAL) return;

	VkAccessFlags srcAccess = 0;
	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

	if (m_CurrentLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
		srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	} else if (m_CurrentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
		srcAccess = VK_ACCESS_SHADER_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	}

	transitionImageLayout(cmd, m_CurrentLayout, VK_IMAGE_LAYOUT_GENERAL,
	                      srcAccess, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	                      srcStage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}

void VulkanImage2D::transitionToShaderRead(VkCommandBuffer cmd) {
	if (m_CurrentLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return;

	VkAccessFlags srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

	if (m_CurrentLayout == VK_IMAGE_LAYOUT_GENERAL) {
		srcAccess = VK_ACCESS_SHADER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
	}

	transitionImageLayout(cmd, m_CurrentLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                      srcAccess, VK_ACCESS_SHADER_READ_BIT,
	                      srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

}
