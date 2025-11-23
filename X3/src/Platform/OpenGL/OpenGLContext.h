#pragma once

#include "Renderer/IRenderingContext.h"
#include "Core/IWindow.h"

namespace Laura 
{

	class OpenGLContext : public IRenderingContext {
	public:
		static void setWindowHints();

		OpenGLContext(GLFWwindow* nativeWindow);
		void init() override;
		void swapBuffers() override;
	private:
		GLFWwindow* m_NativeWindow;
	};
}