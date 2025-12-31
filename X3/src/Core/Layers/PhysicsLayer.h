#pragma once

#include "Core/Layers/ILayer.h"
#include "Physics/PhysicsWorld.h"
#include "Project/ProjectManager.h"

namespace X3
{
	class PhysicsLayer : public ILayer
	{
	public:
		PhysicsLayer(std::shared_ptr<ProjectManager> projectManager);
		~PhysicsLayer() override = default;

		void onAttach() override;
		void onDetach() override;
		void onUpdate() override;
		void onEvent(std::shared_ptr<IEvent> event) override;

		// Start/stop physics simulation
		void StartSimulation();
		void StopSimulation();
		bool IsSimulating() const { return m_IsSimulating; }

		// Access physics world
		PhysicsWorld* GetPhysicsWorld() { return &m_PhysicsWorld; }

		// Rebuild physics from current scene (call when scene changes)
		void RebuildPhysicsWorld();

	private:
		std::shared_ptr<ProjectManager> m_ProjectManager;
		PhysicsWorld m_PhysicsWorld;
		bool m_IsSimulating = false;
	};
}
