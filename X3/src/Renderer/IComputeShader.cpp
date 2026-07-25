#include "Renderer/IComputeShader.h"
#include "Renderer/IRendererAPI.h"

#include "Platform/Vulkan/VulkanComputeShader.h"

namespace X3
{

	std::shared_ptr<IComputeShader> IComputeShader::Create(const std::string& filepath, const glm::uvec3& workGroupSizes) {
		return std::make_shared<VulkanComputeShader>(filepath, workGroupSizes);
	}
}