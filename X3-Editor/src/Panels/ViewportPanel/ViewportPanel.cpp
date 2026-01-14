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

#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanImage2D.h"
#include "Platform/Vulkan/VulkanContext.h"
#include <imgui_impl_vulkan.h>
#endif

namespace X3
{
	ViewportPanel::~ViewportPanel() {
#ifdef X3_USE_VULKAN
		CleanupVulkanResources();
#endif
	}

#ifdef X3_USE_VULKAN
	void ViewportPanel::CleanupVulkanResources() {
		auto context = VulkanContext::Get();
		if (!context) return;

		VkDevice device = context->getDevice();
		vkDeviceWaitIdle(device);

		if (m_ImGuiTextureDescriptor != VK_NULL_HANDLE) {
			ImGui_ImplVulkan_RemoveTexture(m_ImGuiTextureDescriptor);
			m_ImGuiTextureDescriptor = VK_NULL_HANDLE;
		}

		if (m_TextureSampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, m_TextureSampler, nullptr);
			m_TextureSampler = VK_NULL_HANDLE;
		}
	}

	ImTextureID ViewportPanel::GetImGuiTextureID(std::shared_ptr<IImage2D> image) {
		if (!image) return nullptr;

		// Check if we need to re-register (different image or first time)
		int currentImageID = image->GetID();
		if (currentImageID == m_LastRegisteredImageID && m_ImGuiTextureDescriptor != VK_NULL_HANDLE) {
			return m_ImGuiTextureDescriptor;
		}

		auto context = VulkanContext::Get();
		if (!context) return nullptr;

		// Cast to VulkanImage2D to get the image view
		auto vulkanImage = std::dynamic_pointer_cast<VulkanImage2D>(image);
		if (!vulkanImage) return nullptr;

		VkDevice device = context->getDevice();

		// Clean up old resources if re-registering
		if (m_ImGuiTextureDescriptor != VK_NULL_HANDLE) {
			ImGui_ImplVulkan_RemoveTexture(m_ImGuiTextureDescriptor);
			m_ImGuiTextureDescriptor = VK_NULL_HANDLE;
		}

		// Create sampler if needed (only once)
		if (m_TextureSampler == VK_NULL_HANDLE) {
			VkSamplerCreateInfo samplerInfo{};
			samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			samplerInfo.magFilter = VK_FILTER_LINEAR;
			samplerInfo.minFilter = VK_FILTER_LINEAR;
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.anisotropyEnable = VK_FALSE;
			samplerInfo.maxAnisotropy = 1.0f;
			samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
			samplerInfo.unnormalizedCoordinates = VK_FALSE;
			samplerInfo.compareEnable = VK_FALSE;
			samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
			samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

			if (vkCreateSampler(device, &samplerInfo, nullptr, &m_TextureSampler) != VK_SUCCESS) {
				LOG_EDITOR_ERROR("Failed to create texture sampler for viewport");
				return nullptr;
			}
		}

		// Register with ImGui - use GENERAL layout since that's what the compute shader uses
		m_ImGuiTextureDescriptor = ImGui_ImplVulkan_AddTexture(
			m_TextureSampler,
			vulkanImage->getImageView(),
			VK_IMAGE_LAYOUT_GENERAL
		);

		m_LastRegisteredImageID = currentImageID;

		return m_ImGuiTextureDescriptor;
	}
