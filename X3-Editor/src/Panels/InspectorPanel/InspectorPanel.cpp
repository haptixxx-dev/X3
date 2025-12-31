#include "Panels/InspectorPanel/InspectorPanel.h"
#include "Project/Scene/SceneManager.h"
#include "Panels/InspectorPanel/TransformUI.h"
#include "Panels/DNDPayloads.h"
#include "X3BrandIcons.h"

namespace X3
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
			ImGui::Text("%s", guid.string().c_str());
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

				// Material Copy/Paste
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
				if (ImGui::Button(ICON_FA_COPY " Copy Material")) {
					m_EditorState->temp.copiedMaterial = materialComponent;
					m_EditorState->temp.hasCopiedMaterial = true;
				}
				ImGui::SameLine();
				if (m_EditorState->temp.hasCopiedMaterial) {
					if (ImGui::Button(ICON_FA_PASTE " Paste Material")) {
						materialComponent = m_EditorState->temp.copiedMaterial;
					}
				} else {
					ImGui::BeginDisabled();
					ImGui::Button(ICON_FA_PASTE " Paste Material");
					ImGui::EndDisabled();
				}
				ImGui::PopStyleVar();
				ImGui::Dummy({ 0.0f, 3.0f });

				// Material Presets
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Presets:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				const char* presets[] = { "Custom", "Plastic (Smooth)", "Plastic (Rough)", "Metal (Polished)", "Metal (Brushed)", "Metal (Rough)", "Glass", "Rubber", "Stone", "Wood" };
				static int currentPreset = 0;
				if (ImGui::Combo("##MaterialPreset", &currentPreset, presets, IM_ARRAYSIZE(presets))) {
					switch (currentPreset) {
						case 1: // Plastic (Smooth)
							materialComponent.metallic = 0.0f;
							materialComponent.roughness = 0.2f;
							materialComponent.ao = 1.0f;
							break;
						case 2: // Plastic (Rough)
							materialComponent.metallic = 0.0f;
							materialComponent.roughness = 0.6f;
							materialComponent.ao = 1.0f;
							break;
						case 3: // Metal (Polished)
							materialComponent.metallic = 1.0f;
							materialComponent.roughness = 0.1f;
							materialComponent.ao = 1.0f;
							break;
						case 4: // Metal (Brushed)
							materialComponent.metallic = 1.0f;
							materialComponent.roughness = 0.4f;
							materialComponent.ao = 1.0f;
							break;
						case 5: // Metal (Rough)
							materialComponent.metallic = 1.0f;
							materialComponent.roughness = 0.7f;
							materialComponent.ao = 1.0f;
							break;
						case 6: // Glass
							materialComponent.metallic = 0.0f;
							materialComponent.roughness = 0.0f;
							materialComponent.ao = 1.0f;
							break;
						case 7: // Rubber
							materialComponent.metallic = 0.0f;
							materialComponent.roughness = 0.8f;
							materialComponent.ao = 1.0f;
							break;
						case 8: // Stone
							materialComponent.metallic = 0.0f;
							materialComponent.roughness = 0.9f;
							materialComponent.ao = 0.7f;
							break;
						case 9: // Wood
							materialComponent.metallic = 0.0f;
							materialComponent.roughness = 0.7f;
							materialComponent.ao = 0.8f;
							break;
					}
				}
				ImGui::Dummy({ 0.0f, 5.0f });

				// Albedo/Base Color
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Albedo:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit3("##color", glm::value_ptr(materialComponent.color), ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoInputs);

				// PBR Parameters Section
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("PBR Properties");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Metallic:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##metallic", &materialComponent.metallic, 0.0f, 1.0f, "%.2f");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Roughness:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##roughness", &materialComponent.roughness, 0.0f, 1.0f, "%.2f");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Ambient Occlusion:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##ao", &materialComponent.ao, 0.0f, 1.0f, "%.2f");
				theme.PopColor();

				// Emission Section
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Emission");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

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
			}
		);

		// LIGHT COMPONENT
		DrawComponent<LightComponent>(std::string(ICON_FA_LIGHTBULB " Light"), entity, [&](EntityHandle& entity) {
				auto& lightComponent = entity.GetComponent<LightComponent>();
				ImGui::Dummy({ 0.0f, 5.0f });

				// Light Intensity Presets
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Intensity Preset:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				const char* intensityPresets[] = { "Custom", "Candle", "Light Bulb", "Flashlight", "Street Lamp", "Sunlight" };
				static int currentIntensityPreset = 0;
				if (ImGui::Combo("##LightIntensityPreset", &currentIntensityPreset, intensityPresets, IM_ARRAYSIZE(intensityPresets))) {
					switch (currentIntensityPreset) {
						case 1: // Candle
							lightComponent.intensity = (lightComponent.type == LightType::DIRECTIONAL) ? 0.5f : 5.0f;
							break;
						case 2: // Light Bulb
							lightComponent.intensity = (lightComponent.type == LightType::DIRECTIONAL) ? 1.0f : 15.0f;
							break;
						case 3: // Flashlight
							lightComponent.intensity = (lightComponent.type == LightType::DIRECTIONAL) ? 2.0f : 30.0f;
							break;
						case 4: // Street Lamp
							lightComponent.intensity = (lightComponent.type == LightType::DIRECTIONAL) ? 3.0f : 50.0f;
							break;
						case 5: // Sunlight
							lightComponent.intensity = (lightComponent.type == LightType::DIRECTIONAL) ? 5.0f : 100.0f;
							break;
					}
				}
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Light Type:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				const char* lightTypes[] = { "Directional", "Point", "Spot" };
				int currentType = static_cast<int>(lightComponent.type);
				if (ImGui::Combo("##LightType", &currentType, lightTypes, IM_ARRAYSIZE(lightTypes))) {
					lightComponent.type = static_cast<LightType>(currentType);
				}

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit3("##light color", glm::value_ptr(lightComponent.color), ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoInputs);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Intensity:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##intensity", &lightComponent.intensity, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
				theme.PopColor();

				// Point and Spot light specific parameters
				if (lightComponent.type == LightType::POINT || lightComponent.type == LightType::SPOT) {
					theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
					ImGui::Text("Range:");
					theme.PopColor();
					ImGui::SameLine(150.0f);
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
					ImGui::DragFloat("##range", &lightComponent.range, 0.1f, 0.1f, 1000.0f, "%.2f");
					theme.PopColor();

					theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
					ImGui::Text("Attenuation:");
					theme.PopColor();
					ImGui::SameLine(150.0f);
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
					ImGui::DragFloat("##attenuation", &lightComponent.attenuation, 0.01f, 0.0f, 10.0f, "%.3f");
					theme.PopColor();
				}

				// Spot light specific parameters
				if (lightComponent.type == LightType::SPOT) {
					theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
					ImGui::Text("Inner Cone Angle:");
					theme.PopColor();
					ImGui::SameLine(150.0f);
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
					ImGui::SliderFloat("##innerCone", &lightComponent.innerConeAngle, 0.0f, 90.0f, "%.1f°");
					theme.PopColor();

					theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
					ImGui::Text("Outer Cone Angle:");
					theme.PopColor();
					ImGui::SameLine(150.0f);
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
					ImGui::SliderFloat("##outerCone", &lightComponent.outerConeAngle, 0.0f, 90.0f, "%.1f°");
					theme.PopColor();
				}
			}
		);

		// RIGIDBODY COMPONENT
		DrawComponent<RigidBodyComponent>(std::string(ICON_FA_BOWLING_BALL " Rigid Body"), entity, [&](EntityHandle& entity) {
				auto& rb = entity.GetComponent<RigidBodyComponent>();
				ImGui::Dummy({ 0.0f, 5.0f });

				// Body Type
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Body Type:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				const char* bodyTypes[] = { "Static", "Kinematic", "Dynamic" };
				int currentBodyType = static_cast<int>(rb.bodyType);
				if (ImGui::Combo("##BodyType", &currentBodyType, bodyTypes, IM_ARRAYSIZE(bodyTypes))) {
					rb.bodyType = static_cast<BodyType>(currentBodyType);
				}

				// Mass (only for dynamic bodies)
				if (rb.bodyType == BodyType::Dynamic) {
					theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
					ImGui::Text("Mass:");
					theme.PopColor();
					ImGui::SameLine(150.0f);
					ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
					theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
					ImGui::DragFloat("##Mass", &rb.mass, 0.1f, 0.001f, 10000.0f, "%.3f kg");
					theme.PopColor();
				}

				// Damping Section
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Damping");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Linear Damping:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##LinearDamping", &rb.linearDamping, 0.01f, 0.0f, 10.0f, "%.3f");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Angular Damping:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##AngularDamping", &rb.angularDamping, 0.01f, 0.0f, 10.0f, "%.3f");
				theme.PopColor();

				// Material Properties
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Material");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Friction:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##Friction", &rb.friction, 0.0f, 1.0f, "%.2f");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Restitution:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##Restitution", &rb.restitution, 0.0f, 1.0f, "%.2f");
				theme.PopColor();

				// Gravity Scale
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Gravity Scale:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##GravityScale", &rb.gravityScale, 0.1f, -10.0f, 10.0f, "%.2f");
				theme.PopColor();

				// Constraints
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Constraints");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Lock Rotation:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("X##LockRotX", &rb.lockRotationX); ImGui::SameLine();
				ImGui::Checkbox("Y##LockRotY", &rb.lockRotationY); ImGui::SameLine();
				ImGui::Checkbox("Z##LockRotZ", &rb.lockRotationZ);
				theme.PopColor(2);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Lock Position:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("X##LockPosX", &rb.lockPositionX); ImGui::SameLine();
				ImGui::Checkbox("Y##LockPosY", &rb.lockPositionY); ImGui::SameLine();
				ImGui::Checkbox("Z##LockPosZ", &rb.lockPositionZ);
				theme.PopColor(2);
			}
		);

		// COLLIDER COMPONENT
		DrawComponent<ColliderComponent>(std::string(ICON_FA_VECTOR_SQUARE " Collider"), entity, [&](EntityHandle& entity) {
				auto& col = entity.GetComponent<ColliderComponent>();
				ImGui::Dummy({ 0.0f, 5.0f });

				// Shape Type
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Shape:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				const char* shapes[] = { "Box", "Sphere", "Capsule", "Convex Mesh", "Triangle Mesh", "Heightfield" };
				int currentShape = static_cast<int>(col.shape);
				if (ImGui::Combo("##ColliderShape", &currentShape, shapes, IM_ARRAYSIZE(shapes))) {
					col.shape = static_cast<ColliderShape>(currentShape);
				}

				// Shape-specific parameters
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Shape Parameters");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				switch (col.shape) {
					case ColliderShape::Box:
						theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
						ImGui::Text("Half Extents:");
						theme.PopColor();
						ImGui::SameLine(150.0f);
						ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
						theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
						ImGui::DragFloat3("##BoxHalfExtents", glm::value_ptr(col.boxHalfExtents), 0.01f, 0.001f, 1000.0f, "%.3f");
						theme.PopColor();
						break;

					case ColliderShape::Sphere:
						theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
						ImGui::Text("Radius:");
						theme.PopColor();
						ImGui::SameLine(150.0f);
						ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
						theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
						ImGui::DragFloat("##SphereRadius", &col.sphereRadius, 0.01f, 0.001f, 1000.0f, "%.3f");
						theme.PopColor();
						break;

					case ColliderShape::Capsule:
						theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
						ImGui::Text("Radius:");
						theme.PopColor();
						ImGui::SameLine(150.0f);
						ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
						theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
						ImGui::DragFloat("##CapsuleRadius", &col.capsuleRadius, 0.01f, 0.001f, 1000.0f, "%.3f");
						theme.PopColor();

						theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
						ImGui::Text("Half Height:");
						theme.PopColor();
						ImGui::SameLine(150.0f);
						ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
						theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
						ImGui::DragFloat("##CapsuleHalfHeight", &col.capsuleHalfHeight, 0.01f, 0.001f, 1000.0f, "%.3f");
						theme.PopColor();
						break;

					case ColliderShape::ConvexMesh:
					case ColliderShape::TriangleMesh:
						theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
						ImGui::Text("Mesh GUID:");
						theme.PopColor();
						ImGui::SameLine(150.0f);
						ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
						ImGui::Text("%s", col.meshGuid.string().c_str());
						break;

					case ColliderShape::Heightfield:
						theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
						ImGui::Text("Heightfield");
						theme.PopColor();
						ImGui::Text("(Not yet implemented)");
						break;
				}

				// Offset
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Offset");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Position:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat3("##ColliderOffset", glm::value_ptr(col.offset), 0.01f, -1000.0f, 1000.0f, "%.3f");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Rotation:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat3("##ColliderRotation", glm::value_ptr(col.rotationOffset), 0.5f, -180.0f, 180.0f, "%.1f°");
				theme.PopColor();

				// Trigger
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Is Trigger:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("##IsTrigger", &col.isTrigger);
				theme.PopColor(2);
			}
		);

		// CHARACTER CONTROLLER COMPONENT
		DrawComponent<CharacterControllerComponent>(std::string(ICON_FA_PERSON_RUNNING " Character Controller"), entity, [&](EntityHandle& entity) {
				auto& cc = entity.GetComponent<CharacterControllerComponent>();
				ImGui::Dummy({ 0.0f, 5.0f });

				// Shape
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Capsule Shape");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Radius:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCRadius", &cc.capsuleRadius, 0.01f, 0.1f, 5.0f, "%.2f m");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Height:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCHeight", &cc.capsuleHeight, 0.01f, 0.5f, 10.0f, "%.2f m");
				theme.PopColor();

				// Movement
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Movement");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Walk Speed:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCWalkSpeed", &cc.walkSpeed, 0.1f, 0.0f, 50.0f, "%.1f m/s");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Sprint Speed:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCSprintSpeed", &cc.sprintSpeed, 0.1f, 0.0f, 100.0f, "%.1f m/s");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Jump Force:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCJumpForce", &cc.jumpForce, 0.1f, 0.0f, 50.0f, "%.1f m/s");
				theme.PopColor();

				// Step and Slope
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Step & Slope");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Max Step Height:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCStepHeight", &cc.maxStepHeight, 0.01f, 0.0f, 1.0f, "%.2f m");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Max Slope Angle:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::SliderFloat("##CCMaxSlope", &cc.maxSlopeAngle, 0.0f, 90.0f, "%.0f°");
				theme.PopColor();

				// Physics
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Physics");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Mass:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCMass", &cc.mass, 1.0f, 1.0f, 500.0f, "%.0f kg");
				theme.PopColor();

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Skin Width:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::DragFloat("##CCSkinWidth", &cc.skinWidth, 0.001f, 0.001f, 0.1f, "%.3f m");
				theme.PopColor();

				// Runtime State (read-only)
				ImGui::Dummy({ 0.0f, 5.0f });
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::Text("Runtime State");
				theme.PopColor();
				ImGui::Separator();
				ImGui::Dummy({ 0.0f, 3.0f });

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Grounded: %s", cc.isGrounded ? "Yes" : "No");
				ImGui::Text("Velocity: (%.2f, %.2f, %.2f)", cc.velocity.x, cc.velocity.y, cc.velocity.z);
				theme.PopColor();
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
			GiveEntityComponentButton<LightComponent>		(entity, "Light", ICON_FA_LIGHTBULB);
			ImGui::Separator();
			GiveEntityComponentButton<RigidBodyComponent>	(entity, "Rigid Body", ICON_FA_BOWLING_BALL);
			GiveEntityComponentButton<ColliderComponent>	(entity, "Collider", ICON_FA_VECTOR_SQUARE);
			GiveEntityComponentButton<CharacterControllerComponent>(entity, "Character Controller", ICON_FA_PERSON_RUNNING);
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
