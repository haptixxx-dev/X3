#include "Renderer/IRendererAPI.h"

#ifdef X3_USE_OPENGL
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#endif
#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanRendererAPI.h"
#endif

namespace X3
{

	// Set API based on build configuration
	#ifdef X3_USE_VULKAN
	IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::Vulkan;
	#else
	IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::OpenGL;
	#endif

	std::shared_ptr<IRendererAPI> IRendererAPI::Create() {
	#ifdef X3_USE_OPENGL
		return std::make_shared<OpenGLRendererAPI>();
	#elif defined(X3_USE_VULKAN)
		return std::make_shared<VulkanRendererAPI>();
	#else
		LOG_ENGINE_CRITICAL("No graphics API defined at build time!");
		return nullptr;
	#endif
	}
}