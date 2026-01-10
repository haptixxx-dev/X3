#include "Renderer/IComputeShader.h"
#include "Renderer/IRendererAPI.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/Vulkan/VulkanComputeShader.h"

namespace X3 
{

	std::shared_ptr<IComputeShader> IComputeShader::Create(const std::string& filepath, const glm::uvec3& workGroupSizes) {
		switch (IRendererAPI::GetAPI()) {
			case IRendererAPI::API::None:
				LOG_ENGINE_CRITICAL("RendererAPI::None - UNSUPPORTED");
				return nullptr;
			case IRendererAPI::API::OpenGL:
				return std::make_shared<OpenGLComputeShader>(filepath, workGroupSizes);
			case IRendererAPI::API::Vulkan:
				return std::make_shared<VulkanComputeShader>(filepath, workGroupSizes);
		}
		return nullptr;
	}
}