#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Platform/OpenGL/OpenGLContext.h"
#include "Core/Log.h"

namespace Laura 
{ 

	void OpenGLContext::setWindowHints() {
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	}

	OpenGLContext::OpenGLContext(GLFWwindow* window)
		: m_NativeWindow(window){
	}

	void OpenGLContext::init() {
		glfwMakeContextCurrent(m_NativeWindow);

		if (glewInit() != GLEW_OK) {
			LOG_ENGINE_CRITICAL("[ERROR] Failed to initialize GLEW!");
		}

		LOG_ENGINE_INFO("Using OpenGL - Version: {0}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
		LOG_ENGINE_INFO("(OpenGLContext.cpp) Successfully initialized OpenGL context and GLEW!");
	}

	void OpenGLContext::swapBuffers() {
		glfwSwapBuffers(m_NativeWindow);
	}

}