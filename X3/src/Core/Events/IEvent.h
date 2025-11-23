#pragma once

#include <ostream>

namespace Laura 
{

	enum struct EventType {
		// InputEvents.h
		KEY_PRESS_EVENT,
		KEY_RELEASE_EVENT,
		KEY_REPEAT_EVENT,

		MOUSE_MOVE_EVENT,
		MOUSE_BUTTON_PRESS_EVENT,
		MOUSE_BUTTON_RELEASE_EVENT,
		MOUSE_SCROLL_EVENT,

		// EngineEvents.h
		NEW_FRAME_RENDERED_EVENT,
		UPDATE_RENDER_SETTINGS_EVENT,

		WINDOW_RESIZE_EVENT,

		EVENT_COUNT
	};

	inline std::ostream& operator<<(std::ostream& os, EventType type) {
    	switch (type) {
        	case EventType::KEY_PRESS_EVENT:				return os << "KEY_PRESS_EVENT";
        	case EventType::KEY_RELEASE_EVENT:				return os << "KEY_RELEASE_EVENT";
        	case EventType::KEY_REPEAT_EVENT:				return os << "KEY_REPEAT_EVENT";
        	case EventType::MOUSE_MOVE_EVENT:				return os << "MOUSE_MOVE_EVENT";
        	case EventType::MOUSE_BUTTON_PRESS_EVENT:		return os << "MOUSE_BUTTON_PRESS_EVENT";
        	case EventType::MOUSE_BUTTON_RELEASE_EVENT:		return os << "MOUSE_BUTTON_RELEASE_EVENT";
        	case EventType::MOUSE_SCROLL_EVENT:				return os << "MOUSE_SCROLL_EVENT";
        	case EventType::NEW_FRAME_RENDERED_EVENT:		return os << "NEW_FRAME_RENDERED_EVENT";
			case EventType::UPDATE_RENDER_SETTINGS_EVENT:	return os << "UPDATE_RENDER_SETTINGS_EVENT";
			case EventType::WINDOW_RESIZE_EVENT:			return os << "WINDOW_RESIZE_EVENT";
        	case EventType::EVENT_COUNT:                	return os << "EVENT_COUNT";
        	default:										return os << "UNKNOWN_EVENT_TYPE";
    	}
	}

	struct IEvent {
		virtual ~IEvent() = default;
		virtual EventType GetType() const = 0;
		virtual inline bool IsInputEvent() const { return false; }

		inline void Consume() { m_IsConsumed = true; }
		inline bool IsConsumed() const { return m_IsConsumed; }
	private:
		bool m_IsConsumed = false;
	};
}
