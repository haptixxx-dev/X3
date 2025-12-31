#include "Core/Layers/PhysicsLayer.h"
#include "Core/Time.h"
#include "Project/Scene/SceneManager.h"
#include "Core/Events/PhysicsEvents.h"

namespace X3
{
	PhysicsLayer::PhysicsLayer(std::shared_ptr<ProjectManager> projectManager)
		: m_ProjectManager(projectManager)
	{
	}

	void PhysicsLayer::onAttach()
	{
		m_PhysicsWorld.Initialize();
		LOG_ENGINE_INFO("Physics layer attached");
	}

	void PhysicsLayer::onDetach()
	{
		m_PhysicsWorld.Shutdown();
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
		default:
			break;
		}
	}

	void PhysicsLayer::StartSimulation()
	{
		if (m_IsSimulating)
			return;

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
			m_PhysicsWorld.RebuildFromScene(scene.get());
		}
	}
}
