#pragma once

#include "lrpch.h"
#include "Renderer/IComputeShader.h"

namespace Laura 
{

	class IRendererAPI {
	public:
		enum class API {
			None = 0,
			OpenGL = 1,
		};

	public:
		virtual void Init() = 0;
		virtual void Clear(const glm::vec4& color) = 0;
		virtual void SetViewportSize(uint32_t width, uint32_t height) = 0;

		static std::shared_ptr<IRendererAPI> Create();
		static API GetAPI() { return s_API; }
		static void SetAPI(API api) { s_API = api; }

	private:
		static API s_API;
	};
}