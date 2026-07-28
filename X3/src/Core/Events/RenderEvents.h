#pragma once

#include "Core/Events/IEvent.h"
#include "Renderer/RenderSettings.h"
#include <glm/glm.hpp>

namespace X3
{
	class VulkanImage;

	// NON-OWNING. The Renderer owns its images by value for its whole life and
	// recreate() keeps the object address, so this pointer does not dangle -- but
	// the CONTENTS belong to the frame the event was dispatched in. A consumer
	// that holds it across frames must re-check generation() before reusing
	// anything derived from it, which is what the editor's ImGui descriptor cache
	// does. nullptr means the frame produced no image (no camera).
	struct NewFrameRenderedEvent : public IEvent {
		VulkanImage* frame;

		explicit NewFrameRenderedEvent(VulkanImage* frame)
			: frame(frame) {}

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