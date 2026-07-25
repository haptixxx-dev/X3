#include "Renderer/ITexture2D.h"
#include "Renderer/IRendererAPI.h"

#include "Platform/Vulkan/VulkanTexture2D.h"

namespace X3
{

	std::shared_ptr<ITexture2D> ITexture2D::Create(const unsigned char* data, const int width, const int height, int textureUnit) {
		return std::make_shared<VulkanTexture2D>(data, width, height, textureUnit);
	}
}