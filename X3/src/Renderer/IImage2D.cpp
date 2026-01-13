#include "Renderer/IImage2D.h"
#include "Renderer/IRendererAPI.h"

#ifdef X3_USE_OPENGL
#include "Platform/OpenGL/OpenGLImage2D.h"
#endif
#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanImage2D.h"
#endif

namespace X3
{

	std::shared_ptr<IImage2D> IImage2D::Create(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType) {
	#ifdef X3_USE_OPENGL
		return std::make_shared<OpenGLImage2D>(data, width, height, imageUnit, imageType);
	#elif defined(X3_USE_VULKAN)
		return std::make_shared<VulkanImage2D>(data, width, height, imageUnit, imageType);
	#else
		LOG_ENGINE_CRITICAL("No graphics API defined at build time!");
		return nullptr;
	#endif
	}
}