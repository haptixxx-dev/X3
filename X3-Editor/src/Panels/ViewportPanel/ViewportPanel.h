#pragma once

#include "X3.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"
#include "EditorCameraController.h"

#include <vulkan/vulkan.h>

namespace X3
{

	class ViewportPanel : public IEditorPanel {
	public:
		ViewportPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager,
			std::shared_ptr<IEventDispatcher> eventDispatcher)
			: m_EditorState(editorState), m_ProjectManager(projectManager), m_EventDispatcher(eventDispatcher) {}
		~ViewportPanel();

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual void onEvent(std::shared_ptr<IEvent> event) override;

		// Editor camera access
		EditorCameraController& GetEditorCamera() { return m_EditorCamera; }

	private:
		void DrawDropTargetForScene();
		void DrawViewportSettingsPanel();
		void DrawVieportSettingsButton();
		void DrawGizmo();
		void DrawViewportToolbar();
		void DrawPhysicsDebug();

		// Helper to create projection matrix matching shader's ray generation
		glm::mat4 CreateShaderMatchingProjection(float fov, float aspectRatio, float nearPlane, float farPlane);

		// Helper to project 3D world point to 2D screen coordinates
		bool WorldToScreen(const glm::vec3& worldPos, const glm::mat4& viewProj, glm::vec2& screenPos);
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher;

		std::weak_ptr<IImage2D> m_LatestRenderedFrame;

		glm::ivec2 m_TargetImageDimensions, m_PrevImageDimensions, m_PrevWindowDimensions;
		glm::ivec2 m_PrevWindowPosition, m_TopLeftImageCoords, m_BottomRightImageCoords;
		glm::ivec2 ImageDimensions, WindowDimensions, TLWindowPosition;
		bool forceUpdate;

		// Editor camera
		EditorCameraController m_EditorCamera;
		float m_LastFrameTime = 0.0f;
		bool m_ViewportHovered = false;
		bool m_ViewportFocused = false;

		// Gizmo state
		int m_GizmoOperation = 7; // ImGuizmo::OPERATION::TRANSLATE
		int m_GizmoMode = 0;      // ImGuizmo::MODE::LOCAL

		// Vulkan ImGui texture registration
		VkDescriptorSet m_ImGuiTextureDescriptor = VK_NULL_HANDLE;
		VkSampler m_TextureSampler = VK_NULL_HANDLE;
		int m_LastRegisteredImageID = -1;

		void CleanupVulkanResources();
		ImTextureID GetImGuiTextureID(std::shared_ptr<IImage2D> image);
	};
}