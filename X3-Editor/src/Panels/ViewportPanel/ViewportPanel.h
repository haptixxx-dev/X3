#pragma once

#include "Laura.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

namespace Laura 
{

	class ViewportPanel : public IEditorPanel {
	public:
		ViewportPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager) 
			: m_EditorState(editorState), m_ProjectManager(projectManager) {}
		~ViewportPanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual void onEvent(std::shared_ptr<IEvent> event);
	
	private:
		void DrawDropTargetForScene();
		void DrawViewportSettingsPanel();
		void DrawVieportSettingsButton();
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;

		std::weak_ptr<IImage2D> m_LatestRenderedFrame;

		glm::ivec2 m_TargetImageDimensions, m_PrevImageDimensions, m_PrevWindowDimensions;
		glm::ivec2 m_PrevWindowPosition, m_TopLeftImageCoords, m_BottomRightImageCoords;
		glm::ivec2 ImageDimensions, WindowDimensions, TLWindowPosition;
		bool forceUpdate;
	};
}