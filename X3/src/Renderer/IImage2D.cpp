#include "Renderer/IImage2D.h"
#include "Renderer/IRendererAPI.h"

#include "Platform/Vulkan/VulkanImage2D.h"

namespace X3
{

	std::shared_ptr<IImage2D> IImage2D::Create(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType) {
		return std::make_shared<VulkanImage2D>(data, width, height, imageUnit, imageType);
	}
}