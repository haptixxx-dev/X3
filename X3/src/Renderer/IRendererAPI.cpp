#include "Renderer/IRendererAPI.h"
#include "Platform/OpenGL/OpenGLRendererAPI.h"

namespace Laura 
{

	IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::OpenGL; // OpenGL by default

	std::shared_ptr<IRendererAPI> IRendererAPI::Create() {
		switch (IRendererAPI::GetAPI()) {
			case IRendererAPI::API::None: 
				LOG_ENGINE_CRITICAL("in ITexture::Create() - RendererAPI::None UNSUPPORTED"); 
				return nullptr;
			case IRendererAPI::API::OpenGL:
				return std::make_shared<OpenGLRendererAPI>();
		}
		return nullptr;
	}
}