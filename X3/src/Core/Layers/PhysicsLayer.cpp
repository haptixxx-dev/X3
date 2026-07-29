#include "Core/Layers/PhysicsLayer.h"
#include "Core/Time.h"
#include "Project/Scene/SceneManager.h"
#include "Project/Assets/AssetManager.h"
#include "Core/Events/PhysicsEvents.h"

namespace X3
{
	PhysicsLayer::PhysicsLayer(
		std::shared_ptr<ProjectManager> projectManager,
		std::shared_ptr<IEventDispatcher> eventDispatcher)
		: m_ProjectManager(projectManager)
		, m_EventDispatcher(eventDispatcher)
	{
	}

	void PhysicsLayer::onAttach()
	{
		// Don't initialize physics here - defer until simulation starts
		// This prevents Jolt's thread pool from running when not simulating
		LOG_ENGINE_INFO("Physics layer attached");
	}

	void PhysicsLayer::onDetach()
	{
		if (m_IsInitialized)
		{
			m_PhysicsWorld.Shutdown();
			m_IsInitialized = false;
		}
		LOG_ENGINE_INFO("Physics layer detached");
	}

	void PhysicsLayer::onUpdate()
	{
		if (!m_IsSimulating)
			return;

		auto sceneManager = m_ProjectManager->GetSceneManager();
		if (!sceneManager)
			return;

		auto scene = sceneManager->GetOpenScene();
		if (!scene)
			return;

		// Sync kinematic bodies to physics
		m_PhysicsWorld.SyncTransformsToPhysics(scene.get());

		// Step physics simulation
		float deltaTime = Time::GetDeltaTime();
		m_PhysicsWorld.Step(deltaTime);

		// Dispatch collision/trigger events
		DispatchPhysicsEvents();

		// Sync dynamic bodies from physics to ECS
		m_PhysicsWorld.SyncTransformsFromPhysics(scene.get());
	}

	void PhysicsLayer::onEvent(std::shared_ptr<IEvent> event)
	{
		switch (event->GetType())
		{
		case EventType::PHYSICS_SIMULATION_STARTED_EVENT:
			StartSimulation();
			break;
		case EventType::PHYSICS_SIMULATION_STOPPED_EVENT:
			StopSimulation();
			break;
		case EventType::SET_PHYSICS_GRAVITY_EVENT:
			// The only route the editor has into the live world -- m_PhysicsWorld is a
			// by-value member and nothing outside the layer stack holds this layer.
			// Applied unconditionally, not just while simulating: SetGravity stores the
			// vector and Initialize() re-applies it, so an edit made while stopped is
			// still in effect at the next Play. Not consumed -- other layers may want
			// to react (debug overlays, future character-controller tuning).
			m_PhysicsWorld.SetGravity(std::dynamic_pointer_cast<SetPhysicsGravityEvent>(event)->gravity);
			break;
		default:
			break;
		}
	}

	void PhysicsLayer::StartSimulation()
	{
		if (m_IsSimulating)
			return;

		// Take the project's gravity BEFORE Initialize(), which is what pushes
		// m_Gravity into the freshly created JPH::PhysicsSystem. This is what makes
		// the setting work without the editor: StopSimulation() destroys the world
		// entirely, the runtime never opens the settings panel, and a project can be
		// opened after the panel last dispatched -- so the project file, not the last
		// event, has to be the source of truth at every Play.
		if (m_ProjectManager && m_ProjectManager->ProjectIsOpen())
			m_PhysicsWorld.SetGravity(m_ProjectManager->GetPhysicsGravity());

		// Initialize physics on first simulation start
		if (!m_IsInitialized)
		{
			m_PhysicsWorld.Initialize();
			m_IsInitialized = true;
		}

		RebuildPhysicsWorld();
		m_IsSimulating = true;
		LOG_ENGINE_INFO("Physics simulation started");
	}

	void PhysicsLayer::StopSimulation()
	{
		if (!m_IsSimulating)
			return;

		m_IsSimulating = false;
		m_PhysicsWorld.ClearWorld();

		// Shutdown physics to free thread pool resources
		if (m_IsInitialized)
		{
			m_PhysicsWorld.Shutdown();
			m_IsInitialized = false;
		}

		LOG_ENGINE_INFO("Physics simulation stopped");
	}

	void PhysicsLayer::RebuildPhysicsWorld()
	{
		auto sceneManager = m_ProjectManager->GetSceneManager();
		if (!sceneManager)
			return;

		auto scene = sceneManager->GetOpenScene();
		if (scene)
		{
			// Get asset pool for mesh collision shapes
			const AssetPool* assetPool = nullptr;
			auto assetManager = m_ProjectManager->GetAssetManager();
			if (assetManager)
			{
				assetPool = assetManager->GetAssetPool().get();
			}

			m_PhysicsWorld.RebuildFromScene(scene.get(), assetPool);
		}
	}

	void PhysicsLayer::DispatchPhysicsEvents()
	{
		if (!m_EventDispatcher)
			return;

		auto sceneManager = m_ProjectManager->GetSceneManager();
		if (!sceneManager)
			return;

		auto scene = sceneManager->GetOpenScene();
		if (!scene)
			return;

		auto* registry = scene->GetRegistry();
		if (!registry)
			return;

		// Helper to check if an entity is a trigger
		auto isTrigger = [&](entt::entity entity) -> bool {
			if (registry->valid(entity) && registry->all_of<ColliderComponent>(entity))
			{
				return registry->get<ColliderComponent>(entity).isTrigger;
			}
			return false;
		};

		// Dispatch collision/trigger enter events
		for (const auto& contact : m_PhysicsWorld.GetContactsAdded())
		{
			bool triggerA = isTrigger(contact.entityA);
			bool triggerB = isTrigger(contact.entityB);

			if (triggerA || triggerB)
			{
				// At least one is a trigger - dispatch trigger events
				if (triggerA)
				{
					m_EventDispatcher->dispatchEvent(std::make_shared<PhysicsTriggerEnterEvent>(
						contact.entityA, contact.entityB, contact.contactPoint
					));
				}
				if (triggerB)
				{
					m_EventDispatcher->dispatchEvent(std::make_shared<PhysicsTriggerEnterEvent>(
						contact.entityB, contact.entityA, contact.contactPoint
					));
				}
			}
			else
			{
				// Neither is a trigger - dispatch collision event
				m_EventDispatcher->dispatchEvent(std::make_shared<PhysicsCollisionEnterEvent>(
					contact.entityA,
					contact.entityB,
					contact.contactPoint,
					contact.contactNormal,
					contact.penetrationDepth
				));
			}
		}

		// Dispatch collision/trigger exit events
		for (const auto& [entityA, entityB] : m_PhysicsWorld.GetContactsRemoved())
		{
			bool triggerA = isTrigger(entityA);
			bool triggerB = isTrigger(entityB);

			if (triggerA || triggerB)
			{
				// At least one is a trigger - dispatch trigger exit events
				if (triggerA)
				{
					m_EventDispatcher->dispatchEvent(std::make_shared<PhysicsTriggerExitEvent>(
						entityA, entityB
					));
				}
				if (triggerB)
				{
					m_EventDispatcher->dispatchEvent(std::make_shared<PhysicsTriggerExitEvent>(
						entityB, entityA
					));
				}
			}
			else
			{
				// Neither is a trigger - dispatch collision exit event
				m_EventDispatcher->dispatchEvent(std::make_shared<PhysicsCollisionExitEvent>(
					entityA, entityB
				));
			}
		}
	}
}
