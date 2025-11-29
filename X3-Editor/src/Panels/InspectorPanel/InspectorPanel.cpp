#include "Panels/InspectorPanel/InspectorPanel.h"
#include "Project/Scene/SceneManager.h"
#include "Panels/InspectorPanel/TransformUI.h"
#include "Panels/DNDPayloads.h"
#include "LauraBrandIcons.h"

namespace Laura
{

	/// INSPECTOR PANEL METHODS //////////////////////////////////////////////////////////////
	InspectorPanel::InspectorPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager)
		: m_EditorState(editorState), m_ProjectManager(projectManager) {
	}

    void InspectorPanel::OnImGuiRender() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;
		
		
		ImGui::SetNextWindowSizeConstraints({ 350, 50 }, {FLT_MAX, FLT_MAX});
		ImGui::Begin(ICON_FA_SLIDERS " Inspector");
		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::BeginDisabled();
		}

        if (!m_ProjectManager->ProjectIsOpen()) {
			if (m_EditorState->temp.isInRuntimeSimulation) {
				ImGui::EndDisabled();
			}
            ImGui::End();
            return;
        }

        std::shared_ptr<Scene> scene = m_ProjectManager->GetSceneManager()->GetOpenScene();

        if (scene == nullptr || m_EditorState->temp.selectedEntity == entt::null) {
			if (m_EditorState->temp.isInRuntimeSimulation) {
				ImGui::EndDisabled();
			}
			ImGui::End();
			return;
		}

		theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary2);
        entt::registry* activeRegistry = scene->GetRegistry();
        entt::entity selectedEntity = m_EditorState->temp.selectedEntity;
        EntityHandle entity(selectedEntity, activeRegistry);
		
		// TAG COMPONENT
        if (entity.HasComponent<TagComponent>()) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			std::string& tag = entity.GetComponent<TagComponent>().Tag;
			char buffer[256];
			memset(buffer, 0, sizeof(buffer));
			strcpy(buffer, tag.c_str());
			ImGui::AlignTextToFramePadding();
			ImGui::Text(ICON_FA_TAG " Name:"); ImGui::SameLine();
			theme.PopColor();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::InputTextWithHint("##Tag Component", "Entity Name", buffer, sizeof(buffer))) {
				tag = std::string(buffer);
			}
		}

		// ID COMPONENT
		if (entity.HasComponent<IDComponent>()) {
			LR_GUID guid = entity.GetComponent<IDComponent>().guid;

			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("Guid:"); ImGui::SameLine();
			theme.PopColor();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::Text(guid.string().c_str());
		}


		// TRANSFORM COMPONENT
		DrawComponent<TransformComponent>(std::string(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT " Transform"), entity, [&](EntityHandle& _entity) {
				DrawTransformSliders(m_EditorState, _entity);
			}
		);

		// CAMERA COMPOENENT
		DrawComponent<CameraComponent>(std::string(ICON_FA_VIDEO " Camera Component"), entity, [&](EntityHandle& entity) {
				auto& cameraComponent = entity.GetComponent<CameraComponent>();


				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 1));  // thinner widgets
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Main Camera:");
				theme.PopColor();
				ImGui::SameLine();
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				if (ImGui::Checkbox("##MainCameraCheckbox", &cameraComponent.isMain)) {
					for (auto e : scene->GetRegistry()->view<CameraComponent>()) {
						EntityHandle otherEntity(e, scene->GetRegistry());
						if (otherEntity.GetComponent<IDComponent>().guid != entity.GetComponent<IDComponent>().guid) {
							otherEntity.GetComponent<CameraComponent>().isMain = false;
						}
					}
				}
				theme.PopColor(2);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::AlignTextToFramePadding();
				ImGui::Text("FOV");
				theme.PopColor();
				ImGui::SameLine();
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::DragFloat("##fovDragInt", &cameraComponent.fov, 0.1f, 10.0f, 130.0f, "%.1f");
				ImGui::PopStyleVar();
			}
		);

		// MESH COMPONENT
		DrawComponent<MeshComponent>(std::string(ICON_FA_CUBE " Mesh"), entity, [&theme](EntityHandle& entity) {
				std::string& sourceName = entity.GetComponent<MeshComponent>().sourceName;
				ImGui::Dummy({ 0.0f, 5.0f });

				std::string displayName = sourceName.empty() ? "No mesh selected" : sourceName;
				DragDropWidget(
					"Mesh:",
					DNDPayloadTypes::MESH,
					displayName,
					[&](const DNDPayload& payload) {
						sourceName = payload.title;
						entity.GetComponent<MeshComponent>().guid = payload.guid;
					},
					theme,
					"Drag a mesh asset here from the Assets panel",
					{0, 0},
					!sourceName.empty()
				);
			}
		);

		DrawComponent<MaterialComponent>(std::string(ICON_FA_LAYER_GROUP " Material"), entity, [&](EntityHandle& entity) {
				auto& materialComponent = entity.GetComponent<MaterialComponent>();
				ImGui::Dummy({ 0.0f, 5.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Emission Strength:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##emission strength", &materialComponent.emission.w, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Emission Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit3("##emission color", glm::value_ptr(materialComponent.emission), ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoInputs);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit3("##color", glm::value_ptr(materialComponent.color), ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoInputs);
			}
		);
		ImGui::Dummy(ImVec2(0.0f, 10.0f));

		// ADD COMPONENT BUTTON
		ImVec2 panelDims = ImGui::GetContentRegionAvail();
		float lineHeight = ImGui::GetFont()->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f;
		ImGui::SetCursorPosX(panelDims.x / 6);
		bool popupOpened = false;
		float borderSz = ImGui::GetStyle().PopupBorderSize;
		ImGui::GetStyle().PopupBorderSize = 0.0f;
		theme.PushColor(ImGuiCol_Button, EditorCol_Secondary2);
		float buttonWidth = panelDims.x * (2.0f / 3.0f);
		if (ImGui::Button("Add Component", { buttonWidth, lineHeight })) {
			popupOpened = true;
			ImGui::OpenPopup("AddComponent");
		}
		theme.PopColor();

		if (ImGui::IsPopupOpen("AddComponent")) {
			ImVec2 addButtonPos = ImGui::GetItemRectMin();
			ImVec2 addButtonSize = ImGui::GetItemRectSize();
			ImGui::SetNextWindowSizeConstraints(
				ImVec2(FLT_MIN, FLT_MIN),
				ImVec2(FLT_MAX, 300.0f) // if the popup contains too many compnents, adds a scrollbar
			);
			ImGui::SetNextWindowPos(ImVec2(addButtonPos.x, addButtonPos.y + addButtonSize.y));
			ImGui::SetNextWindowSize(ImVec2(buttonWidth, 0.0f));
		}

		if (ImGui::BeginPopup("AddComponent")) {
			GiveEntityComponentButton<TransformComponent>	(entity, "Transform", ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT);
			GiveEntityComponentButton<CameraComponent>		(entity, "Camera", ICON_FA_VIDEO);
			GiveEntityComponentButton<MeshComponent>		(entity, "Mesh", ICON_FA_CUBE);
			GiveEntityComponentButton<MaterialComponent>	(entity, "Material", ICON_FA_LAYER_GROUP);
			ImGui::EndPopup();
		}
		ImGui::GetStyle().PopupBorderSize = borderSz;

		// ensure that there is always some space under the Add Component button when scrolling to display the popup
		ImGui::Dummy(ImVec2(0.0f, 100.0f)); 
		theme.PopColor();
		
		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::EndDisabled();
		}
		
        ImGui::End();
    }
}