#include "Panels/SceneHierarchyPanel/SceneHierarchyPanel.h"
#include "Project/Scene/SceneManager.h"
#include "Dialogs/ConfirmationDialog.h"
#include <IconsFontAwesome6.h>

namespace X3
{
    // Entity drag-and-drop, panel-local on purpose. DNDPayloadTypes in
    // DNDPayloads.h describes ASSET drags: every one of them carries a
    // DNDPayload{guid,title} and lands on an asset slot. An entity drag carries a
    // registry-local entt handle and means "reparent", so it shares neither the
    // payload struct nor a single target with them. Promote it to DNDPayloads.h
    // only when a second panel needs to receive entity drops.
    static constexpr const char* ENTITY_DND_PAYLOAD = "DND_PAYLOAD_ENTITY";

    SceneHierarchyPanel::SceneHierarchyPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager)
		: m_EditorState(editorState), m_ProjectManager(projectManager) {
    }

    void SceneHierarchyPanel::OnImGuiRender() {
        // The theme is fetched inside DrawEntityNode now -- row styling moved
        // there with the rest of the per-node drawing.
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

        // ROOTS ONLY. Children are reached through recursion in DrawEntityNode, so
        // iterating the whole registry here would draw every parented entity twice
        // -- once at its real depth and once at the top level.
        auto view = activeRegistry->view<entt::entity>();
        for (auto entityID : view) {
            EntityHandle entity(entityID, activeRegistry);
            if (scene->GetParent(entity).IsValid()) {
                continue;
            }
            DrawEntityNode(scene, entity, panelDims.x, lineHeight);
        }

        // DROP ZONE FOR UNPARENTING. Without an explicit target for "no parent"
        // there is no gesture that gets an entity back out to the root -- the only
        // drop targets are other entities, all of which parent it deeper.
        //
        // A Dummy rather than an InvisibleButton on purpose: a button would take
        // ImGui's ActiveId on click, which makes IsWindowHovered() below return
        // false, which would silently kill click-empty-space-to-deselect.
        ImVec2 remaining = ImGui::GetContentRegionAvail();
        ImGui::Dummy(ImVec2(remaining.x, remaining.y > lineHeight ? remaining.y : lineHeight));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ENTITY_DND_PAYLOAD)) {
                IM_ASSERT(payload->DataSize == sizeof(entt::entity));
                m_PendingReparentChild = *static_cast<const entt::entity*>(payload->Data);
                m_PendingReparentParent = entt::null; // detach to root
                m_HasPendingReparent = true;
            }
            ImGui::EndDragDropTarget();
        }

        // APPLY DEFERRED STRUCTURAL EDITS. Everything above only recorded intent;
        // this is the one point in the frame where no tree walk is in progress and
        // it is safe to mutate the registry. See the member declarations.
        if (m_HasPendingReparent) {
            EntityHandle child(m_PendingReparentChild, activeRegistry);
            EntityHandle parent = (m_PendingReparentParent == entt::null)
                ? EntityHandle{}
                : EntityHandle(m_PendingReparentParent, activeRegistry);
            scene->SetParent(child, parent); // rejects cycles itself; a refusal is a no-op
            m_HasPendingReparent = false;
            m_PendingReparentChild = entt::null;
            m_PendingReparentParent = entt::null;
        }
        if (m_PendingDestroy != entt::null) {
            scene->DestroyEntity(EntityHandle(m_PendingDestroy, activeRegistry));
            // The selection may have been a DESCENDANT of the destroyed entity and
            // gone with it, so clear it unconditionally rather than comparing.
            m_EditorState->temp.selectedEntity = entt::null;
            m_PendingDestroy = entt::null;
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

    size_t SceneHierarchyPanel::CountDescendants(const std::shared_ptr<Scene>& scene, EntityHandle entity) const {
        size_t count = 0;
        for (EntityHandle child : scene->GetChildren(entity)) {
            count += 1 + CountDescendants(scene, child);
        }
        return count;
    }

    void SceneHierarchyPanel::DrawEntityNode(const std::shared_ptr<Scene>& scene, EntityHandle entity, float panelWidth, float lineHeight) {
        EditorTheme& theme = m_EditorState->temp.editorTheme;
        entt::registry* activeRegistry = scene->GetRegistry();
        const entt::entity entityID = entity.GetEnttID();
        std::string& tag = entity.GetComponent<TagComponent>().Tag;

        // Snapshot the children BEFORE drawing. The drag-drop handlers below only
        // record intent (see the pending members) so nothing should mutate the
        // hierarchy mid-frame -- but holding a snapshot rather than a reference
        // into the component makes that guarantee local instead of a promise about
        // code somewhere else.
        std::vector<EntityHandle> children = scene->GetChildren(entity);
        const bool hasChildren = !children.empty();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                    | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap
                                    | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding;

        // Leaf|NoTreePushOnOpen is now applied PER NODE instead of to every node.
        // It used to be unconditional because there was no hierarchy to expand
        // into. The pairing still matters: NoTreePushOnOpen means the node does not
        // push onto the tree stack, so a leaf owes no TreePop() -- and a leaf
        // WITHOUT it would push silently, leaving the stack unbalanced until
        // ImGui::End() asserts far away from the cause. Hence the symmetric
        // condition on the TreePop below: pop exactly when we did not pass it.
        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        const bool isSelected = (entityID == m_EditorState->temp.selectedEntity);
        size_t pushedColors = 1;
        theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
        if (isSelected) {
            theme.PushColor(ImGuiCol_Text, EditorCol_Text1);
            theme.PushColor(ImGuiCol_Header, EditorCol_Secondary1);
            theme.PushColor(ImGuiCol_Button, EditorCol_Primary1, 0.0f);
            pushedColors += 3;
        }

        // The ImGui ID is the entity handle, NOT the label and not the position in
        // the tree, so a node keeps its open/closed state when it is renamed or
        // reparented. "%s" and not tag.c_str() as the format: an entity named
        // "100%" would otherwise be interpreted as a format specifier.
        const bool opened = ImGui::TreeNodeEx((void*)(uint64_t)entityID, flags, "%s", tag.c_str());

        // EVERYTHING THAT ASKS ABOUT "THE LAST ITEM" MUST COME BEFORE THE TRASH
        // BUTTON. IsItemClicked, BeginDragDropSource and BeginDragDropTarget all
        // refer to the most recently submitted item, and the selected row submits
        // a second one (the button) further down. Ordering these after it would
        // silently bind the drag to the trash icon -- and only on the selected
        // row, which is exactly the row a user drags most.
        const bool nodeClicked = ImGui::IsItemClicked();

        // --- DRAG SOURCE: this entity ---
        // The payload is the raw entt handle. It never leaves this frame's
        // registry, so the stability problem that forces GUIDs into the scene file
        // does not apply here.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload(ENTITY_DND_PAYLOAD, &entityID, sizeof(entt::entity));
            theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
            ImGui::Text(ICON_FA_CUBE " %s", tag.c_str());
            theme.PopColor();
            ImGui::EndDragDropSource();
        }

        // --- DROP TARGET: parent the dragged entity under this one ---
        if (ImGui::BeginDragDropTarget()) {
            // AcceptBeforeDelivery so the illegal cases can be caught while the
            // user is still hovering. Scene::SetParent would refuse them anyway,
            // but a silent no-op on release reads as a broken editor -- the user
            // needs to be told WHY the drop did nothing, before they let go.
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ENTITY_DND_PAYLOAD, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                IM_ASSERT(payload->DataSize == sizeof(entt::entity));
                const entt::entity dragged = *static_cast<const entt::entity*>(payload->Data);

                // A cycle would make THIS FUNCTION recurse forever: DrawEntityNode
                // walks children unconditionally, so a ring of parents is an
                // infinite recursion and a stack overflow the next frame -- with
                // no ImGui assert to point at the cause. Refuse it at the gesture.
                const bool wouldCycle = (dragged == entityID)
                    || scene->IsDescendantOf(entity, EntityHandle(dragged, activeRegistry));

                if (wouldCycle) {
                    ImGui::SetTooltip(ICON_FA_BAN " Cannot parent an entity to itself or to its own child");
                }
                else if (payload->IsDelivery()) {
                    m_PendingReparentChild = dragged;
                    m_PendingReparentParent = entityID;
                    m_HasPendingReparent = true;
                }
            }
            ImGui::EndDragDropTarget();
        }

        // The delete affordance for the selected row. Submitted last so it cannot
        // steal the item queries above; SameLine's explicit offset is measured
        // from the window content origin and ignores tree indentation, so the icon
        // stays in the same column at every depth.
        if (isSelected) {
            ImGui::SameLine(panelWidth - lineHeight * 0.5f);
            if (ImGui::Button(ICON_FA_TRASH)) {
                m_DestroyRequested = true;
                // Built here, once, because DestroyEntity takes the whole subtree
                // and there is no undo -- the user has to be told how much of the
                // scene a collapsed row is hiding.
                const size_t descendants = CountDescendants(scene, entity);
                m_DestroyPrompt = descendants == 0
                    ? "Are you sure you want to delete this entity?"
                    : "Delete '" + tag + "' and its " + std::to_string(descendants) +
                      " child entities? Children are deleted with their parent, not kept.";
            }
            ConfirmAndExecute(m_DestroyRequested, ICON_FA_TRASH " Delete Entity", m_DestroyPrompt.c_str(), [&]() {
                    // Deferred: we are inside the recursive walk right now.
                    m_PendingDestroy = entityID;
                }, m_EditorState);
        }
        theme.PopColor(pushedColors);

        if (nodeClicked) {
            m_EditorState->temp.selectedEntity = entityID;
        }

        if (opened && hasChildren) {
            for (EntityHandle child : children) {
                DrawEntityNode(scene, child, panelWidth, lineHeight);
            }
            ImGui::TreePop(); // owed only because hasChildren meant no NoTreePushOnOpen
        }
    }
}