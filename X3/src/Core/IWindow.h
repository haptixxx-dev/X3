#pragma once

#include "lrpch.h"
#include "Events/KeyEvents.h"
#include "Events/MouseEvents.h"

// Forward declaration
namespace Laura {
    class IEvent;
}

namespace Laura
{

	struct WindowProps {
		std::string title;
		int width;
		int height;
		bool VSync;
        bool CustomTitlebar;

		WindowProps(const std::string& title = std::string("LauraEngine"),
			int width = 1280,
			int height = 720,
			bool VSync = false,
			bool CustomTitlebar = true)
			: width(width), height(height), title(title), VSync(VSync), CustomTitlebar(CustomTitlebar) {
		}
	};

	class IWindow {
	public:
		virtual ~IWindow() = default;

		virtual void onUpdate() = 0;

		virtual void setTitle(const std::string& title) = 0;

		virtual glm::ivec2 getFrameBufferSize() const = 0;

		virtual void* getNativeWindow() const = 0;

		virtual bool isVSync() const = 0;
		virtual void setVSync(bool enabled) = 0;

		virtual bool isFullscreen() const = 0;
		virtual void setFullscreen(bool enabled) = 0;

		// Window control & custom titlebar support
		virtual void minimize() = 0;
		virtual void maximize() = 0;
		virtual void restore() = 0;
		virtual bool isMaximized() const = 0;
		virtual void close() = 0;
		virtual void setPosition(int x, int y) = 0;
		virtual glm::ivec2 getPosition() const = 0;
		virtual void setTitlebarHitTestCallback(const std::function<bool(int, int)>& callback) = 0;

		/// input polling
		virtual bool isKeyPressed(KeyCode key) = 0;
		virtual bool isMouseButtonPressed(MouseCode) = 0;
		virtual std::pair<float, float> getMousePosition() = 0;

		virtual bool shouldClose() = 0;

		// expects a function that takes an Event* as a parameter and returns void
		virtual void setEventCallback(const std::function<void(std::shared_ptr<IEvent>)>& callback) = 0;

		/// createWindow is a factory method that creates a window with the given properties.
		/// reutrns a heap pointer
		static std::shared_ptr<IWindow> createWindow(const WindowProps windowProps);
	};

}