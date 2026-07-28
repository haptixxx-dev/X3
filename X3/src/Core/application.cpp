#include "Core/application.h"
#include "Core/IWindow.h"
#include "Core/Layers/LayerStack.h"
#include "Core/Layers/RenderLayer.h"
#include "Core/Layers/PhysicsLayer.h"
#include "Core/Profiler.h"
#include "Core/Time.h"
#include "Project/ProjectManager.h"
#include "Events/IEvent.h"
#include "Platform/Vulkan/VulkanContext.h"

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

		_RenderLayer = std::make_shared<RenderLayer>(_LayerStack, _Profiler, _ProjectManager);
		_PhysicsLayer = std::make_shared<PhysicsLayer>(_ProjectManager, _LayerStack);

		_LayerStack->PushLayer(_RenderLayer);
		_LayerStack->PushLayer(_PhysicsLayer);
	}

	void Application::Shutdown(){
		_LayerStack->onDetach();
	}

	void Application::run() {
		// THE frame lifecycle lives here, not in the window. beginFrame() opens the
		// command buffer that every layer records into, so it must bracket
		// LayerStack::onUpdate() within one iteration -- that bracketing is what
		// makes VulkanContext.h's frame-slot invariant hold structurally: every CPU
		// write to a slot indexed by frame.index() happens after the fence wait in
		// beginFrame() and before the submit in endFrame().
		//
		// The window is no longer involved. It polls events and nothing else; the
		// old IWindow::swapBuffers() went with the OpenGL backend.
		VulkanContext* context = VulkanContext::Get();

		// mainloop
		while (!_Window->shouldClose()) {
			Time::Update(); // Update frame timing at start of each frame
			auto t = _Profiler->globalTimer("GLOBAL");

			// 1. Poll events (handle input immediately)
			{
				auto t = _Profiler->timer("PollEvents");
				_Window->pollEvents();
			}

			// 2. Open the frame. A null return means the swapchain was out of date
			//    and has already been recreated: skip the ENTIRE frame, including
			//    endFrame() and present(). No counter advanced, so the next
			//    iteration retries the same slot. There is no retry loop here on
			//    purpose -- a click-and-drag resize produces a run of these and each
			//    one still drains the deletion queue.
			const FrameContext* frame = nullptr;
			{
				auto t = _Profiler->timer("BeginFrame");
				frame = context->beginFrame();
			}
			if (!frame) {
				continue;
			}

			// 3. Record. Layers reach the command buffer through the context; Part 3
			//    threads `frame` down to them explicitly.
			{
				auto t = _Profiler->timer("LayerStack::onUpdate()");
				_LayerStack->onUpdate();
			}

			// 4. Submit, then present. endFrame() guarantees the swapchain image
			//    ends in PRESENT_SRC_KHR even if no layer wrote to it.
			{
				auto t = _Profiler->timer("EndFrame");
				context->endFrame();
			}
			{
				auto t = _Profiler->timer("Present");
				context->present();
			}
		}

		Shutdown();
	}
}