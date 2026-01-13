#include "Renderer/IComputeShader.h"
#include "Renderer/IRendererAPI.h"

#ifdef X3_USE_OPENGL
#include "Platform/OpenGL/OpenGLComputeShader.h"
#endif
#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanComputeShader.h"
#endif

namespace X3
{

	std::shared_ptr<IComputeShader> IComputeShader::Create(const std::string& filepath, const glm::uvec3& workGroupSizes) {
	#ifdef X3_USE_OPENGL
		return std::make_shared<OpenGLComputeShader>(filepath, workGroupSizes);
	#elif defined(X3_USE_VULKAN)
		return std::make_shared<VulkanComputeShader>(filepath, workGroupSizes);
	#else
		LOG_ENGINE_CRITICAL("No graphics API defined at build time!");
		return nullptr;
	#endif
	}
}