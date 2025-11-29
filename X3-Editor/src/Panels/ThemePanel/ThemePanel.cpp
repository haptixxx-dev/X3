#include <IconsFontAwesome6.h>
#include <imgui_internal.h>
#include <filesystem>
#include "ThemePanel.h"
#include "Dialogs/ConfirmationDialog.h"
#include "Dialogs/FilePickerDialog.h"

namespace Laura
{

	void ThemePanel::OnImGuiRender() {
		if (!m_EditorState->temp.isThemePanelOpen) {
			return;
		}


		static std::string errorMessage = "";
		auto& theme = m_EditorState->temp.editorTheme;
		ImGuiWindowFlags ThemePanelFlags = ImGuiWindowFlags_NoDocking;
		ImGui::SetNextWindowSizeConstraints({400.0f, 300.0f}, {FLT_MAX, FLT_MAX});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 3));
		theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background3);
		ImGui::Begin(ICON_FA_BRUSH " Themes", &m_EditorState->temp.isThemePanelOpen, ThemePanelFlags);
		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::BeginDisabled();
		}

		if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
			float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
			theme.PushColor(ImGuiCol_Button, EditorCol_Secondary2);

			if (ImGui::BeginTabItem("General")) {
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Load Theme:");

				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60 - 4 - lineHeight); // 4 pixels padding between buttons

				if (ImGui::Button("Open..", ImVec2(60.0f, lineHeight))) {
					auto themePath = FilePickerDialog(EDITOR_THEME_FILE_EXTENSION, "Select Theme:");
					if (!themePath.empty()) { // when the user selected a file
						auto [success, errMsg] = theme.LoadFromFile(themePath.string());
						if (success) {
							errorMessage = ""; // on success reset error message
							m_EditorState->persistent.editorThemeFilepath = themePath.string();
						}
						else {
							errorMessage = errMsg;
						}
					}
				}

				ImGui::SameLine(ImGui::GetContentRegionAvail().x - lineHeight);
				if (ImGui::Button(ICON_FA_ROTATE, ImVec2(lineHeight, lineHeight))) {
					if (m_EditorState->persistent.editorThemeFilepath != "") {
						auto [success, errMsg] = theme.LoadFromFile(m_EditorState->persistent.editorThemeFilepath);
						errorMessage = errMsg;
					}
					else {
						theme.LoadDefaultDark();
					}
				}

				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::Text("Reload the theme if you made changes to the .lrtheme file");
					ImGui::EndTooltip();
				}

				if (errorMessage != "") {
					ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
					ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), errorMessage.c_str(), ImGui::GetContentRegionAvail().x);
					ImGui::PopTextWrapPos();
				}

				ImGui::AlignTextToFramePadding();
				ImGui::Text("Default Theme:");
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);

				static bool shouldSetDefaultTheme = false;
				if (ImGui::Button(ICON_FA_BRUSH "Apply", ImVec2(70.0f, lineHeight))) {
					shouldSetDefaultTheme = true;
				}

				ConfirmAndExecute(shouldSetDefaultTheme, "Apply Default Theme", "Are you sure you want to switch to the default theme?", [&]() {
					errorMessage = "";
					m_EditorState->persistent.editorThemeFilepath = "";
					theme.LoadDefaultDark();
					}, m_EditorState);

				ImGui::Dummy({ 0.0f, 20.0f });

				// Center Title
				float title_text_width = ImGui::CalcTextSize("Current Theme:").x;
				ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - title_text_width) / 2.0f);
				ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Current Theme:");
				
				// Center Filename
				std::filesystem::path& editorThemeFilepath = m_EditorState->persistent.editorThemeFilepath;
				std::string filename = editorThemeFilepath.filename().string();
				if (filename == "") { filename = "Default Theme"; }
				float filenameWidth = ImGui::CalcTextSize(filename.c_str()).x;
				ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - filenameWidth) / 2.0f);
				ImGui::Text(filename.c_str());
				if (editorThemeFilepath.string() != "" && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNone)) {
					ImGui::SetTooltip(editorThemeFilepath.string().c_str());
				}
				ImGui::EndTabItem();
			}

			if ( ImGui::BeginTabItem("Color Editor") ) {
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Export current colors:");
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - 75.0f);
				static std::string saveErrorMessage = "";
				if (ImGui::Button("Export " ICON_FA_FILE_EXPORT,  ImVec2(75.0f, lineHeight))) {
					auto savePath = SaveFileDialog(EDITOR_THEME_FILE_EXTENSION, "Save Theme File", "Theme");
					if (!savePath.empty()) {
						std::filesystem::path saveFilepath = savePath;
						saveFilepath.replace_extension(EDITOR_THEME_FILE_EXTENSION);
						auto [status, errMsg] = theme.SaveToFile(saveFilepath);
						if (!status) {
							saveErrorMessage = errMsg;
						}
					}
				}
				if (saveErrorMessage != "") {
					ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
					ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), saveErrorMessage.c_str(), ImGui::GetContentRegionAvail().x);
					ImGui::PopTextWrapPos();
				}
				ImGui::Dummy({ 0.0f, 10.0f });



    			constexpr int NumColumns = 2; // Label and color picker
    			ImGui::PushItemWidth(-FLT_MIN); // Full width in each column
    			ImGui::Columns(NumColumns, nullptr, true);
    			for (int i = 0; i < EditorCol_COUNT; ++i) {
        			const char* name = EditorColStrings[i];
        			ImGui::Text(name);
        			ImGui::NextColumn();
					ImGui::PushID(i);
					ImGui::ColorEdit4("##color", (float*)&theme[static_cast<EditorCol_>(i)], ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs);
					ImGui::PopID();
        			ImGui::NextColumn();
    			}
    			ImGui::Columns(1);
    			ImGui::PopItemWidth();
				ImGui::EndTabItem();
			}
			theme.PopColor();
			ImGui::EndTabBar();
		}

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::EndDisabled();
		}
		
		ImGui::End();
		theme.PopColor();
		ImGui::PopStyleVar();
		ImGui::PopStyleVar();
	}
}