#include "Renderer/IRendererAPI.h"

#include "Platform/Vulkan/VulkanRendererAPI.h"

namespace X3
{

	IRendererAPI::API IRendererAPI::s_API = IRendererAPI::API::Vulkan;

	std::shared_ptr<IRendererAPI> IRendererAPI::Create() {
		return std::make_shared<VulkanRendererAPI>();
	}
}
