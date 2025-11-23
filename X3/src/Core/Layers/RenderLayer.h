#pragma once

#include "lrpch.h"
#include "Project/Scene/Scene.h"
#include "Core/Layers/ILayer.h"
#include "Core/Layers/LayerStack.h"
#include "Core/Events/IEvent.h"
#include "Renderer/Renderer.h"
#include "Project/ProjectManager.h"

namespace Laura
{

	class RenderLayer : public ILayer {
	public:
		RenderLayer(std::shared_ptr<IEventDispatcher> eventDispatcher,
					std::shared_ptr<Profiler> profiler,
					std::shared_ptr<const ProjectManager> projectManager
		);

		virtual void onAttach() override;
		virtual void onDetach() override;

		// onUpdate RenderLayer dispatches an event with the rendered texture
		virtual void onUpdate() override;
		virtual void onEvent(std::shared_ptr<IEvent> event) override;

	private:
		std::shared_ptr<Profiler> m_Profiler;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher;
		std::shared_ptr<const ProjectManager> m_ProjectManager;

		Renderer m_Renderer;
	};
}
