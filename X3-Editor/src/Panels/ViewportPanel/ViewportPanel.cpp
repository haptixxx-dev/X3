#include "ViewportPanel.h"
#include <IconsFontAwesome6.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "Panels/DNDPayloads.h"
#include "Project/Scene/SceneManager.h"
#include "Export/ExportSettings.h"
#include "Core/Events/RenderEvents.h"

namespace X3
{
	void ViewportPanel::DrawDropTargetForScene() {
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DNDPayloadTypes::SCENE)) {
				IM_ASSERT(payload->DataSize == sizeof(DNDPayload));
				auto& scenePayload = *static_cast<DNDPayload*>(payload->Data);
				if (m_ProjectManager->ProjectIsOpen()) {
					if (auto sceneManager = m_ProjectManager->GetSceneManager()) {
						sceneManager->SetOpenSceneGuid(scenePayload.guid);
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
	}

	void ViewportPanel::onEvent(std::shared_ptr<IEvent> event) {
		if (event->GetType() == EventType::NEW_FRAME_RENDERED_EVENT) {
			m_LatestRenderedFrame = std::dynamic_pointer_cast<NewFrameRenderedEvent>(event)->frame;
			return;
		}

		// Only handle input if editor camera is enabled
		if (!m_EditorState->temp.useEditorCamera) {
			return;
		}

		// Handle keyboard input
		if (event->GetType() == EventType::KEY_PRESS_EVENT) {
			auto keyEvent = std::dynamic_pointer_cast<KeyPressEvent>(event);
			m_EditorCamera.SetKeyState(keyEvent->key, true);
		}
		else if (event->GetType() == EventType::KEY_RELEASE_EVENT) {
			auto keyEvent = std::dynamic_pointer_cast<KeyReleaseEvent>(event);
			m_EditorCamera.SetKeyState(keyEvent->key, false);
		}
		// Handle mouse input
		else if (event->GetType() == EventType::MOUSE_MOVE_EVENT) {
			auto mouseEvent = std::dynamic_pointer_cast<MouseMoveEvent>(event);
			m_EditorCamera.OnMouseMove(mouseEvent->xpos, mouseEvent->ypos);
		}
		else if (event->GetType() == EventType::MOUSE_BUTTON_PRESS_EVENT) {
			auto mouseEvent = std::dynamic_pointer_cast<MouseButtonPressEvent>(event);
			m_EditorCamera.OnMouseButton(mouseEvent->button, true);
		}
		else if (event->GetType() == EventType::MOUSE_BUTTON_RELEASE_EVENT) {
			auto mouseEvent = std::dynamic_pointer_cast<MouseButtonReleaseEvent>(event);
			m_EditorCamera.OnMouseButton(mouseEvent->button, false);
		}
		else if (event->GetType() == EventType::MOUSE_SCROLL_EVENT) {
			auto mouseEvent = std::dynamic_pointer_cast<MouseScrollEvent>(event);
			m_EditorCamera.OnScroll(mouseEvent->yoffset);
		}
	}

	void ViewportPanel::OnImGuiRender() {
		static ImGuiWindowFlags ViewportFlags = ImGuiWindowFlags_NoCollapse;
		auto theme = m_EditorState->temp.editorTheme;

		// Update editor camera with delta time
		float currentTime = ImGui::GetTime();
		float deltaTime = currentTime - m_LastFrameTime;
		m_LastFrameTime = currentTime;

		if (m_EditorState->temp.useEditorCamera) {
			m_EditorCamera.Update(deltaTime);
		}

		// Dispatch editor camera event to renderer
		m_EventDispatcher->dispatchEvent(std::make_shared<UpdateEditorCameraEvent>(
			m_EditorState->temp.useEditorCamera,
			m_EditorCamera.GetTransformMatrix(),
			m_EditorCamera.GetFOV()
		));

		ImGuiStyle& style = ImGui::GetStyle();
		ImVec4 originalWindowBG = style.Colors[ImGuiCol_WindowBg];
		theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background2);
		
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 }); // remove the border padding
		ImGui::Begin(ICON_FA_EYE " Viewport", nullptr, ViewportFlags);

		// Track viewport hover and focus state
		m_ViewportHovered = ImGui::IsWindowHovered();
		m_ViewportFocused = ImGui::IsWindowFocused();

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::BeginDisabled();
		}

		ImGui::BeginChild("DropArea");
	
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		forceUpdate = false;

		DrawViewportSettingsPanel();

		auto latestRenderedFrameShared = m_LatestRenderedFrame.lock();
		if (latestRenderedFrameShared == nullptr) {
			DrawVieportSettingsButton();
			ImGui::EndChild();
			ImGui::PopStyleVar();
			DrawDropTargetForScene();
			
			if (m_EditorState->temp.isInRuntimeSimulation) {
				ImGui::EndDisabled();
			}
			
			ImGui::End();
			theme.PopColor();
			return;
		}
		
		ImageDimensions = latestRenderedFrameShared->GetDimensions();
		WindowDimensions = glm::ivec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
		TLWindowPosition = glm::ivec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y);

		bool DimensionsChanged = (ImageDimensions != m_PrevImageDimensions || WindowDimensions != m_PrevWindowDimensions);
		bool PositionChanged = (TLWindowPosition != m_PrevWindowPosition);

		if (m_EditorState->persistent.viewportMode == ScreenFitMode::OriginalCentered) {
			if (DimensionsChanged || PositionChanged || forceUpdate) {
				glm::ivec2 OffsetTopLeftCorner = (WindowDimensions - ImageDimensions) / 2;
				m_TopLeftImageCoords = OffsetTopLeftCorner + TLWindowPosition;
				m_BottomRightImageCoords = OffsetTopLeftCorner + TLWindowPosition + ImageDimensions;
			}
		}

		if (m_EditorState->persistent.viewportMode == ScreenFitMode::StretchFill) {
			if (DimensionsChanged || PositionChanged || forceUpdate) {
				m_TopLeftImageCoords = TLWindowPosition;
				m_BottomRightImageCoords = TLWindowPosition + WindowDimensions;
			}
		}

		if (m_EditorState->persistent.viewportMode == ScreenFitMode::MaxAspectFit) {
			// if the ViewportPanel has been resized or the renderer output image size has been changed

			if (DimensionsChanged || PositionChanged || forceUpdate) {
				if (DimensionsChanged || forceUpdate) {
					m_PrevWindowDimensions = WindowDimensions;
					m_PrevImageDimensions = ImageDimensions;

					float WindowAspectRatio = (float)WindowDimensions.x / (float)WindowDimensions.y;
					float ImageAspectRatio = (float)ImageDimensions.x / (float)ImageDimensions.y;
					// if true width is the limiting factor (spans the entire width)
					if (WindowAspectRatio <= ImageAspectRatio) {
						m_TargetImageDimensions.x = WindowDimensions.x;
						m_TargetImageDimensions.y = ceil(WindowDimensions.x / ImageAspectRatio);
					}
					else { // height is the limiting factor (spans the entire height)
						m_TargetImageDimensions.x = ceil(WindowDimensions.y * ImageAspectRatio);
						m_TargetImageDimensions.y = WindowDimensions.y;
					}
				}

				m_PrevWindowPosition = TLWindowPosition;

				m_TopLeftImageCoords.x = (WindowDimensions.x - m_TargetImageDimensions.x) / 2.0f;
				m_TopLeftImageCoords.y = (WindowDimensions.y - m_TargetImageDimensions.y) / 2.0f;

				// offset by the viewport panel's position
				m_TopLeftImageCoords.x += TLWindowPosition.x;
				m_TopLeftImageCoords.y += TLWindowPosition.y;

				m_BottomRightImageCoords.x = m_TopLeftImageCoords.x + m_TargetImageDimensions.x;
				m_BottomRightImageCoords.y = m_TopLeftImageCoords.y + m_TargetImageDimensions.y;
			}
		}

		// ImGui Handles scaling up the texture
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 TLImVec = ImVec2(m_TopLeftImageCoords.x, m_TopLeftImageCoords.y);
		ImVec2 BRImVec = ImVec2(m_BottomRightImageCoords.x, m_BottomRightImageCoords.y);
		drawList->AddImage((ImTextureID)latestRenderedFrameShared->GetID(), TLImVec, BRImVec, { 0, 1 }, { 1, 0 });

		// Draw gizmo on top of viewport
		DrawGizmo();

		// Draw viewport toolbar
		DrawViewportToolbar();

		DrawVieportSettingsButton();

		ImGui::EndChild();
		ImGui::PopStyleVar();
		DrawDropTargetForScene();

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::EndDisabled();
		}

		ImGui::End();
		theme.PopColor();
	}

	void ViewportPanel::DrawVieportSettingsButton() {
		auto theme = m_EditorState->temp.editorTheme;
		ImVec2 panelDims = ImGui::GetContentRegionAvail();
		float lineHeight = ImGui::GetFont()->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f;
		ImGui::Spacing();
		ImGui::SameLine(panelDims.x - lineHeight);
		theme.PushColor(ImGuiCol_Button, EditorCol_Background3);
		if (ImGui::Button(ICON_FA_ELLIPSIS_VERTICAL, { lineHeight, lineHeight })) {
			m_EditorState->temp.isViewportSettingsPanelOpen = true;
		}
		theme.PopColor(); // button
	}

	void ViewportPanel::DrawViewportSettingsPanel() {
		auto theme = m_EditorState->temp.editorTheme;
		if (!m_EditorState->temp.isViewportSettingsPanelOpen) {
			return;
		}

		static ImGuiWindowFlags ViewportSettingsFlags = ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoCollapse;
		
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
		theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background3);
		ImGui::SetNextWindowSizeConstraints({ 250.0f, 150.0f }, { FLT_MAX, FLT_MAX });
		ImGui::Begin(ICON_FA_EYE " VIEWPORT OPTIONS", &m_EditorState->temp.isViewportSettingsPanelOpen, ViewportSettingsFlags);

		ScreenFitMode currentMode = m_EditorState->persistent.viewportMode;
		const char* currentLabel = ScreenFitModeStr[static_cast<int>(currentMode)];
		theme.PushColor(ImGuiCol_FrameBg, EditorCol_Secondary1);
		ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		if (ImGui::BeginCombo("##Viewport Mode", currentLabel)) {
			for (int i = 0; i < static_cast<int>(ScreenFitMode::_COUNT); ++i) {
				bool selected = (i == static_cast<int>(currentMode));

				if (selected) { theme.PushColor(ImGuiCol_Header, EditorCol_Accent2); }
				if (ImGui::Selectable(ScreenFitModeStr[i], selected)) {
					m_EditorState->persistent.viewportMode = static_cast<ScreenFitMode>(i);
					forceUpdate = true;
				}
				if (selected) { theme.PopColor(); }
			}
			ImGui::EndCombo();
		}
		theme.PopColor();
		ImGui::PopStyleVar();
		
		ImGui::End();
		theme.PopColor();
		ImGui::PopStyleVar();
	}

	void ViewportPanel::DrawGizmo() {
		// Only draw gizmo if we have a selected entity and editor camera is active
		if (m_EditorState->temp.selectedEntity == entt::null || !m_EditorState->temp.useEditorCamera) {
			return;
		}

		// Check if project is open and get scene
		if (!m_ProjectManager->ProjectIsOpen()) {
			return;
		}

		auto sceneManager = m_ProjectManager->GetSceneManager();
		if (!sceneManager) {
			return;
		}

		auto scene = sceneManager->GetOpenScene();
		if (!scene) {
			return;
		}

		// Get selected entity
		EntityHandle entity(m_EditorState->temp.selectedEntity, scene->GetRegistry());
		if (!entity.HasComponent<TransformComponent>()) {
			return;
		}

		// Handle keyboard shortcuts for gizmo mode switching
		if (ImGui::IsKeyPressed(ImGuiKey_G)) {
			m_GizmoOperation = 7; // TRANSLATE
		}
		if (ImGui::IsKeyPressed(ImGuiKey_R)) {
			m_GizmoOperation = 120; // ROTATE
		}
		if (ImGui::IsKeyPressed(ImGuiKey_S) && !ImGui::GetIO().KeyCtrl) {
			m_GizmoOperation = 896; // SCALE
		}

		// Setup ImGuizmo
		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist();

		// Set ImGuizmo rect to match viewport image bounds
		ImGuizmo::SetRect(
			m_TopLeftImageCoords.x,
			m_TopLeftImageCoords.y,
			m_BottomRightImageCoords.x - m_TopLeftImageCoords.x,
			m_BottomRightImageCoords.y - m_TopLeftImageCoords.y
		);

		// Get camera matrices
		glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();
		float fov = m_EditorCamera.GetFOV();
		float aspectRatio = float(m_BottomRightImageCoords.x - m_TopLeftImageCoords.x) /
		                    float(m_BottomRightImageCoords.y - m_TopLeftImageCoords.y);
		glm::mat4 cameraProjection = glm::perspective(glm::radians(fov), aspectRatio, 0.1f, 1000.0f);

		// Get entity transform
		auto& transformComponent = entity.GetComponent<TransformComponent>();
		glm::mat4 transform = transformComponent.GetMatrix();

		// Draw and manipulate gizmo
		bool snap = m_EditorState->temp.snapToGrid;
		float snapValues[3] = {
			m_EditorState->temp.snapPositionValue,
			m_EditorState->temp.snapRotationValue,
			m_EditorState->temp.snapScaleValue
		};

		ImGuizmo::Manipulate(
			glm::value_ptr(cameraView),
			glm::value_ptr(cameraProjection),
			static_cast<ImGuizmo::OPERATION>(m_GizmoOperation),
			static_cast<ImGuizmo::MODE>(m_GizmoMode),
			glm::value_ptr(transform),
			nullptr,
			snap ? snapValues : nullptr
		);

		// If gizmo is being used, update the entity's transform
		if (ImGuizmo::IsUsing()) {
			// Decompose the transform matrix
			glm::vec3 translation, rotation, scale;
			ImGuizmo::DecomposeMatrixToComponents(
				glm::value_ptr(transform),
				glm::value_ptr(translation),
				glm::value_ptr(rotation),
				glm::value_ptr(scale)
			);

			// Update the transform component
			transformComponent.SetTranslation(translation);
			transformComponent.SetRotation(rotation); // ImGuizmo returns degrees
			transformComponent.SetScale(scale);
		}
	}

	void ViewportPanel::DrawViewportToolbar() {
		auto theme = m_EditorState->temp.editorTheme;

		// Position toolbar at top-left of viewport
		ImGui::SetCursorPos(ImVec2(10, 10));

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
		theme.PushColor(ImGuiCol_ChildBg, EditorCol_Background3);

		ImGui::BeginChild("##ViewportToolbar", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		// Editor Camera Toggle
		{
			bool useEditorCam = m_EditorState->temp.useEditorCamera;
			if (useEditorCam) {
				theme.PushColor(ImGuiCol_Button, EditorCol_Accent1);
			}

			if (ImGui::Button(useEditorCam ? ICON_FA_VIDEO " Editor Camera" : ICON_FA_VIDEO_SLASH " Scene Camera")) {
				m_EditorState->temp.useEditorCamera = !m_EditorState->temp.useEditorCamera;
			}

			if (useEditorCam) {
				theme.PopColor();
			}

			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Toggle between Editor Camera (free-fly) and Scene Camera\nShortcut: Hold RMB + WASD to navigate");
			}
		}

		ImGui::SameLine();
		ImGui::Separator();
		ImGui::SameLine();

		// Gizmo Mode Buttons (only show if editor camera is active)
		if (m_EditorState->temp.useEditorCamera) {
			// Translate button
			{
				bool isTranslate = (m_GizmoOperation == 7); // TRANSLATE
				if (isTranslate) {
					theme.PushColor(ImGuiCol_Button, EditorCol_Accent1);
				}

				if (ImGui::Button(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT)) {
					m_GizmoOperation = 7; // TRANSLATE
				}

				if (isTranslate) {
					theme.PopColor();
				}

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Translate Mode (G)");
				}
			}

			ImGui::SameLine();

			// Rotate button
			{
				bool isRotate = (m_GizmoOperation == 120); // ROTATE
				if (isRotate) {
					theme.PushColor(ImGuiCol_Button, EditorCol_Accent1);
				}

				if (ImGui::Button(ICON_FA_ROTATE)) {
					m_GizmoOperation = 120; // ROTATE
				}

				if (isRotate) {
					theme.PopColor();
				}

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Rotate Mode (R)");
				}
			}

			ImGui::SameLine();

			// Scale button
			{
				bool isScale = (m_GizmoOperation == 896); // SCALE
				if (isScale) {
					theme.PushColor(ImGuiCol_Button, EditorCol_Accent1);
				}

				if (ImGui::Button(ICON_FA_MAXIMIZE)) {
					m_GizmoOperation = 896; // SCALE
				}

				if (isScale) {
					theme.PopColor();
				}

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Scale Mode (S)");
				}
			}

			ImGui::SameLine();
			ImGui::Separator();
			ImGui::SameLine();

			// Local/World toggle
			{
				bool isLocal = (m_GizmoMode == 0); // LOCAL
				if (ImGui::Button(isLocal ? ICON_FA_CUBE " Local" : ICON_FA_GLOBE " World")) {
					m_GizmoMode = isLocal ? 1 : 0; // Toggle between LOCAL (0) and WORLD (1)
				}

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Toggle gizmo space (Local/World)");
				}
			}

			ImGui::SameLine();
			ImGui::Separator();
			ImGui::SameLine();

			// Grid Snap Toggle
			{
				bool snapEnabled = m_EditorState->temp.snapToGrid;
				if (snapEnabled) {
					theme.PushColor(ImGuiCol_Button, EditorCol_Accent1);
				}

				if (ImGui::Button(snapEnabled ? ICON_FA_MAGNET " Snap" : ICON_FA_MAGNET)) {
					m_EditorState->temp.snapToGrid = !m_EditorState->temp.snapToGrid;
				}

				if (snapEnabled) {
					theme.PopColor();
				}

				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("Toggle grid snapping");
				}
			}
		}

		// Camera Info (if editor camera is active)
		if (m_EditorState->temp.useEditorCamera) {
			ImGui::SameLine();
			ImGui::Separator();
			ImGui::SameLine();

			// Camera speed indicator
			ImGui::Text(ICON_FA_GAUGE " Speed: %.1f", m_EditorCamera.MovementSpeed);

			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Camera movement speed\nUse mouse scroll to adjust");
			}
		}

		ImGui::EndChild();
		theme.PopColor();
		ImGui::PopStyleVar(2);
	}
}
#include "ViewportPanel.h"
#include <IconsFontAwesome6.h>
#include <imgui_internal.h>
#include "Panels/DNDPayloads.h"
#include "Project/Scene/SceneManager.h"
#include "Export/ExportSettings.h"

