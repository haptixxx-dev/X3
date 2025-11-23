#pragma once

#include "Core/Events/IEvent.h"

namespace Laura 
{

	using MouseCode = uint16_t;
	namespace Mouse
	{

		enum : MouseCode {
			// From glfw3.h
			MOUSE_BUTTON_1 = 0,
			MOUSE_BUTTON_2 = 1,
			MOUSE_BUTTON_3 = 2,
			MOUSE_BUTTON_4 = 3,
			MOUSE_BUTTON_5 = 4,
			MOUSE_BUTTON_6 = 5,
			MOUSE_BUTTON_7 = 6,
			MOUSE_BUTTON_8 = 7,
			MOUSE_LEFT = MOUSE_BUTTON_1,
			MOUSE_RIGHT = MOUSE_BUTTON_2,
			MOUSE_MIDDLE = MOUSE_BUTTON_3
		};
	}

	struct MouseMoveEvent : public IEvent {
		double xpos, ypos;

		MouseMoveEvent(double x, double y)
			: xpos(x), ypos(y) {}

		inline EventType GetType() const override { return EventType::MOUSE_MOVE_EVENT; }
		inline bool IsInputEvent() const override { return true; }
	};

	struct MouseButtonPressEvent : public IEvent {
		MouseCode button;

		MouseButtonPressEvent(MouseCode btn)
			: button(btn) {}

		inline EventType GetType() const override { return EventType::MOUSE_BUTTON_PRESS_EVENT; }
		inline bool IsInputEvent() const override { return true; }
	};

	struct MouseButtonReleaseEvent : public IEvent {
		MouseCode button;

		MouseButtonReleaseEvent(MouseCode btn)
			: button(btn) {}

		inline EventType GetType() const override { return EventType::MOUSE_BUTTON_RELEASE_EVENT; }
		inline bool IsInputEvent() const override { return true; }
	};

	struct MouseScrollEvent : public IEvent {
		double xoffset, yoffset;

		MouseScrollEvent(double xoff, double yoff)
			: xoffset(xoff), yoffset(yoff) {}

		inline EventType GetType() const override { return EventType::MOUSE_SCROLL_EVENT; }
		inline bool IsInputEvent() const override { return true; }
	};
}