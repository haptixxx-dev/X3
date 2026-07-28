#pragma once

#include "X3.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"
#include "EditorCameraController.h"

#include "Platform/Vulkan/VulkanImage.h"
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

		// NON-OWNING, from NewFrameRenderedEvent. The Renderer owns the image and
		// keeps the object address across recreate(), so this cannot dangle; what
		// changes underneath it is the generation, which the ImGui descriptor cache
		// below keys on.
		VulkanImage* m_LatestRenderedFrame = nullptr;

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
		VkSampler m_TextureSampler = VK_NULL_HANDLE;

		// ONE ENTRY PER LIVE IMAGE, not one entry total. The Renderer alternates
		// between FRAMES_IN_FLIGHT image slots, so a single cached descriptor was
		// invalidated and re-registered every single frame -- and
		// ImGui_ImplVulkan_RemoveTexture calls vkFreeDescriptorSets immediately,
		// on a set the previous frame's command buffer was still using
		// (VUID-vkFreeDescriptorSets-pDescriptorSets-00309).
		//
		// Keyed on VulkanImage::id(), which is unique across live images, never
		// reused and never 0 -- so this map is bounded at FRAMES_IN_FLIGHT entries
		// forever, regardless of how many resolution changes occur. generation() is
		// stored alongside because recreate() keeps the id and replaces the view.
		struct RegisteredTexture {
			uint64_t        generation = 0;
			VkDescriptorSet descriptor = VK_NULL_HANDLE;
		};
		std::unordered_map<uint64_t, RegisteredTexture> m_ImGuiTextures;

		void CleanupVulkanResources();
		ImTextureID GetImGuiTextureID(VulkanImage* image);
	};
}