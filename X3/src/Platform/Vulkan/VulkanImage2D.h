#pragma once

#include "Renderer/IImage2D.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace X3
{

class VulkanImage2D : public IImage2D {
public:
	VulkanImage2D(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType);
	~VulkanImage2D();

	void ChangeImageUnit(int imageUnit) override;
	int GetID() const override { return m_ID; }
	glm::ivec2 GetDimensions() const override { return m_Dimensions; }

	// Vulkan-specific getters
	VkImage getImage() const { return m_Image; }
	VkImageView getImageView() const { return m_ImageView; }
	int getImageUnit() const { return m_ImageUnit; }
	Image2DType getImageType() const { return m_ImageType; }

	// Transition image layout (useful for compute shader access)
	void transitionToGeneral(VkCommandBuffer cmd);
	void transitionToShaderRead(VkCommandBuffer cmd);

private:
	void createImage(unsigned char* data);
	void createImageView();
	void transitionImageLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout,
	                           VkAccessFlags srcAccess, VkAccessFlags dstAccess,
	                           VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

private:
	int m_ID = 0;
	int m_Width = 0;
	int m_Height = 0;
	int m_ImageUnit = 0;
	Image2DType m_ImageType;
	glm::ivec2 m_Dimensions;

	VkImage m_Image = VK_NULL_HANDLE;
	VmaAllocation m_Allocation = VK_NULL_HANDLE;
	VkImageView m_ImageView = VK_NULL_HANDLE;
	VkImageLayout m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	static int s_NextID;
};

}
