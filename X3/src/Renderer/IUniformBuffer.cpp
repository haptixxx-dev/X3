#include "IUniformBuffer.h"
#include "Renderer/IRendererAPI.h"

#include "Platform/Vulkan/VulkanUniformBuffer.h"

namespace X3
{

	std::shared_ptr<IUniformBuffer> IUniformBuffer::Create(uint32_t size, uint32_t bindingPoint, BufferUsageType type) {
		return std::make_shared<VulkanUniformBuffer>(size, bindingPoint, type);
	}
}
