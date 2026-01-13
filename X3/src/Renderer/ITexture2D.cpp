#include "Renderer/ITexture2D.h"
#include "Renderer/IRendererAPI.h"

#ifdef X3_USE_OPENGL
#include "Platform/OpenGL/OpenGLTexture2D.h"
#endif
#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanTexture2D.h"
#endif

namespace X3
{

	std::shared_ptr<ITexture2D> ITexture2D::Create(const unsigned char* data, const int width, const int height, int textureUnit) {
	#ifdef X3_USE_OPENGL
		return std::make_shared<OpenGLTexture2D>(data, width, height, textureUnit);
	#elif defined(X3_USE_VULKAN)
		return std::make_shared<VulkanTexture2D>(data, width, height, textureUnit);
	#else
		LOG_ENGINE_CRITICAL("No graphics API defined at build time!");
		return nullptr;
	#endif
	}
}