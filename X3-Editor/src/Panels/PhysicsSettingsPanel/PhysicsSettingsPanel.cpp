#include "Panels/PhysicsSettingsPanel/PhysicsSettingsPanel.h"
#include "Core/Events/PhysicsEvents.h"
#include "Core/Time.h"
#include <IconsFontAwesome6.h>
#include <imgui.h>

namespace X3
{

	void PhysicsSettingsPanel::OnImGuiRender() {
		if (!m_EditorState->temp.isPhysicsSettingsPanelOpen) {
			return;
		}

		EditorTheme& theme = m_EditorState->temp.editorTheme;

		ImGui::SetNextWindowSizeConstraints({ 350, 200 }, { FLT_MAX, FLT_MAX });
		if (ImGui::Begin(ICON_FA_ATOM " Physics Settings", &m_EditorState->temp.isPhysicsSettingsPanelOpen)) {

			if (!m_ProjectManager->ProjectIsOpen()) {
				ImGui::Text("No project open");
				ImGui::End();
				return;
			}

			// ==================== WORLD SETTINGS ====================
			theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
			ImGui::Text("World Settings");
			theme.PopColor();
			ImGui::Separator();
			ImGui::Dummy({ 0.0f, 3.0f });

			// Gravity -- bound directly to the open project, NOT to a local.
			//
			// The previous version of this control was a DragFloat3 over a function-local
			// `static`, which accepted edits, kept them across project switches, and reached
			// nothing: PhysicsWorld is a by-value member of PhysicsLayer and this panel is
			// constructed (EditorLayer.cpp) with only EditorState, IEventDispatcher and
			// ProjectManager. Binding to the ProjectFile field kills the static: the value
			// now belongs to the project, so opening another project shows that project's
			// gravity and Ctrl+S persists it.
			//
			// Reaching the solver goes through the dispatcher, mirroring how the render
			// settings panel reaches the Renderer (panel -> dispatcher -> RenderLayer ->
			// Renderer::applySettings). Dispatch is synchronous through LayerStack, so the
			// world has the new value before this frame's physics step.
			//
			// Dispatched on every DragFloat3 edit rather than on release: a drag should read
			// as continuous when the simulation is running, and SetGravity is a member store
			// plus one Jolt setter, not a rebuild.
			glm::vec3& gravity = m_ProjectManager->GetMutablePhysicsGravity();
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text("Gravity:");
			theme.PopColor();
			ImGui::SameLine(150.0f);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			if (ImGui::DragFloat3("##Gravity", glm::value_ptr(gravity), 0.1f, -100.0f, 100.0f, "%.2f m/s^2")) {
				m_EventDispatcher->dispatchEvent(std::make_shared<SetPhysicsGravityEvent>(gravity));
			}
			theme.PopColor();
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("World gravity in m/s^2, saved with the project.\n"
				                  "Applies to the running simulation immediately;\n"
				                  "otherwise it takes effect at the next Play.");
			}

			// Fixed Timestep
			float fixedDt = Time::GetFixedDeltaTime();
			float fixedHz = 1.0f / fixedDt;
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text("Physics Rate:");
			theme.PopColor();
			ImGui::SameLine(150.0f);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			if (ImGui::DragFloat("##FixedTimestep", &fixedHz, 1.0f, 30.0f, 240.0f, "%.0f Hz")) {
				Time::SetFixedDeltaTime(1.0f / fixedHz);
			}
			theme.PopColor();

			// Timestep presets
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text("Presets:");
			theme.PopColor();
			ImGui::SameLine(150.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
			if (ImGui::Button("30 Hz")) { Time::SetFixedDeltaTime(1.0f / 30.0f); }
			ImGui::SameLine();
			if (ImGui::Button("60 Hz")) { Time::SetFixedDeltaTime(1.0f / 60.0f); }
			ImGui::SameLine();
			if (ImGui::Button("120 Hz")) { Time::SetFixedDeltaTime(1.0f / 120.0f); }
			ImGui::PopStyleVar();

			ImGui::Dummy({ 0.0f, 10.0f });

			// ==================== DEBUG VISUALIZATION ====================
			theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
			ImGui::Text("Debug Visualization");
			theme.PopColor();
			ImGui::Separator();
			ImGui::Dummy({ 0.0f, 3.0f });

			// Master toggle
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text("Show Physics Debug:");
			theme.PopColor();
			ImGui::SameLine(150.0f);
			theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			ImGui::Checkbox("##ShowPhysicsDebug", &m_EditorState->temp.showPhysicsDebug);
			theme.PopColor(2);

			if (m_EditorState->temp.showPhysicsDebug) {
				ImGui::Indent(10.0f);

				// Collider wireframes
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Collider Wireframes:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("##ShowWireframes", &m_EditorState->temp.showColliderWireframes);
				theme.PopColor(2);

				// Collider AABBs
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Collider AABBs:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("##ShowAABBs", &m_EditorState->temp.showColliderAABBs);
				theme.PopColor(2);

				// Contact points
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Contact Points:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("##ShowContacts", &m_EditorState->temp.showContactPoints);
				theme.PopColor(2);

				// Velocity vectors
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Velocity Vectors:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("##ShowVelocity", &m_EditorState->temp.showVelocityVectors);
				theme.PopColor(2);

				// Constraints
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Constraints:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				theme.PushColor(ImGuiCol_CheckMark, EditorCol_Text1);
				theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
				ImGui::Checkbox("##ShowConstraints", &m_EditorState->temp.showConstraints);
				theme.PopColor(2);

				ImGui::Dummy({ 0.0f, 5.0f });

				// Colors
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Collider Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit4("##ColliderColor", glm::value_ptr(m_EditorState->temp.colliderWireframeColor),
					ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Trigger Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit4("##TriggerColor", glm::value_ptr(m_EditorState->temp.triggerWireframeColor),
					ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Contact Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit4("##ContactColor", glm::value_ptr(m_EditorState->temp.contactPointColor),
					ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Velocity Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit4("##VelocityColor", glm::value_ptr(m_EditorState->temp.velocityVectorColor),
					ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::Text("Constraint Color:");
				theme.PopColor();
				ImGui::SameLine(150.0f);
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				ImGui::ColorEdit4("##ConstraintColor", glm::value_ptr(m_EditorState->temp.constraintColor),
					ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

				ImGui::Unindent(10.0f);
			}

			ImGui::Dummy({ 0.0f, 10.0f });

			// ==================== COLLISION LAYERS ====================
			theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
			ImGui::Text("Collision Layers");
			theme.PopColor();
			ImGui::Separator();
			ImGui::Dummy({ 0.0f, 3.0f });

			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextWrapped("Layer 0: Non-Moving (static geometry)");
			ImGui::TextWrapped("Layer 1: Moving (dynamic/kinematic bodies)");
			ImGui::TextWrapped("Layer 2: Trigger (no collision response)");
			ImGui::TextWrapped("Layer 3: Character (character controllers)");
			theme.PopColor();

			ImGui::Dummy({ 0.0f, 10.0f });

			// ==================== SIMULATION STATUS ====================
			theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
			ImGui::Text("Simulation Status");
			theme.PopColor();
			ImGui::Separator();
			ImGui::Dummy({ 0.0f, 3.0f });

			bool isSimulating = m_EditorState->temp.isInRuntimeSimulation;
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text("Status:");
			theme.PopColor();
			ImGui::SameLine(150.0f);
			if (isSimulating) {
				theme.PushColor(ImGuiCol_Text, EditorCol_Success);
				ImGui::Text(ICON_FA_CIRCLE_PLAY " Running");
				theme.PopColor();
			} else {
				theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
				ImGui::Text(ICON_FA_CIRCLE_PAUSE " Stopped");
				theme.PopColor();
			}

			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text("Fixed Timestep:");
			theme.PopColor();
			ImGui::SameLine(150.0f);
			ImGui::Text("%.4f s (%.0f Hz)", Time::GetFixedDeltaTime(), 1.0f / Time::GetFixedDeltaTime());
		}
		ImGui::End();
	}
}
