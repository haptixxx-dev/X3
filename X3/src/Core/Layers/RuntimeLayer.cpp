#include "Core/Layers/RuntimeLayer.h"
#include "Core/Layers/PhysicsLayer.h"
#include "Core/Time.h"
#include "Core/Events/KeyEvents.h"
#include "Core/Events/MouseEvents.h"
#include "Project/Scene/Scene.h"
#include "Project/Scene/SceneManager.h"
#include "Core/Log.h"
#include <GLFW/glfw3.h>

namespace X3
{
	RuntimeLayer::RuntimeLayer(
		std::shared_ptr<IWindow> window,
		std::shared_ptr<ProjectManager> projectManager,
		std::shared_ptr<IEventDispatcher> eventDispatcher,
		PhysicsLayer* physicsLayer)
		: m_Window(window)
		, m_ProjectManager(projectManager)
		, m_EventDispatcher(eventDispatcher)
		, m_PhysicsLayer(physicsLayer)
	{
	}

	void RuntimeLayer::onAttach()
	{
		m_FPController.Initialize(m_Window.get());
		LOG_ENGINE_INFO("RuntimeLayer attached");
	}

	void RuntimeLayer::onDetach()
	{
		if (m_IsPlaying) {
			StopPlayMode();
		}
		LOG_ENGINE_INFO("RuntimeLayer detached");
	}

