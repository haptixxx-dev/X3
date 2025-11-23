#include "Renderer/ITexture2D.h"
#include "Renderer/IRendererAPI.h"
#include "Platform/OpenGL/OpenGLTexture2D.h"

namespace Laura 
{

	std::shared_ptr<ITexture2D> ITexture2D::Create(const unsigned char* data, const int width, const int height, int textureUnit) {
		switch (IRendererAPI::GetAPI()) {
			case IRendererAPI::API::None: 
				LOG_ENGINE_CRITICAL("in ITexture2D::Create() - RendererAPI::None UNSUPPORTED"); 
				return nullptr;
			case IRendererAPI::API::OpenGL: 
				return std::make_shared<OpenGLTexture2D>(data, width, height, textureUnit);
		}
		return nullptr;
	}
}