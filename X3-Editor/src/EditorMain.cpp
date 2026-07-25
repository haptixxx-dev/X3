#include <X3.h>
#include <X3Entrypoint.h>
#include "EditorLayer.h"
#include "EditorCfg.h"

namespace X3
{

	class X3Editor : public Application {
	public:
		X3Editor()
			: Application() {

			m_ImGuiContext = std::make_shared<ImGuiContext>(_Window);
			m_ImGuiContext->Init();

			_LayerStack->PushLayer(std::make_shared<EditorLayer>(_Window, _Profiler, _LayerStack, _ProjectManager, m_ImGuiContext));

			// Without an open project the editor sits on the launcher and the
			// render layer never dispatches, so automated runs exercise only
			// Vulkan init. This lets a harness get to the render path.
			if (const char* projectPath = std::getenv("X3_OPEN_PROJECT")) {
				if (_ProjectManager->OpenProject(projectPath))
					LOG_EDITOR_INFO("Opened project from X3_OPEN_PROJECT: {}", projectPath);
				else
					LOG_EDITOR_ERROR("X3_OPEN_PROJECT set but could not open: {}", projectPath);
			}
		}

		virtual void Shutdown() override {
			m_ImGuiContext->Shutdown();
			Application::Shutdown();
		}
		
		~X3Editor() {
		}

	private:
		std::shared_ptr<ImGuiContext> m_ImGuiContext;
	};

	Application* CreateApplication(const std::filesystem::path& exeDir) {
		EditorCfg::Init(exeDir); // init EditorCfg::EXECUTABLE_DIR, EditorCfg::RESOURCES_PATH
		return new X3Editor();
	}
}