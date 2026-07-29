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

	// World configuration. Mirrors UpdateRenderSettingsEvent: the editor owns the
	// value, the layer owns the system it applies to, and the event is the only
	// thing that crosses between them. PhysicsWorld::SetGravity is safe to receive
	// this at any time -- it stores the vector and only forwards to Jolt when a
	// JPH::PhysicsSystem exists, and PhysicsWorld::Initialize re-applies the stored
	// value -- so an edit made while stopped survives into the next Play.
	//
	// This is a live override, not the persisted value: the project file is the
	// source of truth (ProjectFile::physicsGravity) and PhysicsLayer::StartSimulation
	// reads it directly, so a project opened without ever touching this panel still
	// simulates with its saved gravity.
	struct SetPhysicsGravityEvent : public IEvent {
		glm::vec3 gravity;

		explicit SetPhysicsGravityEvent(const glm::vec3& gravity)
			: gravity(gravity) {}

		EventType GetType() const override { return EventType::SET_PHYSICS_GRAVITY_EVENT; }
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
