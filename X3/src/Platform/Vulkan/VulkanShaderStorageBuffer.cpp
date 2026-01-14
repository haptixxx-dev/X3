#include "VulkanShaderStorageBuffer.h"
#include "VulkanContext.h"
#include "Core/Log.h"

namespace X3
{

VulkanShaderStorageBuffer::VulkanShaderStorageBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type)
	: m_Size(size), m_BindingPoint(bindingPoint), m_UsageType(type) {

	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_ERROR("VulkanShaderStorageBuffer: No active Vulkan context!");
		return;
	}

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	// Use RANDOM access bit for both read and write operations
	allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
	                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo allocationInfo;
	if (vmaCreateBuffer(context->getAllocator(), &bufferInfo, &allocInfo,
	                    &m_Buffer, &m_Allocation, &allocationInfo) != VK_SUCCESS) {
		LOG_ENGINE_ERROR("Failed to create Vulkan shader storage buffer!");
		return;
	}

	// Buffer is persistently mapped
	m_MappedData = allocationInfo.pMappedData;

	LOG_ENGINE_INFO("Created Vulkan Shader Storage Buffer (size: {} bytes, binding: {})", size, bindingPoint);
}

VulkanShaderStorageBuffer::~VulkanShaderStorageBuffer() {
	auto context = VulkanContext::Get();
	if (!context || m_Buffer == VK_NULL_HANDLE) return;

	vmaDestroyBuffer(context->getAllocator(), m_Buffer, m_Allocation);
	m_Buffer = VK_NULL_HANDLE;
	m_Allocation = VK_NULL_HANDLE;
	m_MappedData = nullptr;
}

void VulkanShaderStorageBuffer::Bind() {
	// Register this buffer with VulkanContext for descriptor set binding
	auto context = VulkanContext::Get();
	if (context && m_Buffer != VK_NULL_HANDLE) {
		context->registerStorageBuffer(m_BindingPoint, m_Buffer, m_Size);
	}
}

void VulkanShaderStorageBuffer::Unbind() {
	// In Vulkan, unbinding is handled differently
	// This is a placeholder for API compatibility
}

void VulkanShaderStorageBuffer::SetBindingPoint(uint32_t bindingPoint) {
	m_BindingPoint = bindingPoint;
}

void VulkanShaderStorageBuffer::AddData(uint32_t offset, uint32_t dataSize, const void* data) {
	if (m_MappedData && data) {
		memcpy(static_cast<char*>(m_MappedData) + offset, data, dataSize);
	} else {
		LOG_ENGINE_WARN("VulkanShaderStorageBuffer::AddData - buffer not mapped or data is null");
	}
}

void* VulkanShaderStorageBuffer::ReadData(uint32_t offset, uint32_t dataSize) {
	if (!m_MappedData) {
		LOG_ENGINE_WARN("VulkanShaderStorageBuffer::ReadData - buffer not mapped");
		return nullptr;
	}

	// Ensure read buffer is large enough
	if (m_ReadBuffer.size() < dataSize) {
		m_ReadBuffer.resize(dataSize);
	}

	// Copy from mapped memory to our read buffer
	// Note: For GPU-written data, a memory barrier should be issued before this
	memcpy(m_ReadBuffer.data(), static_cast<char*>(m_MappedData) + offset, dataSize);

	return m_ReadBuffer.data();
}

}
