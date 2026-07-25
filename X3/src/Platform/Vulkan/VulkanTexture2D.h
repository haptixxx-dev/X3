#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace X3
{

class VulkanTexture2D {
public:
	VulkanTexture2D(const unsigned char* data, int width, int height, int textureUnit);
	~VulkanTexture2D();

	void ChangeTextureUnit(int textureUnit);
	int GetID() const { return static_cast<int>(reinterpret_cast<uintptr_t>(m_Image)); }

	// Vulkan-specific getters
	VkImage getImage() const { return m_Image; }
	VkImageView getImageView() const { return m_ImageView; }
	VkSampler getSampler() const { return m_Sampler; }

private:
	void createImage(const void* data, size_t dataSize);
	void createImageView();
	void createSampler();
	void transitionImageLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout);

private:
	int m_Width = 0;
	int m_Height = 0;
	int m_TextureUnit = 0;

	VkImage m_Image = VK_NULL_HANDLE;
	VmaAllocation m_Allocation = VK_NULL_HANDLE;
	VkImageView m_ImageView = VK_NULL_HANDLE;
	VkSampler m_Sampler = VK_NULL_HANDLE;
};

}
