#include <imgui.h>
#include "EditorLayer.h"
#include "Panels/ExportPanel/ExportPanel.h"
#include "Panels/ViewportPanel/ViewportPanel.h"
#include "Panels/SceneHierarchyPanel/SceneHierarchyPanel.h"
#include "Panels/InspectorPanel/InspectorPanel.h"
#include "Panels/ProfilerPanel/ProfilerPanel.h"
#include "Panels/RenderSettingsPanel/RenderSettingsPanel.h"
#include "Panels/ThemePanel/ThemePanel.h"
#include "Panels/AssetsPanel/AssetsPanel.h"

namespace Laura
{

	EditorLayer::EditorLayer(std::shared_ptr<IWindow> window,
							 std::shared_ptr<Profiler> profiler,
							 std::shared_ptr<IEventDispatcher> eventDispatcher,
							 std::shared_ptr<ProjectManager> projectManager,
							 std::shared_ptr<ImGuiContext> imGuiContext)
		: m_Window(window)
		, m_Profiler(profiler)
		, m_EventDispatcher(eventDispatcher)
		, m_ProjectManager(projectManager)
		, m_EditorState(std::make_shared<EditorState>())
		, m_ImGuiContext(imGuiContext)
		, m_Launcher(m_EditorState, m_ProjectManager)

		, m_WindowTitleBar(std::make_unique<WindowTitleBar>(m_EditorState, m_EventDispatcher, m_Window, m_ProjectManager, m_ImGuiContext))
		, m_EditorPanels({
			std::make_unique<ExportPanel>(m_EditorState, m_ProjectManager),
			std::make_unique<InspectorPanel>(m_EditorState, m_ProjectManager),
			std::make_unique<SceneHierarchyPanel>(m_EditorState, m_ProjectManager),
			std::make_unique<ViewportPanel>(m_EditorState, m_ProjectManager),
			std::make_unique<ThemePanel>(m_EditorState),
			std::make_unique<ProfilerPanel>(m_EditorState, m_Profiler),
			std::make_unique<RenderSettingsPanel>(m_EditorState, m_EventDispatcher, m_ProjectManager),
			std::make_unique<AssetsPanel>(m_EditorState, m_ProjectManager)
		}){
	}

	void EditorLayer::onAttach() {
		deserializeState(m_EditorState);

		for (auto& panel : m_EditorPanels) {
			panel->init();
		}
	}

	void EditorLayer::onDetach() {
		serializeState(m_EditorState);
	}

	void EditorLayer::onEvent(std::shared_ptr<IEvent> event) { 
		// while in editor mode - consume input events
		if (!m_EditorState->temp.isInRuntimeSimulation && event->IsInputEvent()) {
			event->Consume();
			return; // don't propagate further
		}

		for (auto& panel : m_EditorPanels) {
			panel->onEvent(event);
			if (event->IsConsumed()) {
				break;
			}
		}

		if (event->GetType() != EventType::NEW_FRAME_RENDERED_EVENT)
			std::cout << event->GetType() << std::endl;
	}

	void EditorLayer::onUpdate() {
		m_ImGuiContext->BeginFrame();
		m_EditorState->temp.editorTheme.ApplyAllToImgui(); // apply theme every frame


		float yOffset = 0.0f;
		if (m_Window->isMaximized()) {
			yOffset = 6.0f;
		}

		m_WindowTitleBar->OnImGuiRender(yOffset);

		// next window setup (to align with the titlebar)
		ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + m_WindowTitleBar->height() + yOffset));
		ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - m_WindowTitleBar->height()));
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

		// LAUNCHER
		if (!m_ProjectManager->ProjectIsOpen()) {
			m_Launcher.OnImGuiRender(host_flags);
		} 
		// DOCKSPACE
		else {
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::Begin("##DockSpaceHost", nullptr, host_flags);
			ImGui::PopStyleVar(2);
			ImGuiStyle& style = ImGui::GetStyle();
			float minWinSizeX = style.WindowMinSize.x;
			style.WindowMinSize.x = 300.0f;
			ImGui::DockSpace(ImGui::GetID("MyDockspace"));
			style.WindowMinSize.x = minWinSizeX;

			for (auto& panel : m_EditorPanels) {
				panel->OnImGuiRender(); // RENDERING ALL PANELS
			}

			ImGui::End();
		}

		#ifndef BUILD_INSTALL // display demo when not shipping
		bool showDemo = false;
		ImGui::ShowDemoWindow(&showDemo);
		#endif

		m_ImGuiContext->EndFrame();
	}
}
