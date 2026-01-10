#pragma once

#include "Renderer/IRendererAPI.h"
#include <glm/glm.hpp>

namespace X3
{

class VulkanRendererAPI : public IRendererAPI {
public:
	void Init() override;
	void Clear(const glm::vec4& color) override;
	void SetViewportSize(uint32_t width, uint32_t height) override;

	glm::vec4 GetClearColor() const;

private:
	uint32_t m_ViewportWidth = 0;
	uint32_t m_ViewportHeight = 0;
	glm::vec4 m_ClearColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
};

}
