#pragma once

#include "Physics/PhysicsTypes.h"
#include "Physics/CollisionLayers.h"
#include "Physics/ContactListener.h"
#include "Physics/CharacterController.h"
#include "Project/Scene/Components.h"
#include "Project/Scene/Scene.h"
#include "Project/Assets/AssetManager.h"
#include "Core/Time.h"

namespace X3
{
	class PhysicsWorld
	{
	public:
		PhysicsWorld();
		~PhysicsWorld();

		// Initialize the physics system
		void Initialize();

		// Shutdown and cleanup
		void Shutdown();

		// Step the physics simulation (uses fixed timestep internally)
		void Step(float deltaTime);

		// Rebuild physics world from scene (call when entering play mode)
		void RebuildFromScene(Scene* scene, const AssetPool* assetPool);

		// Clear all physics bodies
		void ClearWorld();

		// ========================================================================
		// Body Management
		// ========================================================================

		// Create a physics body from entity components
		JPH::BodyID CreateBody(
			entt::entity entity,
			const RigidBodyComponent& rigidBody,
			const ColliderComponent& collider,
			const glm::mat4& transform,
			const AssetPool* assetPool = nullptr
		);

		// Destroy a physics body
		void DestroyBody(entt::entity entity);

		// Check if entity has a physics body
		bool HasBody(entt::entity entity) const;

		// ========================================================================
		// Character Controllers
		// ========================================================================

		// Create a character controller
		PhysicsCharacterController* CreateCharacterController(
			entt::entity entity,
			const CharacterControllerComponent& config,
			const glm::vec3& position,
			const glm::quat& rotation
		);

		// Destroy a character controller
		void DestroyCharacterController(entt::entity entity);

		// Get a character controller
		PhysicsCharacterController* GetCharacterController(entt::entity entity);

		// ========================================================================
		// Constraints / Joints
		// ========================================================================

		// Create a constraint between two entities (or entity and world if connectedEntity is null)
		void CreateConstraint(
			entt::entity entity,
			const ConstraintComponent& constraint,
			Scene* scene
		);

		// Destroy a constraint
		void DestroyConstraint(entt::entity entity);

		// Check if entity has a constraint
		bool HasConstraint(entt::entity entity) const;

		// ========================================================================
		// Transform Sync
		// ========================================================================

		// Sync kinematic body transforms from ECS to physics (before step)
		void SyncTransformsToPhysics(Scene* scene);

		// Sync dynamic body transforms from physics to ECS (after step)
		void SyncTransformsFromPhysics(Scene* scene);

		// ========================================================================
		// Forces and Impulses
		// ========================================================================

		void AddForce(entt::entity entity, const glm::vec3& force);
		void AddImpulse(entt::entity entity, const glm::vec3& impulse);
		void AddTorque(entt::entity entity, const glm::vec3& torque);
		void AddAngularImpulse(entt::entity entity, const glm::vec3& impulse);

		void SetLinearVelocity(entt::entity entity, const glm::vec3& velocity);
		void SetAngularVelocity(entt::entity entity, const glm::vec3& velocity);

		glm::vec3 GetLinearVelocity(entt::entity entity) const;
		glm::vec3 GetAngularVelocity(entt::entity entity) const;

		// ========================================================================
		// Queries
		// ========================================================================

		// Raycast against physics world
		RaycastHit Raycast(
			const glm::vec3& origin,
			const glm::vec3& direction,
			float maxDistance,
			uint16_t layerMask = 0xFFFF
		) const;

		// Sphere overlap test
		std::vector<entt::entity> OverlapSphere(
			const glm::vec3& center,
			float radius,
			uint16_t layerMask = 0xFFFF
		) const;

		// Box overlap test
		std::vector<entt::entity> OverlapBox(
			const glm::vec3& center,
			const glm::vec3& halfExtents,
			const glm::quat& rotation,
			uint16_t layerMask = 0xFFFF
		) const;

		// ========================================================================
		// Entity <-> Body Mapping
		// ========================================================================

		entt::entity GetEntityFromBodyID(JPH::BodyID bodyId) const;
		JPH::BodyID GetBodyIDFromEntity(entt::entity entity) const;

		// ========================================================================
		// Contact Events
		// ========================================================================

		const std::vector<ContactInfo>& GetContactsAdded() const;
		const std::vector<ContactInfo>& GetContactsPersisted() const;
		const std::vector<std::pair<entt::entity, entt::entity>>& GetContactsRemoved() const;

		// ========================================================================
		// Configuration
		// ========================================================================

		void SetGravity(const glm::vec3& gravity);
		glm::vec3 GetGravity() const;

		// ========================================================================
		// Internal Access (for character controller etc.)
		// ========================================================================

		JPH::PhysicsSystem* GetPhysicsSystem() { return m_PhysicsSystem.get(); }
		JPH::TempAllocator* GetTempAllocator() { return m_TempAllocator.get(); }
		const ObjectVsBroadPhaseLayerFilter& GetObjectVsBroadPhaseLayerFilter() const { return m_ObjectVsBroadPhaseLayerFilter; }
		const ObjectLayerPairFilter& GetObjectLayerPairFilter() const { return m_ObjectLayerPairFilter; }

	private:
		// Create shape from collider component (scale is applied to shape dimensions)
		JPH::Ref<JPH::Shape> CreateShape(const ColliderComponent& collider, const glm::vec3& scale, const AssetPool* assetPool);

		// Get object layer for body type
		JPH::ObjectLayer GetObjectLayer(BodyType type, bool isTrigger) const;

		// Get motion type from body type
		JPH::EMotionType GetMotionType(BodyType type) const;

	private:
		// Jolt systems
		std::unique_ptr<JPH::TempAllocatorImpl> m_TempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool> m_JobSystem;
		std::unique_ptr<JPH::PhysicsSystem> m_PhysicsSystem;

		// Layer filters
		BroadPhaseLayerInterface m_BroadPhaseLayerInterface;
		ObjectVsBroadPhaseLayerFilter m_ObjectVsBroadPhaseLayerFilter;
		ObjectLayerPairFilter m_ObjectLayerPairFilter;

		// Listeners
		std::unique_ptr<PhysicsContactListener> m_ContactListener;
		std::unique_ptr<BodyActivationListener> m_BodyActivationListener;

		// Entity <-> Body mapping
		std::unordered_map<entt::entity, JPH::BodyID> m_EntityToBody;
		std::unordered_map<JPH::BodyID, entt::entity> m_BodyToEntity;

		// Character controllers
		std::unordered_map<entt::entity, std::unique_ptr<PhysicsCharacterController>> m_CharacterControllers;

		// Constraints (stored by the entity that owns the ConstraintComponent)
		std::unordered_map<entt::entity, JPH::Ref<JPH::Constraint>> m_Constraints;

		// Cached mesh collision shapes (by mesh GUID to avoid rebuilding)
		std::unordered_map<LR_GUID, JPH::Ref<JPH::Shape>> m_CachedMeshShapes;

		// Fixed timestep accumulator
		float m_TimeAccumulator = 0.0f;

		// World settings
		glm::vec3 m_Gravity = glm::vec3(0.0f, -9.81f, 0.0f);

		// Constants
		static constexpr uint32_t MAX_BODIES = 65536;
		static constexpr uint32_t MAX_BODY_PAIRS = 65536;
		static constexpr uint32_t MAX_CONTACT_CONSTRAINTS = 10240;
	};
}
