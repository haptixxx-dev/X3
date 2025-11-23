#include "Renderer/IImage2D.h"
#include "Renderer/IRendererAPI.h"
#include "Platform/OpenGL/OpenGLImage2D.h"

namespace Laura 
{

	std::shared_ptr<IImage2D> IImage2D::Create(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType) {
		switch (IRendererAPI::GetAPI()) {
			case IRendererAPI::API::None: 
				LOG_ENGINE_CRITICAL("in IImage2D::Create() - RendererAPI::None UNSUPPORTED"); 
				return nullptr;
			case IRendererAPI::API::OpenGL: 
				return std::make_shared<OpenGLImage2D>(data, width, height, imageUnit, imageType);
		}
		return nullptr;
	}
}