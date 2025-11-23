#pragma once

#include "Core/Events/IEvent.h"
#include "Renderer/IImage2D.h"
#include "Renderer/RenderSettings.h"

namespace Laura 
{

	struct NewFrameRenderedEvent : public IEvent {
		std::shared_ptr<IImage2D> frame;

		NewFrameRenderedEvent(std::shared_ptr<IImage2D> frame) 
			: frame(std::move(frame)) {}

		inline EventType GetType() const override { return EventType::NEW_FRAME_RENDERED_EVENT; }
	};

	struct UpdateRenderSettingsEvent: public IEvent {
		RenderSettings renderSettings;

		UpdateRenderSettingsEvent(RenderSettings renderSettings) 
			: renderSettings(std::move(renderSettings)) {}

		inline EventType GetType() const override { return EventType::UPDATE_RENDER_SETTINGS_EVENT; }
	};
}
