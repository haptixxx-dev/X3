#include "Renderer/IRendererAPI.h"

#include "Platform/Vulkan/VulkanRendererAPI.h"

namespace X3
{

	std::shared_ptr<IRendererAPI> IRendererAPI::Create() {
		return std::make_shared<VulkanRendererAPI>();
	}
}
