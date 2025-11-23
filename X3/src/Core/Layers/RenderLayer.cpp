#include "Core/Layers/RenderLayer.h"
#include "Core/Events/RenderEvents.h"
#include "Project/Scene/SceneManager.h"
#include "Project/Assets/AssetManager.h"

namespace Laura
{

	RenderLayer::RenderLayer(std::shared_ptr<IEventDispatcher> eventDispatcher, 
							 std::shared_ptr<Profiler> profiler,
							 std::shared_ptr<const ProjectManager> projectManager)
		:	m_EventDispatcher(eventDispatcher), 
			m_Profiler(profiler), 
			m_ProjectManager(projectManager),
			m_Renderer(profiler) {
	}

	void RenderLayer::onAttach() {
		m_Renderer.Init();
	}

	void RenderLayer::onDetach() {
	}

	void RenderLayer::onUpdate() {
		if (m_ProjectManager->ProjectIsOpen()) { // Get...Manager should not return nullptr
			const auto& scene = m_ProjectManager->GetSceneManager()->GetOpenScene();
			const auto& assetPool = m_ProjectManager->GetAssetManager()->GetAssetPool();
			std::shared_ptr<IImage2D> RenderedFrame = m_Renderer.Render(scene.get(), assetPool.get());
			m_EventDispatcher->dispatchEvent(std::make_shared<NewFrameRenderedEvent>(RenderedFrame));
		}
	}

	void RenderLayer::onEvent(std::shared_ptr<IEvent> event) {
		if (event->GetType() == EventType::UPDATE_RENDER_SETTINGS_EVENT) {
			RenderSettings settings = std::dynamic_pointer_cast<UpdateRenderSettingsEvent>(event)->renderSettings;
			m_Renderer.applySettings(settings);
		}
	}
}
