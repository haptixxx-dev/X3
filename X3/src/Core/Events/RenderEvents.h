#pragma once

#include "Core/Events/IEvent.h"
#include "Renderer/IImage2D.h"
#include "Renderer/RenderSettings.h"
#include <glm/glm.hpp>

namespace X3
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

	struct UpdateEditorCameraEvent: public IEvent {
		bool useEditorCamera;
		glm::mat4 cameraTransform;
		float cameraFOV;

		UpdateEditorCameraEvent(bool useEditorCamera, const glm::mat4& transform, float fov)
			: useEditorCamera(useEditorCamera), cameraTransform(transform), cameraFOV(fov) {}

		inline EventType GetType() const override { return EventType::UPDATE_EDITOR_CAMERA_EVENT; }
	};
}
#pragma once

#include "Core/Events/IEvent.h"
#include "Renderer/IImage2D.h"
#include "Renderer/RenderSettings.h"

namespace X3 
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
