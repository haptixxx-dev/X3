#include <IconsFontAwesome6.h>
#include "Panels/RenderSettingsPanel/RenderSettingsPanel.h"

namespace Laura
{

	void RenderSettingsPanel::init() {
		// sync the renderer with the newely deserialized render settings 
		m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(m_EditorState->persistent.editorRenderSettings));
	}

	void RenderSettingsPanel::OnImGuiRender() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;

		auto CellLabelCentered = [&theme](const char* label) {
			float columnWidth = ImGui::GetColumnWidth();
			float textWidth = ImGui::CalcTextSize(label).x;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - textWidth) * 0.5f);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%s", label);
		};

		auto DrawSection = [&](const char* label) {
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableSetColumnIndex(0);
			theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
			CellLabelCentered(label);
			theme.PopColor();
			ImGui::TableSetColumnIndex(1);
			ImGui::TableSetColumnIndex(2);
		};

		auto DrawLabel = [&](const char* label) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%s", label);
			theme.PopColor();
		};

		ImGui::SetNextWindowSizeConstraints(ImVec2(400, 200), ImVec2(FLT_MAX, FLT_MAX));
		ImGui::Begin(ICON_FA_WRENCH " Render Settings");

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::BeginDisabled();
		}

		float avail_width = ImGui::GetContentRegionAvail().x;
		ImGui::BeginChild("child_with_margin", ImVec2(avail_width - 5.0f, 0), false);

		RenderSettings& editorSettings = m_EditorState->persistent.editorRenderSettings;
		RenderSettings& runtimeSettings = m_ProjectManager->GetMutableRuntimeRenderSettings();

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));  // thinner widgets
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));   // less vertical gap
		if (ImGui::BeginTable("RenderSettingsTable", 3)) {
			ImGui::TableSetupColumn("##Setting", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##Runtime", ImGuiTableColumnFlags_WidthStretch);

			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableSetColumnIndex(0);

			theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
			CellLabelCentered("Quality");
			theme.PopColor();
			ImGui::TableSetColumnIndex(1);
			CellLabelCentered("Editor");
			ImGui::TableSetColumnIndex(2);
			CellLabelCentered("Runtime");

			// Resolution row
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			DrawLabel("Resolution " ICON_FA_DISPLAY);
			ImGui::TableSetColumnIndex(1);
			{
				ImGui::PushID("##EditorResolution");
				int current_idx = 0;
				for (int i = 0; i < m_ResolutionOptions.size(); i++) {
					if (m_ResolutionOptions[i].resolution == editorSettings.resolution) { current_idx = i; break; }
				}
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::BeginCombo("##Combo", m_ResolutionOptions[current_idx].label, ImGuiComboFlags_HeightLarge)) {
					for (int n = 0; n < m_ResolutionOptions.size(); n++) {
						const auto& label = m_ResolutionOptions[n].label;
						if (m_ResolutionOptions[n].resolution == glm::uvec2(0, 0)) {
							if (n != 0) { ImGui::TextDisabled(""); } // empty line
							ImGui::TextDisabled("%s", label);
							continue;
						}
						bool selected = (current_idx == n);
						if (selected) { theme.PushColor(ImGuiCol_Header, EditorCol_Accent2); }
						if (ImGui::Selectable(label, selected)) {
							current_idx = n;
							editorSettings.resolution = m_ResolutionOptions[n].resolution;
							m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
						}
						if (selected) { theme.PopColor(); ImGui::SetItemDefaultFocus(); }
					}
					ImGui::EndCombo();
				}
				ImGui::PopID();
			}
			ImGui::TableSetColumnIndex(2);
			{
				ImGui::PushID("##RuntimeResolution");
				int current_idx = 0;
				for (int i = 0; i < m_ResolutionOptions.size(); i++) {
					if (m_ResolutionOptions[i].resolution == runtimeSettings.resolution) { current_idx = i; break; }
				}
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
				if (ImGui::BeginCombo("##Combo", m_ResolutionOptions[current_idx].label, ImGuiComboFlags_HeightLarge)) {
					for (int n = 0; n < m_ResolutionOptions.size(); n++) {
						const auto& label = m_ResolutionOptions[n].label;
						if (m_ResolutionOptions[n].resolution == glm::uvec2(0, 0)) {
							if (n != 0) { ImGui::TextDisabled(""); } // empty line
							ImGui::TextDisabled("%s", label);
							continue;
						}
						bool selected = (current_idx == n);
						if (selected) { theme.PushColor(ImGuiCol_Header, EditorCol_Accent2); }
						if (ImGui::Selectable(m_ResolutionOptions[n].label, selected)) {
							current_idx = n;
							runtimeSettings.resolution = m_ResolutionOptions[n].resolution;
						}
						if (selected) { theme.PopColor(); ImGui::SetItemDefaultFocus(); }
					}
					ImGui::EndCombo();
				}
				ImGui::PopID();
			}

			// Rays Per Pixel
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			DrawLabel("Samples per Pixel");
			ImGui::TableSetColumnIndex(1);
			ImGui::PushItemWidth(-FLT_MIN);
			if (ImGui::SliderInt("##EditorRPP", &editorSettings.raysPerPixel, 1, 100)) {
				m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::PushItemWidth(-FLT_MIN);
			ImGui::SliderInt("##RuntimeRPP", &runtimeSettings.raysPerPixel, 1, 100);

			// Bounces Per Ray
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			DrawLabel("Bounces per Ray");
			ImGui::TableSetColumnIndex(1);
			ImGui::PushItemWidth(-FLT_MIN);
			if (ImGui::SliderInt("##EditorBounces", &editorSettings.bouncesPerRay, 0, 100)) {
				m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::PushItemWidth(-FLT_MIN);
			ImGui::SliderInt("##RuntimeBounces", &runtimeSettings.bouncesPerRay, 0, 100);

			// Accumulate
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			DrawLabel("Accumulate Light");
			ImGui::TableSetColumnIndex(1);
			if (ImGui::Checkbox("##EditorAccumulate", &editorSettings.accumulate)) {
				m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::Checkbox("##RuntimeAccumulate", &runtimeSettings.accumulate);
			ImGui::EndTable();
		}

		ImGui::Dummy({ 0.0f, 10.0f });

		if (ImGui::BeginTable("##Editor-Debug-RenderSettings", 2)) {
			ImGui::TableSetupColumn("##Editor-Debug-Col", ImGuiTableColumnFlags_WidthFixed, 100.0f);
			ImGui::TableSetupColumn("##Widget-Col", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			ImGui::TableSetColumnIndex(0);
			CellLabelCentered("Editor-Debug");

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); 
			DrawLabel("AABB Heatmap");
			ImGui::TableSetColumnIndex(1);
			bool isAabbSelected = (editorSettings.debugMode == 1);
			if (ImGui::Checkbox("##ShowAabbHeatmap", &isAabbSelected)) {
				editorSettings.debugMode = (isAabbSelected) ? 1 : 0;
				m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::DragInt("##AabbHeatmapCutoff", &editorSettings.aabbHeatmapCutoff, 10.0f, 1, 100000, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
			}

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); 
			DrawLabel("Triangle Heatmap");
			ImGui::TableSetColumnIndex(1);
			bool isTriSelected = (editorSettings.debugMode == 2);
			if (ImGui::Checkbox("##ShowTriHeatmap", &isTriSelected)) {
				editorSettings.debugMode = (isTriSelected) ? 2 : 0;
				m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			if (ImGui::DragInt("##TriangleHeatmapCutoff", &editorSettings.triangleHeatmapCutoff, 1.0f, 1, 10000, "%d", ImGuiSliderFlags_AlwaysClamp)) {
				m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(editorSettings));
			}

			ImGui::EndTable();
		}
		ImGui::PopStyleVar(2);
		ImGui::EndChild();

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::EndDisabled();
		}

		ImGui::End();
	}
}