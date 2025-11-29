#include "Panels/SceneHierarchyPanel/SceneHierarchyPanel.h"
#include "Project/Scene/SceneManager.h"
#include "Dialogs/ConfirmationDialog.h"
#include <IconsFontAwesome6.h>

namespace Laura
{

    SceneHierarchyPanel::SceneHierarchyPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager)
		: m_EditorState(editorState), m_ProjectManager(projectManager) {
    }

    void SceneHierarchyPanel::OnImGuiRender() {
        EditorTheme& theme = m_EditorState->temp.editorTheme;
        
        
        ImGui::Begin(ICON_FA_CHART_BAR " Scene Hierarchy");
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

        if (!scene) {
            ImGui::End();
            return;
        }

        entt::registry* activeRegistry = scene->GetRegistry();
        if(!activeRegistry) {
			ImGui::End();
            LOG_ENGINE_WARN("SceneHierarchyPanel::OnImGuiRender: activeRegistry is nullptr.");
			return;
		}
        if (ImGui::BeginMenu("Add")) {
            if (ImGui::MenuItem("Empty")) {
                scene->CreateEntity();
            }
            ImGui::EndMenu();
        }
        
        ImVec2 panelDims = ImGui::GetContentRegionAvail();
        float lineHeight = ImGui::GetFont()->FontSize + ImGui::GetStyle().FramePadding.y * 2.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                    | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap
                                    | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;

        static bool destroyEntity = false; // since you can only delete one entity at a time the fact that this is used in the loop is fine

        // ITERATE OVER ENTITIES
        auto view = activeRegistry->view<entt::entity>();
        for (auto entityID : view) {
            bool entityChildrenOpen = false;
            EntityHandle entity(entityID, activeRegistry);
            std::string& tag = entity.GetComponent<TagComponent>().Tag;

            theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
            // display selected entity
            if (entityID == m_EditorState->temp.selectedEntity) {
                theme.PushColor(ImGuiCol_Text, EditorCol_Text1);
                theme.PushColor(ImGuiCol_Header, EditorCol_Secondary1);
                theme.PushColor(ImGuiCol_Button, EditorCol_Primary1, 0.0f);

                entityChildrenOpen = ImGui::TreeNodeEx((void*)(uint64_t)entityID, flags, tag.c_str());
                ImGui::SameLine(panelDims.x - lineHeight * 0.5);
                if (ImGui::Button(ICON_FA_TRASH)) { destroyEntity = true; }
                ConfirmAndExecute(destroyEntity, ICON_FA_TRASH " Delete Entity", "Are you sure you want to delete this entity?", [&]() {
                        scene->DestroyEntity(entity);
                        m_EditorState->temp.selectedEntity = entt::null;
                    }, m_EditorState);
                theme.PopColor(3);
			}
			else { // display non-selected entity
				entityChildrenOpen = ImGui::TreeNodeEx((void*)(uint64_t)entityID, flags, tag.c_str());
			}
            theme.PopColor();

            if (ImGui::IsItemClicked()) {
                m_EditorState->temp.selectedEntity = entityID;
            }

            if (entityChildrenOpen) {
                // TODO: Child entity system
                ImGui::BulletText("This is a testing text.");
                ImGui::TreePop();
            }
        }

        // Deselect the selected entity if the user clicks in the window but outside of any tree node
        if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered()) {
			m_EditorState->temp.selectedEntity = entt::null;
		}

        ImGui::PopStyleVar();
        
        if (m_EditorState->temp.isInRuntimeSimulation) {
            ImGui::EndDisabled();
        }
        
        ImGui::End();
    }
}