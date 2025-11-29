#include "ExportPanel.h"
#include <IconsFontAwesome6.h>
#include "Dialogs/FolderPickerDialog.h"
#include "Export/ExportSettings.h"
#include "Export/ProjectExporter.h"
#include "Panels/DNDPayloads.h"

namespace Laura
{

	void Laura::ExportPanel::OnImGuiRender() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;

		if (m_EditorState->temp.shouldOpenExportPanel && !m_ExportPanelOpen) {
			m_EditorState->temp.shouldOpenExportPanel = false; // reset flag
			m_ExportPanelOpen = true;
			OnPanelOpen();
		}

		if (!m_ExportPanelOpen) {
			return;
		}

		// helper that draws the left-hand label and, if `requiredFalse`, draws a warning star
		auto drawLabel = [&](const char* label, bool requiredSatisfied) {
			ImGui::AlignTextToFramePadding();
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text("%s", label);
			theme.PopColor();

			if (!requiredSatisfied) {
				ImGui::SameLine();
				theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
				ImGui::Text("*");
				theme.PopColor();
			}

			ImGui::SameLine(150.0f);
		};

		float margin = 3.0f;
		theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background3);
		ImGui::SetNextWindowSizeConstraints(ImVec2(400, 350), ImVec2(FLT_MAX, FLT_MAX));
		ImGui::Begin(ICON_FA_ARROW_UP_FROM_BRACKET " Export", &m_ExportPanelOpen, ImGuiWindowFlags_NoDocking);

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::BeginDisabled();
		}

		// compute required flags from current state
		const std::string invalidChars = "\\/:?\"<>|*";
		bool exportProjectNameHasInvalidChars = std::string(m_ExportProjectName).find_first_of(invalidChars) != std::string::npos;
		bool nameSet  = (m_ExportProjectName[0] != '\0' && !exportProjectNameHasInvalidChars);
		bool pathSet  = (!m_Folderpath.empty());
		bool sceneSet = (m_ProjectManager->GetBootSceneGuid() != LR_GUID::INVALID);

		// ----- Executable Name -----
		drawLabel("Executable Name:", nameSet);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - margin);
		if (ImGui::InputTextWithHint("##projectName-input", "Project name", m_ExportProjectName, sizeof(m_ExportProjectName))) {
			m_ExportSuccessful = false;
			m_ExportFailed = false;
			nameSet = (m_ExportProjectName[0] != '\0');
		}

		// ----- Folder selection -----
		drawLabel("Location", pathSet);
		std::string buttonLabel = m_Folderpath.empty() ? "Select folder..." : m_Folderpath.string();
		std::string buttonId = "##folder-button";
		EditorCol_ textCol = m_Folderpath.empty() ? EditorCol_Text2 : EditorCol_Text1;
		theme.PushColor(ImGuiCol_Button, EditorCol_Primary3);
		theme.PushColor(ImGuiCol_Text, textCol);
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2{ 0.0f, 0.5f });
		if (ImGui::Button((buttonLabel + buttonId).c_str(), { ImGui::GetContentRegionAvail().x - margin, 0 })) {
			if (auto path = FolderPickerDialog("Select folder"); !path.empty()) {
				m_Folderpath = path;
				m_ExportSuccessful = false;
				m_ExportFailed = false;
				pathSet = true;
			}
		}
		ImGui::PopStyleVar();
		theme.PopColor(2);

		// ----- Fullscreen checkbox -----
		drawLabel("Fullscreen  " ICON_FA_EXPAND , true);
		ImGui::Checkbox("##RuntimeFullscreen", &m_ExportSettings.fullscreen);

		// ----- Viewport Scaling combo -----
		drawLabel("Viewport Scaling:", true);
		ScreenFitMode currentMode = m_ExportSettings.screenFitMode;
		const char* currentLabel = ScreenFitModeStr[static_cast<int>(currentMode)];
		theme.PushColor(ImGuiCol_FrameBg, EditorCol_Secondary1);
		ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - margin);
		if (ImGui::BeginCombo("##Viewport Mode", currentLabel)) {
			for (int i = 0; i < static_cast<int>(ScreenFitMode::_COUNT); ++i) {
				bool selected = (i == static_cast<int>(currentMode));

				if (selected) { theme.PushColor(ImGuiCol_Header, EditorCol_Accent2); }
				if (ImGui::Selectable(ScreenFitModeStr[i], selected)) {
					m_ExportSettings.screenFitMode = static_cast<ScreenFitMode>(i);
				}
				if (selected) { theme.PopColor(); }
			}
			ImGui::EndCombo();
		}
		theme.PopColor();
		ImGui::PopStyleVar();

		// ----- Boot Scene (drag-drop) -----
		drawLabel("Boot Scene", sceneSet);
		std::string displayName = m_BootSceneTitle.empty() ? "No scene selected" : m_BootSceneTitle;
		DragDropWidget(
			"",
			DNDPayloadTypes::SCENE,
			displayName,
			[&](const DNDPayload& payload) {
				m_ProjectManager->SetBootSceneGuid(payload.guid);
				m_BootSceneTitle = payload.title;
				m_ExportSuccessful = false;
				m_ExportFailed = false;
				sceneSet = (m_ProjectManager->GetBootSceneGuid() != LR_GUID::INVALID);
			},
			theme,
			"Drag a scene asset here from the Assets panel",
			{ ImGui::GetContentRegionAvail().x - margin, 0 },
			!m_BootSceneTitle.empty()
		);

		drawLabel("VSync  " ICON_FA_DESKTOP, true);
		ImGui::Checkbox("##RuntimeVSync", &m_ExportSettings.vSync);

		// ----- Export button (centered) -----
		ImGui::Dummy({ 0.0f, 20.0f });
		const char* label = ICON_FA_ARROW_UP_FROM_BRACKET " Export";
		float totalWidth = ImGui::GetContentRegionAvail().x;
		float buttonWidth = ImGui::GetStyle().FramePadding.x * 2 + ImGui::CalcTextSize(label).x;
		ImGui::SetCursorPosX((totalWidth - buttonWidth) / 2.0f);

		bool canExport = (nameSet && pathSet && sceneSet);

		if (!canExport) ImGui::BeginDisabled();
		theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_Accent2);
		if (ImGui::Button(label, { buttonWidth, 30.0f })) {
			bool success = ExportProject(m_ExportProjectName, m_Folderpath, m_ProjectManager->GetProjectFolder(), m_ExportSettings);
			if (success) {
				m_ExportSuccessful = true;
				m_ExportFailed = false;
			} else {
				m_ExportSuccessful = false;
				m_ExportFailed = true;
			}
		}
		theme.PopColor();
		if (!canExport) ImGui::EndDisabled();

		// ----- Result messages below the button -----
		ImGui::Dummy({ 0.0f, 6.0f });
		if (m_ExportSuccessful) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Success);
			const char* msg = "Export completed successfully!";
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) / 2.0f);
			ImGui::Text(msg);
			theme.PopColor();
		} else if (m_ExportFailed) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Error); // red text
			const char* msg = "Export failed - check the console for more details!";
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) / 2.0f);
			ImGui::Text(msg);
			theme.PopColor();
		}

		if (m_EditorState->temp.isInRuntimeSimulation) {
			ImGui::EndDisabled();
		}

		ImGui::End();
		theme.PopColor(); // windowBg
	}
}
