#pragma once 

#include "Laura.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

namespace Laura
{

	class SceneHierarchyPanel : public IEditorPanel {
	public:
		SceneHierarchyPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager);
		~SceneHierarchyPanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;
	};
}