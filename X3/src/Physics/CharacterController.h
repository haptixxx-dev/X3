#pragma once

#include "Physics/PhysicsTypes.h"
#include "Project/Scene/Components.h"
#include <Jolt/Physics/Character/CharacterVirtual.h>

namespace X3
{
	class PhysicsWorld;

	// Wrapper around Jolt's CharacterVirtual for player/NPC control
	class PhysicsCharacterController
	{
	public:
		PhysicsCharacterController(
			PhysicsWorld* world,
			entt::entity entity,
			const CharacterControllerComponent& config,
			const glm::vec3& position,
			const glm::quat& rotation
		);

		~PhysicsCharacterController();

		// Update character physics (call each physics step)
		void Update(float deltaTime, const glm::vec3& gravity);

		// Movement input
		void SetLinearVelocity(const glm::vec3& velocity);
		void SetInputVelocity(const glm::vec3& desiredVelocity);
		glm::vec3 GetLinearVelocity() const;
		glm::vec3 GetPosition() const;
		glm::quat GetRotation() const;

		// Teleport (instant position change)
		void SetPosition(const glm::vec3& position);
		void SetRotation(const glm::quat& rotation);

		// State queries
		bool IsGrounded() const;
		bool IsOnSlope() const;
		glm::vec3 GetGroundNormal() const;
		float GetGroundDistance() const;

		// Jump
		void RequestJump() { m_WantsJump = true; }
		void SetJumpForce(float force) { m_JumpForce = force; }
		float GetJumpForce() const { return m_JumpForce; }

		// Configuration
		void SetMaxSlopeAngle(float degrees);
		void SetMass(float mass);

		// Get the underlying Jolt character
		JPH::CharacterVirtual* GetJoltCharacter() { return m_Character.get(); }

		entt::entity GetEntity() const { return m_Entity; }

		// Advanced movement support
		void SetCapsuleHeight(float height, float radius);
		bool CheckWall(const glm::vec3& direction, float distance, glm::vec3& outNormal) const;
		bool CanStand() const;  // Check if there's room to stand up

	private:
		PhysicsWorld* m_World;
		entt::entity m_Entity;
		std::unique_ptr<JPH::CharacterVirtual> m_Character;
		JPH::Ref<JPH::Shape> m_StandingShape;
		JPH::Ref<JPH::Shape> m_CrouchingShape;

		glm::vec3 m_InputVelocity = glm::vec3(0.0f);
		float m_JumpForce = 5.0f;
		float m_CapsuleRadius = 0.3f;
		float m_StandingHeight = 1.8f;
		float m_CrouchingHeight = 1.0f;
		bool m_WantsJump = false;
		bool m_IsCrouching = false;
	};
}
