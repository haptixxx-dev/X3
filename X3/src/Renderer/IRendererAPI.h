#pragma once

#include "lrpch.h"
#include "Renderer/IComputeShader.h"

namespace X3 
{

	class IRendererAPI {
	public:
		virtual void Init() = 0;
		virtual void Clear(const glm::vec4& color) = 0;
		virtual void SetViewportSize(uint32_t width, uint32_t height) = 0;

		static std::shared_ptr<IRendererAPI> Create();
	};
}