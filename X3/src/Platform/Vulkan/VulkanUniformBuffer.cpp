#include "VulkanUniformBuffer.h"
#include "VulkanContext.h"
#include "Core/Log.h"

namespace X3
{

VulkanUniformBuffer::VulkanUniformBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type)
	: m_Size(size), m_BindingPoint(bindingPoint), m_UsageType(type) {

	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanUniformBuffer: No active Vulkan context!");
		return;
	}

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
	                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo allocationInfo;
	if (vmaCreateBuffer(context->getAllocator(), &bufferInfo, &allocInfo,
	                    &m_Buffer, &m_Allocation, &allocationInfo) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create Vulkan uniform buffer!");
		return;
	}

	// Buffer is persistently mapped
	m_MappedData = allocationInfo.pMappedData;

	LOG_ENGINE_INFO("Created Vulkan Uniform Buffer (size: {} bytes)", size);
}

VulkanUniformBuffer::~VulkanUniformBuffer() {
	auto context = VulkanContext::Get();
	if (!context || m_Buffer == VK_NULL_HANDLE) return;

	vmaDestroyBuffer(context->getAllocator(), m_Buffer, m_Allocation);
	m_Buffer = VK_NULL_HANDLE;
	m_Allocation = VK_NULL_HANDLE;
	m_MappedData = nullptr;
}

void VulkanUniformBuffer::Bind() {
	// In Vulkan, buffers are bound through descriptor sets
	// This is a placeholder for API compatibility
}

void VulkanUniformBuffer::Unbind() {
	// In Vulkan, unbinding is handled differently
	// This is a placeholder for API compatibility
}

void VulkanUniformBuffer::SetBindingPoint(uint32_t bindingPoint) {
	m_BindingPoint = bindingPoint;
}

void VulkanUniformBuffer::AddData(uint32_t offset, uint32_t dataSize, const void* data) {
	if (m_MappedData) {
		memcpy(static_cast<char*>(m_MappedData) + offset, data, dataSize);
	} else {
		LOG_ENGINE_WARN("VulkanUniformBuffer::AddData - buffer not mapped");
	}
}

}
