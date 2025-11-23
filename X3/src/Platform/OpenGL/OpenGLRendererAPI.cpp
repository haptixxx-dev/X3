#include "Platform/OpenGL/OpenGLRendererAPI.h"
#include <GL/glew.h>
#include "Renderer/IComputeShader.h"

namespace Laura 
{

	void OpenGLRendererAPI::Init() {
	}

	void OpenGLRendererAPI::Clear(const glm::vec4& color) {
		glClearColor(color.r, color.g, color.b, color.a);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	void OpenGLRendererAPI::SetViewportSize(uint32_t width, uint32_t height) {
		glViewport(0, 0, width, height);
	}
}