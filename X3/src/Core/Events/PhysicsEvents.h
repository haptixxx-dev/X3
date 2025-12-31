#pragma once

#include "Core/Events/IEvent.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>

namespace X3
{
	// Physics simulation state events
	struct PhysicsSimulationStartedEvent : public IEvent {
		EventType GetType() const override { return EventType::PHYSICS_SIMULATION_STARTED_EVENT; }
	};

	struct PhysicsSimulationStoppedEvent : public IEvent {
		EventType GetType() const override { return EventType::PHYSICS_SIMULATION_STOPPED_EVENT; }
	};

	// Collision events - fired when two entities with colliders touch
	struct PhysicsCollisionEnterEvent : public IEvent {
		PhysicsCollisionEnterEvent(
			entt::entity entityA,
			entt::entity entityB,
			glm::vec3 contactPoint,
			glm::vec3 contactNormal,
			float penetrationDepth)
			: entityA(entityA)
			, entityB(entityB)
			, contactPoint(contactPoint)
			, contactNormal(contactNormal)
			, penetrationDepth(penetrationDepth)
		{}

		EventType GetType() const override { return EventType::PHYSICS_COLLISION_ENTER_EVENT; }

		entt::entity entityA;
		entt::entity entityB;
		glm::vec3 contactPoint;
		glm::vec3 contactNormal;
		float penetrationDepth;
	};

	struct PhysicsCollisionExitEvent : public IEvent {
		PhysicsCollisionExitEvent(entt::entity entityA, entt::entity entityB)
			: entityA(entityA), entityB(entityB)
		{}

		EventType GetType() const override { return EventType::PHYSICS_COLLISION_EXIT_EVENT; }

		entt::entity entityA;
		entt::entity entityB;
	};

	// Trigger events - fired when an entity enters/exits a trigger volume
	struct PhysicsTriggerEnterEvent : public IEvent {
		PhysicsTriggerEnterEvent(
			entt::entity triggerEntity,
			entt::entity otherEntity,
			glm::vec3 contactPoint)
			: triggerEntity(triggerEntity)
			, otherEntity(otherEntity)
			, contactPoint(contactPoint)
		{}

		EventType GetType() const override { return EventType::PHYSICS_TRIGGER_ENTER_EVENT; }

		entt::entity triggerEntity;  // The trigger volume entity
		entt::entity otherEntity;    // The entity that entered
		glm::vec3 contactPoint;
	};

	struct PhysicsTriggerExitEvent : public IEvent {
		PhysicsTriggerExitEvent(entt::entity triggerEntity, entt::entity otherEntity)
			: triggerEntity(triggerEntity), otherEntity(otherEntity)
		{}

		EventType GetType() const override { return EventType::PHYSICS_TRIGGER_EXIT_EVENT; }

		entt::entity triggerEntity;
		entt::entity otherEntity;
	};
}
