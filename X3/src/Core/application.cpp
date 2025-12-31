#include "Core/application.h"
#include "Core/IWindow.h"
#include "Core/Layers/LayerStack.h"
#include "Core/Layers/RenderLayer.h"
#include "Core/Layers/PhysicsLayer.h"
#include "Core/Profiler.h"
#include "Core/Time.h"
#include "Project/ProjectManager.h"
#include "Renderer/IRendererAPI.h"
#include "Events/IEvent.h"

namespace X3 
{
	Application::Application(WindowProps windowProps) {
		Log::Init();
		LOG_ENGINE_INFO("C++ version: {0}", __cplusplus);
		
		_Profiler = std::make_shared<Profiler>(500);

		_Window = IWindow::createWindow(windowProps);
		_LayerStack = std::make_shared<LayerStack>();
		// make window forward events to the layerStack
		_Window->setEventCallback([this](std::shared_ptr<IEvent> event) { _LayerStack->dispatchEvent(event); });

		_ProjectManager = std::make_shared<ProjectManager>();

		_RendererAPI = IRendererAPI::Create();
		_RendererAPI->Init();

		_RenderLayer = std::make_shared<RenderLayer>(_LayerStack, _Profiler, _ProjectManager);
		_PhysicsLayer = std::make_shared<PhysicsLayer>(_ProjectManager);

		_LayerStack->PushLayer(_RenderLayer);
		_LayerStack->PushLayer(_PhysicsLayer);
	}

	void Application::Shutdown(){
		_LayerStack->onDetach();
	}

	void Application::run() {
		// mainloop
		while (!_Window->shouldClose()) {
			Time::Update(); // Update frame timing at start of each frame
			auto t = _Profiler->globalTimer("GLOBAL");
			{
				auto t = _Profiler->timer("Window::OnUpdate()");
				_Window->onUpdate();
			}
			#ifdef BUILD_INSTALL
			_RendererAPI->Clear({ 0.0f, 0.0f, 0.0f, 1.0f }); // black when shipped 
			#else
			_RendererAPI->Clear({ 0.98f, 0.24f, 0.97f, 1.0f }); // bright pink (for debugging)
			#endif
			{
				auto t = _Profiler->timer("LayerStack::onUpdate()");
				_LayerStack->onUpdate();
			}
		}

		Shutdown();
	}
}