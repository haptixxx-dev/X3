#pragma once

#include "Core/Layers/ILayer.h"
#include "Core/Layers/LayerStack.h"
#include "Core/IWindow.h"
#include "Physics/PhysicsWorld.h"
#include "Gameplay/FirstPersonController.h"
#include "Project/ProjectManager.h"

namespace X3
{
	class PhysicsLayer;

	// RuntimeLayer handles gameplay systems during play mode
	// - First-person camera control
	// - Character movement
	// - Flow state management
	// - Integration with physics
	class RuntimeLayer : public ILayer
	{
	public:
		RuntimeLayer(
			std::shared_ptr<IWindow> window,
			std::shared_ptr<ProjectManager> projectManager,
			std::shared_ptr<IEventDispatcher> eventDispatcher,
			PhysicsLayer* physicsLayer
		);
		~RuntimeLayer() override = default;

		void onAttach() override;
		void onDetach() override;
		void onUpdate() override;
		void onEvent(std::shared_ptr<IEvent> event) override;

		// Play mode control
		void StartPlayMode();
		void StopPlayMode();
		bool IsPlaying() const { return m_IsPlaying; }

		// Player entity management
		void SetPlayerEntity(entt::entity entity) { m_PlayerEntity = entity; }
		entt::entity GetPlayerEntity() const { return m_PlayerEntity; }

		// Access first person controller for external configuration
		FirstPersonController& GetFirstPersonController() { return m_FPController; }

	private:
		void UpdatePlayer(float deltaTime);
		void UpdateCamera(float deltaTime);
		void UpdateFlow(float deltaTime);

		// Find and cache player entity (entity with FirstPersonCameraComponent + CharacterControllerComponent)
		void FindPlayerEntity();

	private:
		std::shared_ptr<IWindow> m_Window;
		std::shared_ptr<ProjectManager> m_ProjectManager;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher;
		PhysicsLayer* m_PhysicsLayer = nullptr;

		FirstPersonController m_FPController;

		entt::entity m_PlayerEntity = entt::null;

		bool m_IsPlaying = false;
		bool m_CursorWasLocked = false;
	};
}
