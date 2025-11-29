#include <Laura.h>
#include <LauraEntrypoint.h>
#include "EditorLayer.h"
#include "EditorCfg.h"

namespace Laura
{

	class LauraEditor : public Application {
	public:
		LauraEditor()
			: Application() {

			m_ImGuiContext = std::make_shared<ImGuiContext>(_Window);
			m_ImGuiContext->Init();

			_LayerStack->PushLayer(std::make_shared<EditorLayer>(_Window, _Profiler, _LayerStack, _ProjectManager, m_ImGuiContext));
		}

		virtual void Shutdown() override {
			m_ImGuiContext->Shutdown();
			Application::Shutdown();
		}
		
		~LauraEditor() {
		}

	private:
		std::shared_ptr<ImGuiContext> m_ImGuiContext;
	};

	Application* CreateApplication(const std::filesystem::path& exeDir) {
		EditorCfg::Init(exeDir); // init EditorCfg::EXECUTABLE_DIR, EditorCfg::RESOURCES_PATH
		return new LauraEditor();
	}
}