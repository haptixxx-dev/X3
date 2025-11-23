#pragma once

#include <glm/glm.hpp>
#include "Core/Events/IEvent.h"

namespace Laura 
{

	struct WindowResizeEvent: public IEvent {
		glm::ivec2 windowSize;

		WindowResizeEvent(int width, int height) 
			: windowSize(glm::ivec2{width, height}) {}

		inline EventType GetType() const override { return EventType::WINDOW_RESIZE_EVENT; }
	};
}