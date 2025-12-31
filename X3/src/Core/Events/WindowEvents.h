#pragma once

#include <glm/glm.hpp>
#include "Core/Events/IEvent.h"

namespace X3 
{

	struct WindowResizeEvent: public IEvent {
		glm::ivec2 windowSize;

		WindowResizeEvent(int width, int height)
			: windowSize(glm::ivec2{width, height}) {}

		inline EventType GetType() const override { return EventType::WINDOW_RESIZE_EVENT; }
	};

	struct SetVSyncEvent : public IEvent {
		bool enabled;

		SetVSyncEvent(bool enabled) : enabled(enabled) {}

		inline EventType GetType() const override { return EventType::SET_VSYNC_EVENT; }
	};
}