namespace X3
{
	void ViewportPanel::DrawDropTargetForScene() {
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DNDPayloadTypes::SCENE)) {
				IM_ASSERT(payload->DataSize == sizeof(DNDPayload));
				auto& scenePayload = *static_cast<DNDPayload*>(payload->Data);
				if (m_ProjectManager->ProjectIsOpen()) {
					if (auto sceneManager = m_ProjectManager->GetSceneManager()) {
						sceneManager->SetOpenSceneGuid(scenePayload.guid);
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
	}

	void ViewportPanel::onEvent(std::shared_ptr<IEvent> event) {
		if (event->GetType() == EventType::NEW_FRAME_RENDERED_EVENT) {
			m_LatestRenderedFrame = std::dynamic_pointer_cast<NewFrameRenderedEvent>(event)->frame;
		}
	}

	void ViewportPanel::OnImGuiRender() {
		static ImGuiWindowFlags ViewportFlags = ImGuiWindowFlags_NoCollapse;
		auto theme = m_EditorState->temp.editorTheme;


		ImGuiStyle& style = ImGui::GetStyle();
		ImVec4 originalWindowBG = style.Colors[ImGuiCol_WindowBg];
		theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background2);
		
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0, 0 }); // remove the border padding
		ImGui::Begin(ICON_FA_EYE " Viewport", nullptr, ViewportFlags);
		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::BeginDisabled();
		}

		ImGui::BeginChild("DropArea");
	
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		forceUpdate = false;

		DrawViewportSettingsPanel();

		auto latestRenderedFrameShared = m_LatestRenderedFrame.lock();
		if (latestRenderedFrameShared == nullptr) {
			DrawVieportSettingsButton();
			ImGui::EndChild();
			ImGui::PopStyleVar();
			DrawDropTargetForScene();
			
			if (m_EditorState->temp.isInRuntimeSimulation) {
				ImGui::EndDisabled();
			}
			
			ImGui::End();
			theme.PopColor();
			return;
		}
		
		ImageDimensions = latestRenderedFrameShared->GetDimensions();
		WindowDimensions = glm::ivec2(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
		TLWindowPosition = glm::ivec2(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y);

		bool DimensionsChanged = (ImageDimensions != m_PrevImageDimensions || WindowDimensions != m_PrevWindowDimensions);
		bool PositionChanged = (TLWindowPosition != m_PrevWindowPosition);

		if (m_EditorState->persistent.viewportMode == ScreenFitMode::OriginalCentered) {
			if (DimensionsChanged || PositionChanged || forceUpdate) {
				glm::ivec2 OffsetTopLeftCorner = (WindowDimensions - ImageDimensions) / 2;
				m_TopLeftImageCoords = OffsetTopLeftCorner + TLWindowPosition;
				m_BottomRightImageCoords = OffsetTopLeftCorner + TLWindowPosition + ImageDimensions;
			}
		}

		if (m_EditorState->persistent.viewportMode == ScreenFitMode::StretchFill) {
			if (DimensionsChanged || PositionChanged || forceUpdate) {
				m_TopLeftImageCoords = TLWindowPosition;
				m_BottomRightImageCoords = TLWindowPosition + WindowDimensions;
			}
		}

		if (m_EditorState->persistent.viewportMode == ScreenFitMode::MaxAspectFit) {
			// if the ViewportPanel has been resized or the renderer output image size has been changed

			if (DimensionsChanged || PositionChanged || forceUpdate) {
				if (DimensionsChanged || forceUpdate) {
					m_PrevWindowDimensions = WindowDimensions;
					m_PrevImageDimensions = ImageDimensions;

					float WindowAspectRatio = (float)WindowDimensions.x / (float)WindowDimensions.y;
					float ImageAspectRatio = (float)ImageDimensions.x / (float)ImageDimensions.y;
					// if true width is the limiting factor (spans the entire width)
					if (WindowAspectRatio <= ImageAspectRatio) {
						m_TargetImageDimensions.x = WindowDimensions.x;
						m_TargetImageDimensions.y = ceil(WindowDimensions.x / ImageAspectRatio);
					}
					else { // height is the limiting factor (spans the entire height)
						m_TargetImageDimensions.x = ceil(WindowDimensions.y * ImageAspectRatio);
						m_TargetImageDimensions.y = WindowDimensions.y;
					}
				}

				m_PrevWindowPosition = TLWindowPosition;

				m_TopLeftImageCoords.x = (WindowDimensions.x - m_TargetImageDimensions.x) / 2.0f;
				m_TopLeftImageCoords.y = (WindowDimensions.y - m_TargetImageDimensions.y) / 2.0f;

				// offset by the viewport panel's position
				m_TopLeftImageCoords.x += TLWindowPosition.x;
				m_TopLeftImageCoords.y += TLWindowPosition.y;

				m_BottomRightImageCoords.x = m_TopLeftImageCoords.x + m_TargetImageDimensions.x;
				m_BottomRightImageCoords.y = m_TopLeftImageCoords.y + m_TargetImageDimensions.y;
			}
		}

		// ImGui Handles scaling up the texture			
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 TLImVec = ImVec2(m_TopLeftImageCoords.x, m_TopLeftImageCoords.y);
		ImVec2 BRImVec = ImVec2(m_BottomRightImageCoords.x, m_BottomRightImageCoords.y);
		drawList->AddImage((ImTextureID)latestRenderedFrameShared->GetID(), TLImVec, BRImVec, { 0, 1 }, { 1, 0 });

		DrawVieportSettingsButton();

		ImGui::EndChild();
		ImGui::PopStyleVar();
		DrawDropTargetForScene();

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::EndDisabled();
		}

		ImGui::End();
		theme.PopColor();
	}

	void ViewportPanel::DrawVieportSettingsButton() {
		auto theme = m_EditorState->temp.editorTheme;
		ImVec2 panelDims = ImGui::GetContentRegionAvail();
		float lineHeight = ImGui::GetFont()->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f;
		ImGui::Spacing();
		ImGui::SameLine(panelDims.x - lineHeight);
		theme.PushColor(ImGuiCol_Button, EditorCol_Background3);
		if (ImGui::Button(ICON_FA_ELLIPSIS_VERTICAL, { lineHeight, lineHeight })) {
			m_EditorState->temp.isViewportSettingsPanelOpen = true;
		}
		theme.PopColor(); // button
	}

	void ViewportPanel::DrawViewportSettingsPanel() {
		auto theme = m_EditorState->temp.editorTheme;
		if (!m_EditorState->temp.isViewportSettingsPanelOpen) {
			return;
		}

		static ImGuiWindowFlags ViewportSettingsFlags = ImGuiWindowFlags_NoDocking |
			ImGuiWindowFlags_NoCollapse;
		
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
		theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background3);
		ImGui::SetNextWindowSizeConstraints({ 250.0f, 150.0f }, { FLT_MAX, FLT_MAX });
		ImGui::Begin(ICON_FA_EYE " VIEWPORT OPTIONS", &m_EditorState->temp.isViewportSettingsPanelOpen, ViewportSettingsFlags);

		ScreenFitMode currentMode = m_EditorState->persistent.viewportMode;
		const char* currentLabel = ScreenFitModeStr[static_cast<int>(currentMode)];
		theme.PushColor(ImGuiCol_FrameBg, EditorCol_Secondary1);
		ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		if (ImGui::BeginCombo("##Viewport Mode", currentLabel)) {
			for (int i = 0; i < static_cast<int>(ScreenFitMode::_COUNT); ++i) {
				bool selected = (i == static_cast<int>(currentMode));

				if (selected) { theme.PushColor(ImGuiCol_Header, EditorCol_Accent2); }
				if (ImGui::Selectable(ScreenFitModeStr[i], selected)) {
					m_EditorState->persistent.viewportMode = static_cast<ScreenFitMode>(i);
					forceUpdate = true;
				}
				if (selected) { theme.PopColor(); }
			}
			ImGui::EndCombo();
		}
		theme.PopColor();
		ImGui::PopStyleVar();
		
		ImGui::End();
		theme.PopColor();
		ImGui::PopStyleVar();
	}
}
