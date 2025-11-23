#pragma once

#include "lrpch.h"
#include "Renderer/IRendererAPI.h"

namespace Laura 
{

	class OpenGLRendererAPI : public IRendererAPI {
	public:
		virtual void Init() override;
		virtual void Clear(const glm::vec4& color) override;
		virtual void SetViewportSize(uint32_t width, uint32_t height) override;
	};
}