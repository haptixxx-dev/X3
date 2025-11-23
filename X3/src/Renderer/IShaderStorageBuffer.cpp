#include "IShaderStorageBuffer.h"
#include "Renderer/IRendererAPI.h"
#include "Platform/OpenGL/OpenGLShaderStorageBuffer.h"

namespace Laura
{

	std::shared_ptr<IShaderStorageBuffer> IShaderStorageBuffer::Create(uint32_t size, uint32_t bindingPoint, BufferUsageType type) {
		switch (IRendererAPI::GetAPI()) {
			case IRendererAPI::API::None: 
				LOG_ENGINE_CRITICAL("RendererAPI::None - UNSUPPORTED"); 
				return nullptr;
			case IRendererAPI::API::OpenGL: 
				return std::make_shared<OpenGLShaderStorageBuffer>(size, bindingPoint, type);
		}
		return nullptr;
	}

}
