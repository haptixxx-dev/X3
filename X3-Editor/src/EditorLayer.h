#pragma once

#include <Laura.h>
#include "EditorState.h"
#include "ImGuiContext.h"
#include "Panels/IEditorPanel.h"
#include "Panels/Launcher/Launcher.h"
#include "WindowTitleBar/WindowTitleBar.h"

namespace Laura
{

	class EditorLayer : public ILayer {
	public:
		EditorLayer(std::shared_ptr<IWindow> window,
					std::shared_ptr<Profiler> profiler,
					std::shared_ptr<IEventDispatcher> eventDispatcher,
					std::shared_ptr<ProjectManager> projectManager,
					std::shared_ptr<ImGuiContext> imGuiContext
		);

		virtual void onAttach() override;
		virtual void onDetach() override;
		virtual void onUpdate() override;
		virtual void onEvent(std::shared_ptr<IEvent> event) override;

	private:
		// Engine Systems
		std::shared_ptr<IWindow> m_Window;
		std::shared_ptr<Profiler> m_Profiler;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher; // layerstack  
		std::shared_ptr<ProjectManager> m_ProjectManager;

		// Editor Systems
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ImGuiContext> m_ImGuiContext; // passed from LauraEditor.cpp

		Launcher m_Launcher;

		std::shared_ptr<WindowTitleBar> m_WindowTitleBar;
		// Editor Panels
		std::array<std::unique_ptr<IEditorPanel>, 8> m_EditorPanels;
	};
}