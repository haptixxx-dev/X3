#pragma once

#include "Laura.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"
#include "Export/ExportSettings.h"

namespace Laura
{

	class ExportPanel : public IEditorPanel {
	public:
		ExportPanel(std::shared_ptr<EditorState> editorState, 
			        std::shared_ptr<ProjectManager> projectManager)
			: m_EditorState(editorState)
			, m_ProjectManager(projectManager) 
		{}

		~ExportPanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

		inline void OnPanelOpen() {
			// copy current project name with _export suffix
			std::string projectNameWithExport = m_ProjectManager->GetProjectName() + "_export";
			strncpy(m_ExportProjectName, projectNameWithExport.c_str(), sizeof(m_ExportProjectName) - 1);
			m_ExportProjectName[sizeof(m_ExportProjectName) - 1] = '\0';
			m_Folderpath = "";

			m_ExportSuccessful = false;
			m_ExportFailed = false;

			// set the currently set boot scene
			if (auto scnManager = m_ProjectManager->GetSceneManager()) {
				if (auto bootScene = scnManager->find(m_ProjectManager->GetBootSceneGuid())) {
					m_BootSceneTitle = bootScene->name;
				}
			}
			// sync export settings with current viewport mode
			m_ExportSettings.screenFitMode = m_EditorState->persistent.viewportMode;
		}

	private:
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher;
		std::shared_ptr<ProjectManager> m_ProjectManager;

		bool m_ExportPanelOpen = false;

		// serialized data
		ExportSettings m_ExportSettings;

		// non serialized
		char m_ExportProjectName[250] = "";
		std::filesystem::path m_Folderpath = "";
		std::string m_BootSceneTitle = "";

		bool m_ExportSuccessful = false;
		bool m_ExportFailed = false;
	};
}
