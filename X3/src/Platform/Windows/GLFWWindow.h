#pragma once

#include <GLFW/glfw3.h>
#include "Core/IWindow.h"
#include "Platform/OpenGL/OpenGLContext.h"
#include "Core/Events/KeyEvents.h"
#include "Core/Events/MouseEvents.h"

namespace Laura 
{

	class GLFWWindowIMPL : public IWindow {
	public:
		GLFWWindowIMPL(const WindowProps& windowProps);
		~GLFWWindowIMPL();

		void onUpdate() override;

		void setTitle(const std::string& title) override;

		glm::ivec2 getFrameBufferSize() const override;

		bool isVSync() const override;
		void setVSync(bool enabled) override;

		void* getNativeWindow() const override;

		bool isFullscreen() const override;
		void setFullscreen(bool enabled) override;

		// Custom titlebar/window control (Walnut fork)
		void minimize() override;
		void maximize() override;
		void restore() override;
		bool isMaximized() const override;
		void close() override;
		void setPosition(int x, int y) override;
		glm::ivec2 getPosition() const override;
		void setTitlebarHitTestCallback(const std::function<bool(int, int)>& callback) override;

		///  input polling
		bool isKeyPressed(KeyCode key) override;
		bool isMouseButtonPressed(MouseCode) override;
		std::pair<float, float> getMousePosition() override;
		void setEventCallback(const std::function<void(std::shared_ptr<IEvent>)>& callback) override;

		bool shouldClose() override;

	private:
		GLFWwindow* m_NativeWindow;
		OpenGLContext* m_Context;

		std::function<void(std::shared_ptr<IEvent>)> dispatchEvent;
        std::function<bool(int, int)> m_TitlebarHitTest;

		bool m_Fullscreen = false, m_VSync = false;
		int m_WindowedPosX, m_WindowedPosY, m_WindowedWidth, m_WindowedHeight; // cache windowed position (during fullscreen)

		/// These are callback methods called by GLFW when an event occurs.
		/// They are static methods because GLFW requires them to be static. The method gets the EventDispatcher
		/// object of the current instance (self) from the GLFW window user pointer to perform actions as if the function was not static.
		static void GLFWKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
		static void GLFWMousePositionCallback(GLFWwindow* window, double xpos, double ypos);
		static void GLFWMouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
		static void GLFWScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
		static void GLFWWindowResizeCallback(GLFWwindow* window, int width, int height);
	};
}