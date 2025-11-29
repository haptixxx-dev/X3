#pragma once 

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include "Laura.h"
#include "Panels/IEditorPanel.h"
#include "EditorState.h"
#include "EditorTheme.h"
#include "Panels/DNDPayloads.h"
#include "Dialogs/ConfirmationDialog.h"

namespace Laura
{

	class InspectorPanel : public IEditorPanel {
	public:
		InspectorPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager);
		~InspectorPanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		template<typename T, typename UILambda>
		inline void DrawComponent(const std::string& TreenodeTitle, EntityHandle& entity, UILambda uiLambda, bool removable = true) {
			auto theme = m_EditorState->temp.editorTheme;
			const ImGuiTreeNodeFlags treenodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowItemOverlap
													| ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;
			static bool deleteComponent = false;
			if (entity.HasComponent<T>()) {
				std::string idStr = std::to_string((uint64_t)entity.GetEnttID()) + TreenodeTitle;
				ImGui::PushID(idStr.c_str());
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Secondary1);
				theme.PushColor(ImGuiCol_Header, EditorCol_Secondary1);
				ImGui::BeginChild((idStr + "ChildWindow").c_str(), ImVec2(-FLT_MIN, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_FrameStyle);

				ImVec2 windowWidth = ImGui::GetContentRegionAvail();
				float delBtnWidth = ImGui::CalcTextSize(ICON_FA_TRASH).x + ImGui::GetStyle().FramePadding.x * 2.0f;
				float optionsBtnWidth = ImGui::CalcTextSize(ICON_FA_GEARS).x + ImGui::GetStyle().FramePadding.x * 2.0f;

				bool componentTreeNodeOpen = ImGui::TreeNodeEx(TreenodeTitle.c_str(), treenodeFlags);
				ImGui::SameLine(windowWidth.x - delBtnWidth - optionsBtnWidth);
				theme.PushColor(ImGuiCol_Button, EditorCol_Primary1, 0.0f);
				if (removable) {
					if (ImGui::Button(ICON_FA_TRASH)) {
						deleteComponent = true;
						ImGui::OpenPopup(ICON_FA_TRASH " Delete Component");
					}

					ConfirmAndExecute(deleteComponent, ICON_FA_TRASH " Delete Component", "Are you sure you want to delete this component?", [&]() {
						entity.RemoveComponent<T>();
					}, m_EditorState);
				}

				ImGui::SameLine(windowWidth.x - optionsBtnWidth);
				if (ImGui::Button(ICON_FA_GEARS)) {
					ImGui::OpenPopup("ComponentSettings");
				}
				ImGui::PopStyleVar();

				if (ImGui::BeginPopup("ComponentSettings")) {
					if (ImGui::MenuItem("Reset")) {
						entity.RemoveComponent<T>();
						entity.GetOrAddComponent<T>();
					}
					ImGui::EndPopup();
				}
				theme.PopColor(); // transparent button
				if (componentTreeNodeOpen) {
					float avail_width = ImGui::GetContentRegionAvail().x;
					float margin_right = 5.0f;  // pixels to leave empty on the right
					ImGui::BeginChild("test", ImVec2(avail_width - margin_right, 0), ImGuiChildFlags_AutoResizeY);

					if (entity.HasComponent<T>()){ // the component could have been removed in the meantime
						uiLambda(entity);
					}
					ImGui::EndChild();

					ImGui::TreePop();
					ImGui::Spacing();
				}
				ImGui::EndChild();
				theme.PopColor(2); // headerBg, frameBg
				ImGui::PopID();
			}
		}

        template<typename T>
        void GiveEntityComponentButton(EntityHandle entity, const char* label, const char* icon) {
			if (entity.HasComponent<T>()) { ImGui::BeginDisabled(); }

			if (ImGui::Selectable((icon + std::string(" ") + label).c_str(), false)) { 
				entity.GetOrAddComponent<T>(); 
				return; // avoid calling EndDisabled()
			}

            if (entity.HasComponent<T>()) { ImGui::EndDisabled(); }
        }

	private:
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;
	};
}