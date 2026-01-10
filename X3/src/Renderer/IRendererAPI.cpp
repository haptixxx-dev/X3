#include "Renderer/IRendererAPI.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include "Platform/Vulkan/VulkanRendererAPI.h"

namespace X3 
{

	// Use Vulkan on macOS (OpenGL 4.1 lacks compute shaders), OpenGL elsewhere
	#ifdef __APPLE__
	IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::Vulkan;
	#else
	IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::OpenGL;
	#endif

	std::shared_ptr<IRendererAPI> IRendererAPI::Create() {
		switch (IRendererAPI::GetAPI()) {
			case IRendererAPI::API::None:
				LOG_ENGINE_CRITICAL("in ITexture::Create() - RendererAPI::None UNSUPPORTED");
				return nullptr;
			case IRendererAPI::API::OpenGL:
				return std::make_shared<OpenGLRendererAPI>();
			case IRendererAPI::API::Vulkan:
				return std::make_shared<VulkanRendererAPI>();
		}
		return nullptr;
	}
}