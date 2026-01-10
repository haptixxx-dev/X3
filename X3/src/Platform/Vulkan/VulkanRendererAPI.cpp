#include "VulkanRendererAPI.h"
#include "VulkanContext.h"
#include "Core/Log.h"

namespace X3
{

void VulkanRendererAPI::Init() {
	LOG_ENGINE_INFO("VulkanRendererAPI initialized");
}

void VulkanRendererAPI::Clear(const glm::vec4& color) {
	// In Vulkan, clearing is done at the start of the render pass
	// The clear color is set in VulkanContext::beginFrame()
	// This function stores the clear color to be used in the next frame
	m_ClearColor = color;
}

void VulkanRendererAPI::SetViewportSize(uint32_t width, uint32_t height) {
	m_ViewportWidth = width;
	m_ViewportHeight = height;

	auto context = VulkanContext::Get();
	if (!context) {
		LOG_ENGINE_WARN("VulkanContext not available for viewport setting");
		return;
	}

	VkCommandBuffer cmd = context->getCurrentCommandBuffer();
	if (cmd == VK_NULL_HANDLE) {
		LOG_ENGINE_WARN("No active command buffer for viewport setting");
		return;
	}

	// Set viewport
	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = static_cast<float>(width);
	viewport.height = static_cast<float>(height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	// Set scissor
	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = {width, height};
	vkCmdSetScissor(cmd, 0, 1, &scissor);
}

glm::vec4 VulkanRendererAPI::GetClearColor() const {
	return m_ClearColor;
}

}