	void RuntimeLayer::StartPlayMode()
	{
		if (m_IsPlaying) return;

		m_IsPlaying = true;

		// Find player entity
		FindPlayerEntity();

		// Lock cursor for FPS controls
		m_FPController.SetCursorLocked(true);
		GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(m_Window->getNativeWindow());
		glfwSetInputMode(glfwWindow, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

		// Enable raw mouse input if available
		if (glfwRawMouseMotionSupported()) {
			glfwSetInputMode(glfwWindow, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
		}

		LOG_ENGINE_INFO("Play mode started");
	}

	void RuntimeLayer::StopPlayMode()
	{
		if (!m_IsPlaying) return;

		m_IsPlaying = false;

		// Unlock cursor
		m_FPController.SetCursorLocked(false);
		GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(m_Window->getNativeWindow());
		glfwSetInputMode(glfwWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		glfwSetInputMode(glfwWindow, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);

		m_PlayerEntity = entt::null;

		LOG_ENGINE_INFO("Play mode stopped");
	}

	void RuntimeLayer::onUpdate()
	{
		if (!m_IsPlaying) return;

		float deltaTime = Time::GetDeltaTime();

		UpdatePlayer(deltaTime);
		UpdateCamera(deltaTime);
		UpdateFlow(deltaTime);
	}

	void RuntimeLayer::onEvent(std::shared_ptr<IEvent> event)
	{
		if (!m_IsPlaying) return;

		switch (event->GetType()) {
			case EventType::MOUSE_MOVE_EVENT: {
				auto* moveEvent = static_cast<MouseMoveEvent*>(event.get());
				m_FPController.OnMouseMove(
					static_cast<float>(moveEvent->xpos),
					static_cast<float>(moveEvent->ypos)
				);
				break;
			}

			case EventType::KEY_PRESS_EVENT: {
				auto* keyEvent = static_cast<KeyPressEvent*>(event.get());
				// Escape to exit play mode
				if (keyEvent->key == Key::ESCAPE) {
					StopPlayMode();
				}
				break;
			}

			default:
				break;
		}
	}

	void RuntimeLayer::FindPlayerEntity()
	{
		Scene* scene = m_ProjectManager->GetSceneManager()->GetOpenScene().get();
		if (!scene) {
			LOG_ENGINE_WARN("No active scene for RuntimeLayer");
			return;
		}

		auto* registry = scene->GetRegistry();

		// Find entity with both FirstPersonCameraComponent and CharacterControllerComponent
		auto view = registry->view<FirstPersonCameraComponent, CharacterControllerComponent, TransformComponent>();
		for (auto entity : view) {
			m_PlayerEntity = entity;
			LOG_ENGINE_INFO("Found player entity");
			return;
		}

		// Fallback: find any entity with CharacterControllerComponent
		auto ccView = registry->view<CharacterControllerComponent, TransformComponent>();
		for (auto entity : ccView) {
			m_PlayerEntity = entity;
			LOG_ENGINE_WARN("Using CharacterController entity as player (no FirstPersonCameraComponent)");
			return;
		}

		LOG_ENGINE_WARN("No player entity found in scene");
	}

	void RuntimeLayer::UpdatePlayer(float deltaTime)
	{
		if (m_PlayerEntity == entt::null) return;

		Scene* scene = m_ProjectManager->GetSceneManager()->GetOpenScene().get();
		if (!scene) return;

		auto* registry = scene->GetRegistry();

		// Get required components
		if (!registry->valid(m_PlayerEntity)) {
			m_PlayerEntity = entt::null;
			return;
		}

		auto* cc = registry->try_get<CharacterControllerComponent>(m_PlayerEntity);
		auto* transform = registry->try_get<TransformComponent>(m_PlayerEntity);
		auto* fpCamera = registry->try_get<FirstPersonCameraComponent>(m_PlayerEntity);

		if (!cc || !transform) return;

		// Get physics controller
		PhysicsWorld* physicsWorld = m_PhysicsLayer ? m_PhysicsLayer->GetPhysicsWorld() : nullptr;
		PhysicsCharacterController* physicsController = nullptr;
		if (physicsWorld) {
			physicsController = physicsWorld->GetCharacterController(m_PlayerEntity);
		}

		// Update grounded state from physics
		if (physicsController) {
			cc->isGrounded = physicsController->IsGrounded();
		}

		// Create temporary camera for movement calculations if no FP camera
		FirstPersonCameraComponent tempCamera;
		if (!fpCamera) {
			// Use transform rotation for yaw
			glm::vec3 rot = transform->GetRotation();
			tempCamera.currentYaw = glm::radians(rot.y);
			tempCamera.currentPitch = glm::radians(rot.x);
			fpCamera = &tempCamera;
		}

		// Calculate movement
		glm::vec3 desiredVelocity = m_FPController.UpdateMovement(*cc, *fpCamera, deltaTime);

		// Apply to physics
		if (physicsController) {
			physicsController->SetInputVelocity(glm::vec3(desiredVelocity.x, 0, desiredVelocity.z));

			// Handle jump
			if (cc->state == MovementState::Jumping && cc->velocity.y > 0) {
				// Apply jump velocity
				physicsController->SetLinearVelocity(desiredVelocity);
			}

			// Sync back velocity from physics
			cc->velocity = physicsController->GetLinearVelocity();
		}

		// Update transform from physics
		if (physicsController) {
			m_FPController.SyncTransformFromPhysics(*transform, physicsController, *fpCamera);
		}
	}

	void RuntimeLayer::UpdateCamera(float deltaTime)
	{
		if (m_PlayerEntity == entt::null) return;

		Scene* scene = m_ProjectManager->GetSceneManager()->GetOpenScene().get();
		if (!scene) return;

		auto* registry = scene->GetRegistry();

		auto* fpCamera = registry->try_get<FirstPersonCameraComponent>(m_PlayerEntity);
		auto* transform = registry->try_get<TransformComponent>(m_PlayerEntity);
		auto* cc = registry->try_get<CharacterControllerComponent>(m_PlayerEntity);

		if (!fpCamera || !transform) return;

		// Update camera look
		m_FPController.UpdateCamera(*fpCamera, *transform, deltaTime);

		// FOV changes based on movement state
		if (cc) {
			float targetFOV = fpCamera->baseFOV;

			switch (cc->state) {
				case MovementState::Sprinting:
					targetFOV = fpCamera->sprintFOV;
					break;
				case MovementState::Sliding:
					targetFOV = fpCamera->slideFOV;
					break;
				case MovementState::WallRunningLeft:
				case MovementState::WallRunningRight:
					targetFOV = fpCamera->sprintFOV;
					break;
				default:
					targetFOV = fpCamera->baseFOV;
					break;
			}

			// Lerp FOV
			fpCamera->currentFOV = fpCamera->currentFOV +
				(targetFOV - fpCamera->currentFOV) * fpCamera->fovLerpSpeed * deltaTime;

			// Update CameraComponent FOV if present
			auto* camera = registry->try_get<CameraComponent>(m_PlayerEntity);
			if (camera) {
				camera->fov = fpCamera->currentFOV;
			}
		}

		// Head bob
		if (fpCamera->enableHeadBob && cc && cc->isGrounded) {
			float speed = glm::length(glm::vec3(cc->velocity.x, 0, cc->velocity.z));
			if (speed > 1.0f) {
				float bobSpeed = fpCamera->bobFrequency * (speed / cc->runSpeed);
				fpCamera->bobPhase += bobSpeed * deltaTime;

				// Apply subtle vertical bob through eye offset modification
				// (actual implementation would modify camera position slightly)
			}
		}
	}

	void RuntimeLayer::UpdateFlow(float deltaTime)
	{
		if (m_PlayerEntity == entt::null) return;

		Scene* scene = m_ProjectManager->GetSceneManager()->GetOpenScene().get();
		if (!scene) return;

		auto* registry = scene->GetRegistry();

		auto* flow = registry->try_get<FlowStateComponent>(m_PlayerEntity);
		auto* cc = registry->try_get<CharacterControllerComponent>(m_PlayerEntity);

		if (!flow || !cc) return;

		FlowSystem::Update(*flow, *cc, deltaTime);
	}
}
