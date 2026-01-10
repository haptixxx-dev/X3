#pragma once

#include "Renderer/IUniformBuffer.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace X3
{

class VulkanUniformBuffer : public IUniformBuffer {
public:
	VulkanUniformBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type = BufferUsageType::DYNAMIC_DRAW);
	~VulkanUniformBuffer();

	void Bind() override;
	void Unbind() override;
	void SetBindingPoint(uint32_t bindingPoint) override;
	void AddData(uint32_t offset, uint32_t dataSize, const void* data) override;

	// Vulkan-specific getters
	VkBuffer getBuffer() const { return m_Buffer; }

private:
	uint32_t m_Size = 0;
	uint32_t m_BindingPoint = 0;
	BufferUsageType m_UsageType;

	VkBuffer m_Buffer = VK_NULL_HANDLE;
	VmaAllocation m_Allocation = VK_NULL_HANDLE;
	void* m_MappedData = nullptr;
};

}
