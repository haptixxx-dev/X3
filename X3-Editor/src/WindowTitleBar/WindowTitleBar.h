#pragma once

#include "Laura.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"
#include "ImGuiContext.h"

namespace Laura
{

	class WindowTitleBar {
	public:
		WindowTitleBar(std::shared_ptr<EditorState> editorState, 
					   std::shared_ptr<IEventDispatcher> eventDispatcher, 
					   std::shared_ptr<IWindow> window,
			           std::shared_ptr<ProjectManager> projectManager,
					   std::shared_ptr<ImGuiContext> imGuiContext)
			: m_EditorState(editorState)
			, m_EventDispatcher(eventDispatcher)
			, m_Window(window)
			, m_ProjectManager(projectManager)
			, m_ImGuiContext(imGuiContext)
		{}

		~WindowTitleBar() = default;

		inline void init(){};
		void OnImGuiRender(float yOffset);
		void onEvent(std::shared_ptr<IEvent> event){}
		inline float height() {
			return m_TitleBarHeight;

		}

	private:
		bool m_ShouldOpenProject = false, m_ShouldCloseProject = false;
		float m_TitleBarHeight;

		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher;
		std::shared_ptr<IWindow> m_Window;
		std::shared_ptr<ProjectManager> m_ProjectManager;
		std::shared_ptr<ImGuiContext> m_ImGuiContext;
	};
}