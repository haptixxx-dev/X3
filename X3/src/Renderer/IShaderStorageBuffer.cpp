#include "IShaderStorageBuffer.h"
#include "Renderer/IRendererAPI.h"

#ifdef X3_USE_OPENGL
#include "Platform/OpenGL/OpenGLShaderStorageBuffer.h"
#endif
#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanShaderStorageBuffer.h"
#endif

namespace X3
{

	std::shared_ptr<IShaderStorageBuffer> IShaderStorageBuffer::Create(uint32_t size, uint32_t bindingPoint, BufferUsageType type) {
	#ifdef X3_USE_OPENGL
		return std::make_shared<OpenGLShaderStorageBuffer>(size, bindingPoint, type);
	#elif defined(X3_USE_VULKAN)
		return std::make_shared<VulkanShaderStorageBuffer>(size, bindingPoint, type);
	#else
		LOG_ENGINE_CRITICAL("No graphics API defined at build time!");
		return nullptr;
	#endif
	}

}
