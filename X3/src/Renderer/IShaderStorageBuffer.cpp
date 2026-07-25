#include "IShaderStorageBuffer.h"
#include "Renderer/IRendererAPI.h"

#include "Platform/Vulkan/VulkanShaderStorageBuffer.h"

namespace X3
{

	std::shared_ptr<IShaderStorageBuffer> IShaderStorageBuffer::Create(uint32_t size, uint32_t bindingPoint, BufferUsageType type) {
		return std::make_shared<VulkanShaderStorageBuffer>(size, bindingPoint, type);
	}

}