#endif
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
			// when last checked, frames were valid
			//LOG_EDITOR_INFO("ViewportPanel received frame: {}", (m_LatestRenderedFrame.lock() != nullptr) ? "valid" : "null");
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

		// Draw viewport toolbar at top of window (before child area)
		DrawViewportToolbar();

		ImGui::BeginChild("DropArea");

		ImGuiWindow* window = ImGui::GetCurrentWindow();
		forceUpdate = false;

		DrawViewportSettingsPanel();

		auto latestRenderedFrameShared = m_LatestRenderedFrame.lock();
		if (latestRenderedFrameShared == nullptr) {
			LOG_EDITOR_ERROR("Last Rendered Frame was a nullptr.. something has gone wrong");
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

#ifdef X3_USE_VULKAN
		// Vulkan requires proper descriptor set registration with ImGui
		ImTextureID textureID = GetImGuiTextureID(latestRenderedFrameShared);
		if (textureID) {
			drawList->AddImage(textureID, TLImVec, BRImVec, { 0, 1 }, { 1, 0 });
		}
#else
		// OpenGL can use the texture ID directly
		drawList->AddImage((ImTextureID)(intptr_t)latestRenderedFrameShared->GetID(), TLImVec, BRImVec, { 0, 1 }, { 1, 0 });
#endif

		// Draw gizmo on top of viewport
		DrawGizmo();

		// Draw physics debug visualization
		DrawPhysicsDebug();

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
		// Only process shortcuts when viewport is hovered and RMB is not pressed (to avoid conflict with camera movement)
		bool canUseShortcuts = m_ViewportHovered && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
		if (canUseShortcuts) {
			if (ImGui::IsKeyPressed(ImGuiKey_G)) {
				m_GizmoOperation = 7; // TRANSLATE
			}
			if (ImGui::IsKeyPressed(ImGuiKey_R)) {
				m_GizmoOperation = 120; // ROTATE
			}
			if (ImGui::IsKeyPressed(ImGuiKey_S) && !ImGui::GetIO().KeyCtrl) {
				m_GizmoOperation = 896; // SCALE
			}
		}

		// Setup ImGuizmo
		ImGuizmo::Enable(true);
		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist();

		// Set ImGuizmo rect to match viewport image bounds
		ImGuizmo::SetRect(
			m_TopLeftImageCoords.x,
			m_TopLeftImageCoords.y,
			m_BottomRightImageCoords.x - m_TopLeftImageCoords.x,
			m_BottomRightImageCoords.y - m_TopLeftImageCoords.y
		);

		// Get camera matrices using custom projection that matches shader's ray generation
		glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();
		float fov = m_EditorCamera.GetFOV();
		float aspectRatio = float(m_BottomRightImageCoords.x - m_TopLeftImageCoords.x) /
		                    float(m_BottomRightImageCoords.y - m_TopLeftImageCoords.y);
		glm::mat4 cameraProjection = CreateShaderMatchingProjection(fov, aspectRatio, 0.1f, 1000.0f);

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
				ImGui::SetTooltip("Camera movement speed\nShift + Scroll to adjust (0.5 - 100)");
			}
		}

		ImGui::EndChild();
		theme.PopColor();
		ImGui::PopStyleVar(2);
	}

	glm::mat4 ViewportPanel::CreateShaderMatchingProjection(float fov, float aspectRatio, float nearPlane, float farPlane) {
		// Create projection matrix that matches the shader's ray generation:
		//   x = (texelCoords.x * 2 - dims.x) / dims.x  → range [-1, 1]
		//   y = (texelCoords.y * 2 - dims.y) / dims.x  → range [-1/aspect, 1/aspect]
		//   rayDir = normalize(x, y, focalLength)
		//
		// The shader applies aspect ratio to Y (divides by width), while standard
		// glm::perspective applies it to X. This custom matrix matches the shader.

		float focalLength = 1.0f / glm::tan(glm::radians(fov) * 0.5f);

		glm::mat4 proj(0.0f);
		proj[0][0] = focalLength;                              // X scaling
		proj[1][1] = focalLength * aspectRatio;                // Y scaling (with aspect ratio)
		proj[2][2] = (farPlane + nearPlane) / (farPlane - nearPlane);  // Depth mapping
		proj[2][3] = 1.0f;                                     // w = z (shader uses +Z forward)
		proj[3][2] = -2.0f * farPlane * nearPlane / (farPlane - nearPlane);

		return proj;
	}

	bool ViewportPanel::WorldToScreen(const glm::vec3& worldPos, const glm::mat4& viewProj, glm::vec2& screenPos) {
		glm::vec4 clipPos = viewProj * glm::vec4(worldPos, 1.0f);

		// Check if point is behind camera
		if (clipPos.w <= 0.0f) {
			return false;
		}

		// Perspective divide
		glm::vec3 ndcPos = glm::vec3(clipPos) / clipPos.w;

		// Convert to screen coordinates
		// Matches shader's texel coordinate formula
		// Note: We don't bounds-check here - ImGui clips lines automatically
		float viewportWidth = m_BottomRightImageCoords.x - m_TopLeftImageCoords.x;
		float viewportHeight = m_BottomRightImageCoords.y - m_TopLeftImageCoords.y;

		screenPos.x = m_TopLeftImageCoords.x + (ndcPos.x + 1.0f) * 0.5f * viewportWidth;
		screenPos.y = m_TopLeftImageCoords.y + (1.0f - ndcPos.y) * 0.5f * viewportHeight;

		return true;
	}

	void ViewportPanel::DrawPhysicsDebug() {
		// Check if debug visualization is enabled
		if (!m_EditorState->temp.showPhysicsDebug) {
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

		// Get camera matrices using custom projection that matches shader's ray generation
		glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();
		float fov = m_EditorCamera.GetFOV();
		float aspectRatio = float(m_BottomRightImageCoords.x - m_TopLeftImageCoords.x) /
		                    float(m_BottomRightImageCoords.y - m_TopLeftImageCoords.y);
		glm::mat4 cameraProjection = CreateShaderMatchingProjection(fov, aspectRatio, 0.1f, 1000.0f);
		glm::mat4 viewProj = cameraProjection * cameraView;

		ImDrawList* drawList = ImGui::GetWindowDrawList();

		// Iterate through all entities with ColliderComponent
		auto* registry = scene->GetRegistry();
		auto view = registry->view<TransformComponent, ColliderComponent>();

		for (auto entityId : view) {
			EntityHandle entity(entityId, registry);
			auto& transform = entity.GetComponent<TransformComponent>();
			auto& collider = entity.GetComponent<ColliderComponent>();

			// Get world transform
			glm::mat4 worldMatrix = transform.GetMatrix();
			glm::vec3 worldPos = glm::vec3(worldMatrix[3]);

			// Determine color based on trigger status
			glm::vec4 color = collider.isTrigger
				? m_EditorState->temp.triggerWireframeColor
				: m_EditorState->temp.colliderWireframeColor;
			ImU32 imColor = IM_COL32(
				static_cast<int>(color.r * 255),
				static_cast<int>(color.g * 255),
				static_cast<int>(color.b * 255),
				static_cast<int>(color.a * 255)
			);

			// Apply collider offset
			glm::vec3 offset = collider.offset;
			glm::vec3 colliderCenter = worldPos + glm::mat3(worldMatrix) * offset;

			// Get rotation matrix from entity transform (without scale)
			glm::mat3 rotationMatrix = glm::mat3(worldMatrix);
			glm::vec3 scale = transform.GetScale();
			// Remove scale from rotation matrix to get pure rotation
			rotationMatrix[0] /= scale.x;
			rotationMatrix[1] /= scale.y;
			rotationMatrix[2] /= scale.z;

			if (m_EditorState->temp.showColliderWireframes) {
				switch (collider.shape) {
					case ColliderShape::Box: {
						// Draw box wireframe
						glm::vec3 halfExtents = collider.boxHalfExtents * scale;

						// Define 8 corners of the box in local space, then rotate to world space
						glm::vec3 localCorners[8] = {
							glm::vec3(-halfExtents.x, -halfExtents.y, -halfExtents.z),
							glm::vec3( halfExtents.x, -halfExtents.y, -halfExtents.z),
							glm::vec3( halfExtents.x,  halfExtents.y, -halfExtents.z),
							glm::vec3(-halfExtents.x,  halfExtents.y, -halfExtents.z),
							glm::vec3(-halfExtents.x, -halfExtents.y,  halfExtents.z),
							glm::vec3( halfExtents.x, -halfExtents.y,  halfExtents.z),
							glm::vec3( halfExtents.x,  halfExtents.y,  halfExtents.z),
							glm::vec3(-halfExtents.x,  halfExtents.y,  halfExtents.z)
						};

						glm::vec3 corners[8];
						for (int i = 0; i < 8; i++) {
							corners[i] = colliderCenter + rotationMatrix * localCorners[i];
						}

						// Project corners to screen
						glm::vec2 screenCorners[8];
						bool cornerVisible[8];
						for (int i = 0; i < 8; i++) {
							cornerVisible[i] = WorldToScreen(corners[i], viewProj, screenCorners[i]);
						}

						// Draw 12 edges of the box (only if both endpoints are in front of camera)
						int edges[12][2] = {
							{0, 1}, {1, 2}, {2, 3}, {3, 0}, // Front face
							{4, 5}, {5, 6}, {6, 7}, {7, 4}, // Back face
							{0, 4}, {1, 5}, {2, 6}, {3, 7}  // Connecting edges
						};

						for (int i = 0; i < 12; i++) {
							int a = edges[i][0], b = edges[i][1];
							if (cornerVisible[a] && cornerVisible[b]) {
								drawList->AddLine(
									ImVec2(screenCorners[a].x, screenCorners[a].y),
									ImVec2(screenCorners[b].x, screenCorners[b].y),
									imColor, 2.0f
								);
							}
						}
						break;
					}

					case ColliderShape::Sphere: {
						// Draw sphere as circles in 3 planes (rotated with entity)
						float radius = collider.sphereRadius * glm::max(glm::max(scale.x, scale.y), scale.z);
						int segments = 24;

						// Get rotated axes
						glm::vec3 axisX = rotationMatrix * glm::vec3(1, 0, 0);
						glm::vec3 axisY = rotationMatrix * glm::vec3(0, 1, 0);
						glm::vec3 axisZ = rotationMatrix * glm::vec3(0, 0, 1);

						// Draw circle in XY plane (around Z axis)
						for (int i = 0; i < segments; i++) {
							float angle1 = (float)i / segments * 2.0f * glm::pi<float>();
							float angle2 = (float)(i + 1) / segments * 2.0f * glm::pi<float>();

							glm::vec3 p1 = colliderCenter + (cos(angle1) * axisX + sin(angle1) * axisY) * radius;
							glm::vec3 p2 = colliderCenter + (cos(angle2) * axisX + sin(angle2) * axisY) * radius;

							glm::vec2 s1, s2;
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}
						}

						// Draw circle in XZ plane (around Y axis)
						for (int i = 0; i < segments; i++) {
							float angle1 = (float)i / segments * 2.0f * glm::pi<float>();
							float angle2 = (float)(i + 1) / segments * 2.0f * glm::pi<float>();

							glm::vec3 p1 = colliderCenter + (cos(angle1) * axisX + sin(angle1) * axisZ) * radius;
							glm::vec3 p2 = colliderCenter + (cos(angle2) * axisX + sin(angle2) * axisZ) * radius;

							glm::vec2 s1, s2;
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}
						}

						// Draw circle in YZ plane (around X axis)
						for (int i = 0; i < segments; i++) {
							float angle1 = (float)i / segments * 2.0f * glm::pi<float>();
							float angle2 = (float)(i + 1) / segments * 2.0f * glm::pi<float>();

							glm::vec3 p1 = colliderCenter + (cos(angle1) * axisY + sin(angle1) * axisZ) * radius;
							glm::vec3 p2 = colliderCenter + (cos(angle2) * axisY + sin(angle2) * axisZ) * radius;

							glm::vec2 s1, s2;
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}
						}
						break;
					}

					case ColliderShape::Capsule: {
						// Draw capsule as cylinder with hemispherical caps
						float radius = collider.capsuleRadius;
						float halfHeight = collider.capsuleHalfHeight;
						int segments = 16;

						// Scale
						glm::vec3 scale = transform.GetScale();
						radius *= glm::max(scale.x, scale.z);
						halfHeight *= scale.y;

						// Draw top and bottom circles
						for (int circle = 0; circle < 2; circle++) {
							float yOffset = (circle == 0) ? halfHeight : -halfHeight;

							for (int i = 0; i < segments; i++) {
								float angle1 = (float)i / segments * 2.0f * glm::pi<float>();
								float angle2 = (float)(i + 1) / segments * 2.0f * glm::pi<float>();

								glm::vec3 p1 = colliderCenter + glm::vec3(cos(angle1) * radius, yOffset, sin(angle1) * radius);
								glm::vec3 p2 = colliderCenter + glm::vec3(cos(angle2) * radius, yOffset, sin(angle2) * radius);

								glm::vec2 s1, s2;
								if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
									drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
								}
							}
						}

						// Draw vertical lines connecting the circles
						for (int i = 0; i < 4; i++) {
							float angle = (float)i / 4 * 2.0f * glm::pi<float>();

							glm::vec3 p1 = colliderCenter + glm::vec3(cos(angle) * radius, halfHeight, sin(angle) * radius);
							glm::vec3 p2 = colliderCenter + glm::vec3(cos(angle) * radius, -halfHeight, sin(angle) * radius);

							glm::vec2 s1, s2;
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}
						}

						// Draw hemisphere arcs at top
						for (int i = 0; i < segments / 2; i++) {
							float angle1 = (float)i / segments * glm::pi<float>();
							float angle2 = (float)(i + 1) / segments * glm::pi<float>();

							// XY arc
							glm::vec3 p1 = colliderCenter + glm::vec3(sin(angle1) * radius, halfHeight + cos(angle1) * radius, 0);
							glm::vec3 p2 = colliderCenter + glm::vec3(sin(angle2) * radius, halfHeight + cos(angle2) * radius, 0);
							glm::vec2 s1, s2;
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}

							// ZY arc
							p1 = colliderCenter + glm::vec3(0, halfHeight + cos(angle1) * radius, sin(angle1) * radius);
							p2 = colliderCenter + glm::vec3(0, halfHeight + cos(angle2) * radius, sin(angle2) * radius);
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}
						}

						// Draw hemisphere arcs at bottom
						for (int i = 0; i < segments / 2; i++) {
							float angle1 = (float)i / segments * glm::pi<float>();
							float angle2 = (float)(i + 1) / segments * glm::pi<float>();

							// XY arc
							glm::vec3 p1 = colliderCenter + glm::vec3(sin(angle1) * radius, -halfHeight - cos(angle1) * radius, 0);
							glm::vec3 p2 = colliderCenter + glm::vec3(sin(angle2) * radius, -halfHeight - cos(angle2) * radius, 0);
							glm::vec2 s1, s2;
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}

							// ZY arc
							p1 = colliderCenter + glm::vec3(0, -halfHeight - cos(angle1) * radius, sin(angle1) * radius);
							p2 = colliderCenter + glm::vec3(0, -halfHeight - cos(angle2) * radius, sin(angle2) * radius);
							if (WorldToScreen(p1, viewProj, s1) && WorldToScreen(p2, viewProj, s2)) {
								drawList->AddLine(ImVec2(s1.x, s1.y), ImVec2(s2.x, s2.y), imColor, 2.0f);
							}
						}
						break;
					}

					default:
						// ConvexMesh, TriangleMesh, Heightfield - show center point only
						glm::vec2 screenPos;
						if (WorldToScreen(colliderCenter, viewProj, screenPos)) {
							drawList->AddCircleFilled(ImVec2(screenPos.x, screenPos.y), 5.0f, imColor);
						}
						break;
				}
			}

			// Draw AABBs if enabled
			if (m_EditorState->temp.showColliderAABBs) {
				glm::vec3 aabbMin, aabbMax;

				switch (collider.shape) {
					case ColliderShape::Box: {
						// For rotated boxes, compute AABB from rotated corner positions
						glm::vec3 halfExtents = collider.boxHalfExtents * scale;
						glm::vec3 localCorners[8] = {
							glm::vec3(-halfExtents.x, -halfExtents.y, -halfExtents.z),
							glm::vec3( halfExtents.x, -halfExtents.y, -halfExtents.z),
							glm::vec3( halfExtents.x,  halfExtents.y, -halfExtents.z),
							glm::vec3(-halfExtents.x,  halfExtents.y, -halfExtents.z),
							glm::vec3(-halfExtents.x, -halfExtents.y,  halfExtents.z),
							glm::vec3( halfExtents.x, -halfExtents.y,  halfExtents.z),
							glm::vec3( halfExtents.x,  halfExtents.y,  halfExtents.z),
							glm::vec3(-halfExtents.x,  halfExtents.y,  halfExtents.z)
						};

						aabbMin = glm::vec3(FLT_MAX);
						aabbMax = glm::vec3(-FLT_MAX);
						for (int i = 0; i < 8; i++) {
							glm::vec3 worldCorner = colliderCenter + rotationMatrix * localCorners[i];
							aabbMin = glm::min(aabbMin, worldCorner);
							aabbMax = glm::max(aabbMax, worldCorner);
						}
						break;
					}
					case ColliderShape::Sphere: {
						// Sphere AABB is rotation-invariant
						float radius = collider.sphereRadius * glm::max(glm::max(scale.x, scale.y), scale.z);
						aabbMin = colliderCenter - glm::vec3(radius);
						aabbMax = colliderCenter + glm::vec3(radius);
						break;
					}
					case ColliderShape::Capsule: {
						// For rotated capsules, compute endpoints in world space and expand by radius
						float radius = collider.capsuleRadius * glm::max(scale.x, scale.z);
						float halfHeight = collider.capsuleHalfHeight * scale.y;

						// Capsule extends along local Y axis
						glm::vec3 localUp = glm::vec3(0, halfHeight, 0);
						glm::vec3 worldUp = rotationMatrix * localUp;

						glm::vec3 topCenter = colliderCenter + worldUp;
						glm::vec3 bottomCenter = colliderCenter - worldUp;

						// AABB is min/max of both sphere centers, expanded by radius
						aabbMin = glm::min(topCenter, bottomCenter) - glm::vec3(radius);
						aabbMax = glm::max(topCenter, bottomCenter) + glm::vec3(radius);
						break;
					}
					default:
						continue;
				}

				// Draw AABB as dashed box
				glm::vec3 aabbCorners[8] = {
					{aabbMin.x, aabbMin.y, aabbMin.z},
					{aabbMax.x, aabbMin.y, aabbMin.z},
					{aabbMax.x, aabbMax.y, aabbMin.z},
					{aabbMin.x, aabbMax.y, aabbMin.z},
					{aabbMin.x, aabbMin.y, aabbMax.z},
					{aabbMax.x, aabbMin.y, aabbMax.z},
					{aabbMax.x, aabbMax.y, aabbMax.z},
					{aabbMin.x, aabbMax.y, aabbMax.z}
				};

				glm::vec2 screenCorners[8];
				bool cornerVisible[8];
				for (int i = 0; i < 8; i++) {
					cornerVisible[i] = WorldToScreen(aabbCorners[i], viewProj, screenCorners[i]);
				}

				ImU32 aabbColor = IM_COL32(255, 255, 0, 128); // Yellow, semi-transparent
				int edges[12][2] = {
					{0, 1}, {1, 2}, {2, 3}, {3, 0},
					{4, 5}, {5, 6}, {6, 7}, {7, 4},
					{0, 4}, {1, 5}, {2, 6}, {3, 7}
				};

				for (int i = 0; i < 12; i++) {
					int a = edges[i][0], b = edges[i][1];
					if (cornerVisible[a] && cornerVisible[b]) {
						drawList->AddLine(
							ImVec2(screenCorners[a].x, screenCorners[a].y),
							ImVec2(screenCorners[b].x, screenCorners[b].y),
							aabbColor, 1.0f
						);
					}
				}
			}
		}

		// Draw velocity vectors for dynamic bodies
		if (m_EditorState->temp.showVelocityVectors) {
			glm::vec4 velColor = m_EditorState->temp.velocityVectorColor;
			ImU32 imVelColor = IM_COL32(
				static_cast<int>(velColor.r * 255),
				static_cast<int>(velColor.g * 255),
				static_cast<int>(velColor.b * 255),
				static_cast<int>(velColor.a * 255)
			);

			auto rigidBodyView = registry->view<TransformComponent, RigidBodyComponent>();
			for (auto entityId : rigidBodyView) {
				auto& rb = rigidBodyView.get<RigidBodyComponent>(entityId);
				if (rb.bodyType != BodyType::Dynamic) continue;

				auto& transform = rigidBodyView.get<TransformComponent>(entityId);
				glm::vec3 pos = transform.GetTranslation();

				// For visualization purposes, show a scaled velocity arrow
				// In runtime, actual velocity would come from physics world
				glm::vec3 velocityDir = glm::vec3(0, 0, 0);  // Would get from physics in runtime
				float velocityMag = glm::length(velocityDir);

				if (velocityMag > 0.1f) {
					glm::vec3 endPos = pos + velocityDir * 0.5f;  // Scale for visibility

					glm::vec2 startScreen, endScreen;
					if (WorldToScreen(pos, viewProj, startScreen) && WorldToScreen(endPos, viewProj, endScreen)) {
						drawList->AddLine(
							ImVec2(startScreen.x, startScreen.y),
							ImVec2(endScreen.x, endScreen.y),
							imVelColor, 2.0f
						);

						// Draw arrowhead
						glm::vec2 dir = glm::normalize(endScreen - startScreen);
						glm::vec2 perp(-dir.y, dir.x);
						float arrowSize = 8.0f;

						drawList->AddTriangleFilled(
							ImVec2(endScreen.x, endScreen.y),
							ImVec2(endScreen.x - dir.x * arrowSize - perp.x * arrowSize * 0.5f,
								   endScreen.y - dir.y * arrowSize - perp.y * arrowSize * 0.5f),
							ImVec2(endScreen.x - dir.x * arrowSize + perp.x * arrowSize * 0.5f,
								   endScreen.y - dir.y * arrowSize + perp.y * arrowSize * 0.5f),
							imVelColor
						);
					}
				}
			}
		}

		// Draw constraints
		if (m_EditorState->temp.showConstraints) {
			glm::vec4 conColor = m_EditorState->temp.constraintColor;
			ImU32 imConColor = IM_COL32(
				static_cast<int>(conColor.r * 255),
				static_cast<int>(conColor.g * 255),
				static_cast<int>(conColor.b * 255),
				static_cast<int>(conColor.a * 255)
			);

			auto constraintView = registry->view<TransformComponent, ConstraintComponent>();
			for (auto entityId : constraintView) {
				auto& transform = constraintView.get<TransformComponent>(entityId);
				auto& constraint = constraintView.get<ConstraintComponent>(entityId);

				glm::vec3 worldPosA = transform.GetTranslation() + glm::mat3(transform.GetMatrix()) * constraint.anchorA;

				// Get position of connected entity
				glm::vec3 worldPosB;
				if (constraint.connectedEntity != entt::null && registry->valid(constraint.connectedEntity)) {
					if (registry->all_of<TransformComponent>(constraint.connectedEntity)) {
						auto& transformB = registry->get<TransformComponent>(constraint.connectedEntity);
						worldPosB = transformB.GetTranslation() + glm::mat3(transformB.GetMatrix()) * constraint.anchorB;
					} else {
						worldPosB = constraint.anchorB;  // World anchor
					}
				} else {
					worldPosB = constraint.anchorB;  // World anchor
				}

				glm::vec2 screenA, screenB;
				if (WorldToScreen(worldPosA, viewProj, screenA) && WorldToScreen(worldPosB, viewProj, screenB)) {
					// Draw line between anchor points
					drawList->AddLine(
						ImVec2(screenA.x, screenA.y),
						ImVec2(screenB.x, screenB.y),
						imConColor, 2.0f
					);

					// Draw circles at anchor points
					drawList->AddCircleFilled(ImVec2(screenA.x, screenA.y), 4.0f, imConColor);
					drawList->AddCircleFilled(ImVec2(screenB.x, screenB.y), 4.0f, imConColor);

					// Draw axis for hinge/slider constraints
					if (constraint.type == ConstraintType::Hinge || constraint.type == ConstraintType::Slider) {
						glm::vec3 axisEnd = worldPosA + glm::normalize(constraint.axis) * 0.5f;
						glm::vec2 screenAxisEnd;
						if (WorldToScreen(axisEnd, viewProj, screenAxisEnd)) {
							ImU32 axisColor = IM_COL32(255, 0, 255, 255);  // Magenta for axis
							drawList->AddLine(
								ImVec2(screenA.x, screenA.y),
								ImVec2(screenAxisEnd.x, screenAxisEnd.y),
								axisColor, 3.0f
							);
						}
					}
				}
			}
		}
	}
}