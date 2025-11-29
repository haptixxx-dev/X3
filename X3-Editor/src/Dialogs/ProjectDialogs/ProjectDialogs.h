#pragma once

#include <filesystem>
#include "Laura.h"
#include "EditorState.h"
#include "Dialogs/FolderPickerDialog.h"
#include "Dialogs/FilePickerDialog.h"

namespace Laura
{
	
	void CreateProjectDialog(std::shared_ptr<ProjectManager> m_ProjectManager, std::shared_ptr<EditorState> editorState) {
		if (!editorState->temp.isCreateProjectDialogOpen) {
			return;
		}
		
		auto& theme = editorState->temp.editorTheme;
		theme.PushColor(ImGuiCol_WindowBg, EditorCol_Background3);
		ImGui::SetNextWindowSizeConstraints(ImVec2(400, 170), ImVec2(FLT_MAX, FLT_MAX));
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking;
		ImGui::Begin("New Project " ICON_FA_DIAGRAM_PROJECT, &editorState->temp.isCreateProjectDialogOpen, flags);

		float margin = 3.0f;
		static char projectName[250] = "";
		static std::filesystem::path folderpath = "";
		static bool folderpathSelected = false;

		// Project Name
		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		ImGui::Text("Project Name");
		theme.PopColor();
		ImGui::SameLine(150.0f);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - margin);
		ImGui::InputTextWithHint("##projectName-input", "Project name", projectName, IM_ARRAYSIZE(projectName));

		// Folder Path Button (acts as label + picker)
		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		ImGui::Text("Location");
		theme.PopColor();
		ImGui::SameLine(150.0f);
		std::string buttonLabel = folderpath.empty()
			? "Select folder..."
			: folderpath.string();
		std::string buttonId = "##folder-button";
		EditorCol_ textCol = folderpathSelected ? EditorCol_Text1 : EditorCol_Text2;
		theme.PushColor(ImGuiCol_Button, EditorCol_Primary3);
		theme.PushColor(ImGuiCol_Text, textCol);
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2{ 0.0f, 0.5f });
		if (ImGui::Button((buttonLabel + buttonId).c_str(), { ImGui::GetContentRegionAvail().x - margin, 0 })) {
			if (auto path = FolderPickerDialog("Select folder"); !path.empty()) {
				folderpath = path;
				folderpathSelected = true;
			}
		}
		ImGui::PopStyleVar();
		theme.PopColor();
		theme.PopColor();

		// Validation feedback
		bool validProjectName = strlen(projectName) > 0;
		// Simple check for invalid path characters (adjust as needed)
		std::string projectNameStr(projectName);
		const std::string invalidChars = "\\/:?\"<>|*";
		bool projectNameHasInvalidChars = projectNameStr.find_first_of(invalidChars) != std::string::npos;
		bool validFolderPath = folderpathSelected && std::filesystem::exists(folderpath) && std::filesystem::is_directory(folderpath);

		std::string validationMsg;
		if (!validProjectName) {
			validationMsg = "Project name cannot be empty.";
		}
		else if (projectNameHasInvalidChars) {
			validationMsg = "Project name contains invalid characters: \\ / : ? \" < > | *";
		}
		else if (!validFolderPath) {
			validationMsg = "Please select a valid folder location.";
		}
		else {
			auto projectPath = folderpath / projectName;
			if (std::filesystem::exists(projectPath)) {
				validationMsg = "Project folder already exists.";
			}
		}

		// Show validation message
		if (!validationMsg.empty()) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
			ImGui::TextWrapped("%s", validationMsg.c_str());
			theme.PopColor();
			ImGui::Dummy(ImVec2(0, 5));
		}

		float createProjectBtnHeight = 30.0f;
		ImVec2 windowSize = ImGui::GetContentRegionAvail();
		ImGui::Dummy({ 0.0f, (windowSize.y - createProjectBtnHeight) / 2.0f });

		// center horizontal
		std::string createProjectLabel = "Create Project " ICON_FA_SHARE;
		float centerX = (windowSize.x - ImGui::CalcTextSize(createProjectLabel.c_str()).x) / 2.0f;
		ImGui::SetCursorPosX(centerX);

		bool canCreate = validationMsg.empty();
		if (!canCreate) {
			ImGui::BeginDisabled();
		}
		if (ImGui::Button(createProjectLabel.c_str(), { 0, createProjectBtnHeight }) && canCreate) {
			auto projectPath = folderpath / projectName;
			if (m_ProjectManager->NewProject(projectPath)) {
				editorState->temp.isCreateProjectDialogOpen = false;

				// Reset static inputs on success (optional)
				memset(projectName, 0, sizeof(projectName));
				folderpath.clear();
				folderpathSelected = false;
			}
		}
		if (!canCreate) {
			ImGui::EndDisabled();
		}

		ImGui::End();
		theme.PopColor();
	}

	void OpenProjectDialog(std::shared_ptr<ProjectManager> m_ProjectManager) {
		if (auto filepath = FilePickerDialog(PROJECT_FILE_EXTENSION, "Select Project File:"); !filepath.empty()) {
			m_ProjectManager->OpenProject(filepath);
		}
	}
}