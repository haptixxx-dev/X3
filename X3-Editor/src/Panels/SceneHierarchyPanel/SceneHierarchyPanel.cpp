#include "Panels/SceneHierarchyPanel/SceneHierarchyPanel.h"
#include "Project/Scene/SceneManager.h"
#include "Dialogs/ConfirmationDialog.h"
#include <IconsFontAwesome6.h>

namespace X3
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
            ImGui::Separator();

            // Mesh Entity
            if (ImGui::MenuItem(ICON_FA_CUBE " Mesh")) {
                auto entity = scene->CreateEntity("Mesh");
                entity.GetOrAddComponent<MeshComponent>();
                entity.GetOrAddComponent<MaterialComponent>();
            }

            // Camera Entity
            if (ImGui::MenuItem(ICON_FA_VIDEO " Camera")) {
                auto entity = scene->CreateEntity("Camera");
                auto& camera = entity.GetOrAddComponent<CameraComponent>();
                camera.fov = 60.0f;
            }

            ImGui::Separator();

            // Light Submenu
            if (ImGui::BeginMenu(ICON_FA_LIGHTBULB " Light")) {
                if (ImGui::MenuItem("Directional Light")) {
                    auto entity = scene->CreateEntity("Directional Light");
                    auto& light = entity.GetOrAddComponent<LightComponent>();
                    light.type = LightType::DIRECTIONAL;
                    light.intensity = 1.0f;
                    light.color = glm::vec3(1.0f, 1.0f, 1.0f);
                }
                if (ImGui::MenuItem("Point Light")) {
                    auto entity = scene->CreateEntity("Point Light");
                    auto& light = entity.GetOrAddComponent<LightComponent>();
                    light.type = LightType::POINT;
                    light.intensity = 10.0f;
                    light.range = 10.0f;
                    light.attenuation = 1.0f;
                    light.color = glm::vec3(1.0f, 1.0f, 1.0f);
                }
                if (ImGui::MenuItem("Spot Light")) {
                    auto entity = scene->CreateEntity("Spot Light");
                    auto& light = entity.GetOrAddComponent<LightComponent>();
                    light.type = LightType::SPOT;
                    light.intensity = 10.0f;
                    light.range = 15.0f;
                    light.attenuation = 1.0f;
                    light.innerConeAngle = 30.0f;
                    light.outerConeAngle = 45.0f;
                    light.color = glm::vec3(1.0f, 1.0f, 1.0f);
                }
                ImGui::EndMenu();
            }

            // Physics Primitives Submenu
            if (ImGui::BeginMenu(ICON_FA_SHAPES " Physics")) {
                // Static Bodies
                if (ImGui::BeginMenu("Static")) {
                    if (ImGui::MenuItem("Box Collider")) {
                        auto entity = scene->CreateEntity("Static Box");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Static;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Box;
                        col.boxHalfExtents = glm::vec3(0.5f);
                    }
                    if (ImGui::MenuItem("Sphere Collider")) {
                        auto entity = scene->CreateEntity("Static Sphere");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Static;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Sphere;
                        col.sphereRadius = 0.5f;
                    }
                    if (ImGui::MenuItem("Capsule Collider")) {
                        auto entity = scene->CreateEntity("Static Capsule");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Static;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Capsule;
                        col.capsuleRadius = 0.25f;
                        col.capsuleHalfHeight = 0.5f;
                    }
                    ImGui::EndMenu();
                }
                // Dynamic Bodies
                if (ImGui::BeginMenu("Dynamic")) {
                    if (ImGui::MenuItem("Box Collider")) {
                        auto entity = scene->CreateEntity("Dynamic Box");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Dynamic;
                        rb.mass = 1.0f;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Box;
                        col.boxHalfExtents = glm::vec3(0.5f);
                    }
                    if (ImGui::MenuItem("Sphere Collider")) {
                        auto entity = scene->CreateEntity("Dynamic Sphere");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Dynamic;
                        rb.mass = 1.0f;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Sphere;
                        col.sphereRadius = 0.5f;
                    }
                    if (ImGui::MenuItem("Capsule Collider")) {
                        auto entity = scene->CreateEntity("Dynamic Capsule");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Dynamic;
                        rb.mass = 1.0f;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Capsule;
                        col.capsuleRadius = 0.25f;
                        col.capsuleHalfHeight = 0.5f;
                    }
                    ImGui::EndMenu();
                }
                // Kinematic Bodies
                if (ImGui::BeginMenu("Kinematic")) {
                    if (ImGui::MenuItem("Box Collider")) {
                        auto entity = scene->CreateEntity("Kinematic Box");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Kinematic;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Box;
                        col.boxHalfExtents = glm::vec3(0.5f);
                    }
                    if (ImGui::MenuItem("Sphere Collider")) {
                        auto entity = scene->CreateEntity("Kinematic Sphere");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Kinematic;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Sphere;
                        col.sphereRadius = 0.5f;
                    }
                    if (ImGui::MenuItem("Capsule Collider")) {
                        auto entity = scene->CreateEntity("Kinematic Capsule");
                        auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
                        rb.bodyType = BodyType::Kinematic;
                        auto& col = entity.GetOrAddComponent<ColliderComponent>();
                        col.shape = ColliderShape::Capsule;
                        col.capsuleRadius = 0.25f;
                        col.capsuleHalfHeight = 0.5f;
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                // Character Controller
                if (ImGui::MenuItem("Character Controller")) {
                    auto entity = scene->CreateEntity("Character");
                    auto& cc = entity.GetOrAddComponent<CharacterControllerComponent>();
                    cc.capsuleRadius = 0.3f;
                    cc.capsuleHeight = 1.8f;
                    cc.walkSpeed = 5.0f;
                    cc.sprintSpeed = 8.0f;
                    cc.jumpForce = 5.0f;
                }
                ImGui::EndMenu();
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

		// Keyboard shortcuts
		if (ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) {
			// Ctrl+C: Copy selected entity
			if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
				if (m_EditorState->temp.selectedEntity != entt::null) {
					m_EditorState->temp.copiedEntity = m_EditorState->temp.selectedEntity;
					m_EditorState->temp.isCutOperation = false;
				}
			}
			// Ctrl+X: Cut selected entity
			if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
				if (m_EditorState->temp.selectedEntity != entt::null) {
					m_EditorState->temp.copiedEntity = m_EditorState->temp.selectedEntity;
					m_EditorState->temp.isCutOperation = true;
				}
			}
			// Ctrl+V: Paste entity
			if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
				if (m_EditorState->temp.copiedEntity != entt::null && activeRegistry->valid(m_EditorState->temp.copiedEntity)) {
					EntityHandle sourceEntity(m_EditorState->temp.copiedEntity, activeRegistry);
					EntityHandle duplicated = scene->DuplicateEntity(sourceEntity);
					m_EditorState->temp.selectedEntity = duplicated.GetEnttID();

					// If it was a cut operation, delete the original
					if (m_EditorState->temp.isCutOperation) {
						scene->DestroyEntity(sourceEntity);
						m_EditorState->temp.copiedEntity = entt::null;
						m_EditorState->temp.isCutOperation = false;
					}
				}
			}
			// Ctrl+D: Duplicate selected entity
			if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
				if (m_EditorState->temp.selectedEntity != entt::null) {
					EntityHandle selectedEntity(m_EditorState->temp.selectedEntity, activeRegistry);
					EntityHandle duplicated = scene->DuplicateEntity(selectedEntity);
					m_EditorState->temp.selectedEntity = duplicated.GetEnttID();
				}
			}
			// Delete: Delete selected entity
			if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
				if (m_EditorState->temp.selectedEntity != entt::null) {
					scene->DestroyEntity(EntityHandle(m_EditorState->temp.selectedEntity, activeRegistry));
					m_EditorState->temp.selectedEntity = entt::null;
				}
			}
		}

        ImGui::PopStyleVar();
        
        if (m_EditorState->temp.isInRuntimeSimulation) {
            ImGui::EndDisabled();
        }
        
        ImGui::End();
    }
}