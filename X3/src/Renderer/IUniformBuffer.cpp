#include "IUniformBuffer.h"
#include "Renderer/IRendererAPI.h"

#ifdef X3_USE_OPENGL
#include "Platform/OpenGL/OpenGLUniformBuffer.h"
#endif
#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanUniformBuffer.h"
#endif

namespace X3
{

	std::shared_ptr<IUniformBuffer> IUniformBuffer::Create(uint32_t size, uint32_t bindingPoint, BufferUsageType type) {
	#ifdef X3_USE_OPENGL
		return std::make_shared<OpenGLUniformBuffer>(size, bindingPoint, type);
	#elif defined(X3_USE_VULKAN)
		return std::make_shared<VulkanUniformBuffer>(size, bindingPoint, type);
	#else
		LOG_ENGINE_CRITICAL("No graphics API defined at build time!");
		return nullptr;
	#endif
	}
}
