#pragma once

#include <Laura.h>
#include "EditorState.h"
#include <filesystem>
#include <optional>


namespace Laura 
{

	class Launcher {
	public:
		Launcher(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager) 
			: m_EditorState(editorState), 
			  m_ProjectManager(projectManager) {
		}
		~Launcher() = default;
	
		void OnImGuiRender(ImGuiWindowFlags window_flags);
		void DrawCreateProjectWindow();

	private:
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;

		bool m_CreateProjectWindowOpen = false;
	};
}
