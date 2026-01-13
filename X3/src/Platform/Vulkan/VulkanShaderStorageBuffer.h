#pragma once

#include "Renderer/IShaderStorageBuffer.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace X3
{

class VulkanShaderStorageBuffer : public IShaderStorageBuffer {
public:
	VulkanShaderStorageBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type = BufferUsageType::DYNAMIC_DRAW);
	~VulkanShaderStorageBuffer();

	void Bind() override;
	void Unbind() override;
	void SetBindingPoint(uint32_t bindingPoint) override;
	void AddData(uint32_t offset, uint32_t dataSize, const void* data) override;
	void* ReadData(uint32_t offset, uint32_t dataSize) override;

	// Vulkan-specific getters
	VkBuffer getBuffer() const { return m_Buffer; }
	uint32_t getSize() const { return m_Size; }
	uint32_t getBindingPoint() const { return m_BindingPoint; }

private:
	uint32_t m_Size = 0;
	uint32_t m_BindingPoint = 0;
	BufferUsageType m_UsageType;

	VkBuffer m_Buffer = VK_NULL_HANDLE;
	VmaAllocation m_Allocation = VK_NULL_HANDLE;
	void* m_MappedData = nullptr;

	// Temporary storage for ReadData
	std::vector<char> m_ReadBuffer;
};

}
