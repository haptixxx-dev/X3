#include <X3.h>
#include <X3Entrypoint.h>
#include <filesystem>
#include "RuntimeLayer.h"
#include "RuntimeCfg.h"

namespace X3
{

	class X3Runtime : public Application {
	public:
		X3Runtime(WindowProps windowProps)
			: Application(windowProps) {

			_LayerStack->PushLayer(std::make_shared<RuntimeLayer>(_Window, _Profiler, _LayerStack, _ProjectManager));
		}

		~X3Runtime() {

		}
	};

	Application* CreateApplication(const std::filesystem::path& exeDir) {
		RuntimeCfg::Init(exeDir); // init EXECUTABLE_DIR
		WindowProps spec;
		spec.CustomTitlebar = false;
		return new X3Runtime(spec);
	}
}
