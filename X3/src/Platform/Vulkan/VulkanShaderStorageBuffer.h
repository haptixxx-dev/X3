#pragma once

#include "lrpch.h"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace X3
{

// Enum class to give the user the option to choose between STATIC_DRAW or DYNAMIC_DRAW
// STATIC_DRAW: The data store contents will be modified once and used many times.
// DYNAMIC_DRAW: The data store contents will be modified repeatedly and used many times.
#ifndef BUFFER_USAGE_TYPE_STRUCT
#define BUFFER_USAGE_TYPE_STRUCT
	enum class BufferUsageType {
		STATIC_DRAW = 0,
		DYNAMIC_DRAW = 1
	};
#endif

class VulkanShaderStorageBuffer {
public:
	VulkanShaderStorageBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type = BufferUsageType::DYNAMIC_DRAW);
	~VulkanShaderStorageBuffer();

	void Bind();
	void Unbind();
	void SetBindingPoint(uint32_t bindingPoint);
	void AddData(uint32_t offset, uint32_t dataSize, const void* data);
	void* ReadData(uint32_t offset, uint32_t dataSize);

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
