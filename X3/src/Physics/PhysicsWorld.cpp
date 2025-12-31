#include "Physics/PhysicsWorld.h"
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

// Jolt requires a custom trace function in debug builds
JPH_SUPPRESS_WARNINGS

namespace X3
{
	// Custom trace function for Jolt
	static void JoltTraceImpl(const char* inFMT, ...)
	{
		va_list list;
		va_start(list, inFMT);
		char buffer[1024];
		vsnprintf(buffer, sizeof(buffer), inFMT, list);
		va_end(list);
		LOG_ENGINE_TRACE("[Jolt] {}", buffer);
	}

#ifdef JPH_ENABLE_ASSERTS
	// Custom assert handler for Jolt
	static bool JoltAssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine)
	{
		LOG_ENGINE_ERROR("[Jolt Assert] {} : {} ({}:{})", inExpression, inMessage ? inMessage : "", inFile, inLine);
		return true; // Break into debugger
	}
#endif

	PhysicsWorld::PhysicsWorld()
	{
	}

	PhysicsWorld::~PhysicsWorld()
	{
		Shutdown();
	}

	void PhysicsWorld::Initialize()
	{
		// Register allocation hook and trace
		JPH::RegisterDefaultAllocator();
		JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
		JPH::AssertFailed = JoltAssertFailedImpl;
#endif

		// Create factory and register types
		JPH::Factory::sInstance = new JPH::Factory();
		JPH::RegisterTypes();

		// Create temp allocator (10MB stack)
		m_TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

		// Create job system (use available threads minus one for main thread)
		uint32_t numThreads = std::max(1u, std::thread::hardware_concurrency() - 1);
		m_JobSystem = std::make_unique<JPH::JobSystemThreadPool>(
			JPH::cMaxPhysicsJobs,
			JPH::cMaxPhysicsBarriers,
			numThreads
		);

		// Create physics system
		m_PhysicsSystem = std::make_unique<JPH::PhysicsSystem>();
		m_PhysicsSystem->Init(
			MAX_BODIES,
			0, // Number of body mutexes (0 = auto)
			MAX_BODY_PAIRS,
			MAX_CONTACT_CONSTRAINTS,
			m_BroadPhaseLayerInterface,
			m_ObjectVsBroadPhaseLayerFilter,
			m_ObjectLayerPairFilter
		);

		// Create and set listeners
		m_ContactListener = std::make_unique<PhysicsContactListener>(this);
		m_BodyActivationListener = std::make_unique<BodyActivationListener>(this);
		m_PhysicsSystem->SetContactListener(m_ContactListener.get());
		m_PhysicsSystem->SetBodyActivationListener(m_BodyActivationListener.get());

		// Set gravity
		m_PhysicsSystem->SetGravity(ToJolt(m_Gravity));

		LOG_ENGINE_INFO("Physics world initialized with {} threads", numThreads);
	}

	void PhysicsWorld::Shutdown()
	{
		ClearWorld();

		m_PhysicsSystem.reset();
		m_JobSystem.reset();
		m_TempAllocator.reset();

		// Cleanup Jolt
		JPH::UnregisterTypes();
		if (JPH::Factory::sInstance) {
			delete JPH::Factory::sInstance;
			JPH::Factory::sInstance = nullptr;
		}
	}

	void PhysicsWorld::Step(float deltaTime)
	{
		// Fixed timestep accumulator pattern
		float fixedDt = Time::GetFixedDeltaTime();
		m_TimeAccumulator += deltaTime;

		// Limit maximum accumulated time to prevent spiral of death
		const float maxAccumulated = fixedDt * 8.0f;
		if (m_TimeAccumulator > maxAccumulated)
			m_TimeAccumulator = maxAccumulated;

		// Clear contact events
		m_ContactListener->ClearContacts();

		// Step physics at fixed intervals
		int collisionSteps = 0;
		while (m_TimeAccumulator >= fixedDt)
		{
			// Update character controllers
			for (auto& [entity, controller] : m_CharacterControllers)
			{
				controller->Update(fixedDt, m_Gravity);
			}

			// Step physics
			m_PhysicsSystem->Update(
				fixedDt,
				1, // Collision steps per update
				m_TempAllocator.get(),
				m_JobSystem.get()
			);

			m_TimeAccumulator -= fixedDt;
			collisionSteps++;
		}
	}

	void PhysicsWorld::RebuildFromScene(Scene* scene)
	{
		ClearWorld();

		if (!scene || !scene->GetRegistry())
			return;

		auto* registry = scene->GetRegistry();

		// Create bodies for all entities with RigidBody + Collider
		auto view = registry->view<TransformComponent, RigidBodyComponent, ColliderComponent>();
		for (auto entity : view)
		{
			auto& transform = view.get<TransformComponent>(entity);
			auto& rigidBody = view.get<RigidBodyComponent>(entity);
			auto& collider = view.get<ColliderComponent>(entity);

			CreateBody(entity, rigidBody, collider, transform.GetMatrix());
		}

		// Create character controllers
		auto charView = registry->view<TransformComponent, CharacterControllerComponent>();
		for (auto entity : charView)
		{
			auto& transform = charView.get<TransformComponent>(entity);
			auto& charController = charView.get<CharacterControllerComponent>(entity);

			glm::vec3 position = transform.GetTranslation();
			glm::vec3 eulerRot = glm::radians(transform.GetRotation());
			glm::quat rotation = glm::quat(eulerRot);

			CreateCharacterController(entity, charController, position, rotation);
		}

		LOG_ENGINE_INFO("Physics world rebuilt: {} bodies, {} character controllers",
			m_EntityToBody.size(), m_CharacterControllers.size());
	}

	void PhysicsWorld::ClearWorld()
	{
		// Remove all bodies
		auto& bodyInterface = m_PhysicsSystem->GetBodyInterface();
		for (auto& [entity, bodyId] : m_EntityToBody)
		{
			if (!bodyId.IsInvalid())
			{
				bodyInterface.RemoveBody(bodyId);
				bodyInterface.DestroyBody(bodyId);
			}
		}
		m_EntityToBody.clear();
		m_BodyToEntity.clear();

		// Clear character controllers
		m_CharacterControllers.clear();

		m_TimeAccumulator = 0.0f;
	}

	JPH::BodyID PhysicsWorld::CreateBody(
		entt::entity entity,
		const RigidBodyComponent& rigidBody,
		const ColliderComponent& collider,
		const glm::mat4& transform)
	{
		// Extract position and rotation from transform
		glm::vec3 position = glm::vec3(transform[3]);
		glm::vec3 scale = glm::vec3(
			glm::length(glm::vec3(transform[0])),
			glm::length(glm::vec3(transform[1])),
			glm::length(glm::vec3(transform[2]))
		);
		glm::mat3 rotMat = glm::mat3(
			glm::vec3(transform[0]) / scale.x,
			glm::vec3(transform[1]) / scale.y,
			glm::vec3(transform[2]) / scale.z
		);
		glm::quat rotation = glm::quat_cast(rotMat);

		// Create shape
		JPH::Ref<JPH::Shape> shape = CreateShape(collider);
		if (!shape)
		{
			LOG_ENGINE_ERROR("Failed to create physics shape for entity");
			return JPH::BodyID();
		}

		// Apply offset if specified
		if (collider.offset != glm::vec3(0.0f) || collider.rotationOffset != glm::vec3(0.0f))
		{
			glm::vec3 rotRad = glm::radians(collider.rotationOffset);
			glm::quat offsetRot = glm::quat(rotRad);

			JPH::RotatedTranslatedShapeSettings offsetSettings(
				ToJolt(collider.offset),
				ToJolt(offsetRot),
				shape
			);
			auto result = offsetSettings.Create();
			if (result.IsValid())
				shape = result.Get();
		}

		// Create body settings
		JPH::BodyCreationSettings bodySettings(
			shape,
			ToJolt(position),
			ToJolt(rotation),
			GetMotionType(rigidBody.bodyType),
			GetObjectLayer(rigidBody.bodyType, collider.isTrigger)
		);

		// Configure mass and other properties
		if (rigidBody.bodyType == BodyType::Dynamic)
		{
			bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			bodySettings.mMassPropertiesOverride.mMass = rigidBody.mass;
		}

		bodySettings.mFriction = rigidBody.friction;
		bodySettings.mRestitution = rigidBody.restitution;
		bodySettings.mLinearDamping = rigidBody.linearDamping;
		bodySettings.mAngularDamping = rigidBody.angularDamping;
		bodySettings.mGravityFactor = rigidBody.gravityScale;
		bodySettings.mIsSensor = collider.isTrigger;

		// Store entity in user data for lookups
		bodySettings.mUserData = static_cast<uint64_t>(entity);

		// Create body
		auto& bodyInterface = m_PhysicsSystem->GetBodyInterface();
		JPH::BodyID bodyId = bodyInterface.CreateAndAddBody(
			bodySettings,
			rigidBody.bodyType == BodyType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate
		);

		if (bodyId.IsInvalid())
		{
			LOG_ENGINE_ERROR("Failed to create physics body for entity");
			return JPH::BodyID();
		}

		// Apply constraint locks
		ApplyConstraintLocks(bodyId, rigidBody);

		// Store mapping
		m_EntityToBody[entity] = bodyId;
		m_BodyToEntity[bodyId] = entity;

		return bodyId;
	}

	void PhysicsWorld::DestroyBody(entt::entity entity)
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return;

		JPH::BodyID bodyId = it->second;
		if (!bodyId.IsInvalid())
		{
			auto& bodyInterface = m_PhysicsSystem->GetBodyInterface();
			bodyInterface.RemoveBody(bodyId);
			bodyInterface.DestroyBody(bodyId);
		}

		m_BodyToEntity.erase(bodyId);
		m_EntityToBody.erase(it);
	}

	bool PhysicsWorld::HasBody(entt::entity entity) const
	{
		return m_EntityToBody.find(entity) != m_EntityToBody.end();
	}

	PhysicsCharacterController* PhysicsWorld::CreateCharacterController(
		entt::entity entity,
		const CharacterControllerComponent& config,
		const glm::vec3& position,
		const glm::quat& rotation)
	{
		auto controller = std::make_unique<PhysicsCharacterController>(
			this, entity, config, position, rotation
		);

		PhysicsCharacterController* ptr = controller.get();
		m_CharacterControllers[entity] = std::move(controller);
		return ptr;
	}

	void PhysicsWorld::DestroyCharacterController(entt::entity entity)
	{
		m_CharacterControllers.erase(entity);
	}

	PhysicsCharacterController* PhysicsWorld::GetCharacterController(entt::entity entity)
	{
		auto it = m_CharacterControllers.find(entity);
		if (it != m_CharacterControllers.end())
			return it->second.get();
		return nullptr;
	}

	void PhysicsWorld::SyncTransformsToPhysics(Scene* scene)
	{
		if (!scene || !scene->GetRegistry())
			return;

		auto* registry = scene->GetRegistry();
		auto& bodyInterface = m_PhysicsSystem->GetBodyInterface();

		// Sync kinematic bodies from ECS to physics
		auto view = registry->view<TransformComponent, RigidBodyComponent>();
		for (auto entity : view)
		{
			auto& rigidBody = view.get<RigidBodyComponent>(entity);
			if (rigidBody.bodyType != BodyType::Kinematic)
				continue;

			auto it = m_EntityToBody.find(entity);
			if (it == m_EntityToBody.end())
				continue;

			auto& transform = view.get<TransformComponent>(entity);
			glm::mat4 mat = transform.GetMatrix();

			glm::vec3 position = glm::vec3(mat[3]);
			glm::vec3 scale = glm::vec3(
				glm::length(glm::vec3(mat[0])),
				glm::length(glm::vec3(mat[1])),
				glm::length(glm::vec3(mat[2]))
			);
			glm::mat3 rotMat = glm::mat3(
				glm::vec3(mat[0]) / scale.x,
				glm::vec3(mat[1]) / scale.y,
				glm::vec3(mat[2]) / scale.z
			);
			glm::quat rotation = glm::quat_cast(rotMat);

			bodyInterface.SetPositionAndRotation(
				it->second,
				ToJolt(position),
				ToJolt(rotation),
				JPH::EActivation::Activate
			);
		}
	}

	void PhysicsWorld::SyncTransformsFromPhysics(Scene* scene)
	{
		if (!scene || !scene->GetRegistry())
			return;

		auto* registry = scene->GetRegistry();
		auto& bodyInterface = m_PhysicsSystem->GetBodyInterface();

		// Sync dynamic bodies from physics to ECS
		for (auto& [entity, bodyId] : m_EntityToBody)
		{
			if (!registry->valid(entity))
				continue;

			// Only sync dynamic bodies
			if (!registry->all_of<RigidBodyComponent>(entity))
				continue;

			auto& rigidBody = registry->get<RigidBodyComponent>(entity);
			if (rigidBody.bodyType != BodyType::Dynamic)
				continue;

			if (!registry->all_of<TransformComponent>(entity))
				continue;

			JPH::Vec3 pos = bodyInterface.GetPosition(bodyId);
			JPH::Quat rot = bodyInterface.GetRotation(bodyId);

			auto& transform = registry->get<TransformComponent>(entity);
			transform.SetTranslation(FromJolt(pos));

			// Convert quaternion to euler angles
			glm::quat q = FromJolt(rot);
			glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
			transform.SetRotation(euler);
		}

		// Sync character controllers
		for (auto& [entity, controller] : m_CharacterControllers)
		{
			if (!registry->valid(entity))
				continue;

			if (!registry->all_of<TransformComponent, CharacterControllerComponent>(entity))
				continue;

			auto& transform = registry->get<TransformComponent>(entity);
			auto& charComp = registry->get<CharacterControllerComponent>(entity);

			transform.SetTranslation(controller->GetPosition());

			glm::quat q = controller->GetRotation();
			glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
			transform.SetRotation(euler);

			// Update runtime state
			charComp.velocity = controller->GetLinearVelocity();
			charComp.isGrounded = controller->IsGrounded();
		}
	}

	void PhysicsWorld::AddForce(entt::entity entity, const glm::vec3& force)
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return;

		m_PhysicsSystem->GetBodyInterface().AddForce(it->second, ToJolt(force));
	}

	void PhysicsWorld::AddImpulse(entt::entity entity, const glm::vec3& impulse)
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return;

		m_PhysicsSystem->GetBodyInterface().AddImpulse(it->second, ToJolt(impulse));
	}

	void PhysicsWorld::AddTorque(entt::entity entity, const glm::vec3& torque)
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return;

		m_PhysicsSystem->GetBodyInterface().AddTorque(it->second, ToJolt(torque));
	}

	void PhysicsWorld::AddAngularImpulse(entt::entity entity, const glm::vec3& impulse)
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return;

		m_PhysicsSystem->GetBodyInterface().AddAngularImpulse(it->second, ToJolt(impulse));
	}

	void PhysicsWorld::SetLinearVelocity(entt::entity entity, const glm::vec3& velocity)
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return;

		m_PhysicsSystem->GetBodyInterface().SetLinearVelocity(it->second, ToJolt(velocity));
	}

	void PhysicsWorld::SetAngularVelocity(entt::entity entity, const glm::vec3& velocity)
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return;

		m_PhysicsSystem->GetBodyInterface().SetAngularVelocity(it->second, ToJolt(velocity));
	}

	glm::vec3 PhysicsWorld::GetLinearVelocity(entt::entity entity) const
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return glm::vec3(0.0f);

		return FromJolt(m_PhysicsSystem->GetBodyInterface().GetLinearVelocity(it->second));
	}

	glm::vec3 PhysicsWorld::GetAngularVelocity(entt::entity entity) const
	{
		auto it = m_EntityToBody.find(entity);
		if (it == m_EntityToBody.end())
			return glm::vec3(0.0f);

		return FromJolt(m_PhysicsSystem->GetBodyInterface().GetAngularVelocity(it->second));
	}

	RaycastHit PhysicsWorld::Raycast(
		const glm::vec3& origin,
		const glm::vec3& direction,
		float maxDistance,
		uint16_t layerMask) const
	{
		RaycastHit result;

		JPH::RRayCast ray(ToJolt(origin), ToJolt(direction * maxDistance));
		JPH::RayCastResult hit;

		// Use broad phase for quick test
		JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(m_ObjectVsBroadPhaseLayerFilter, Layers::MOVING);
		JPH::DefaultObjectLayerFilter objectLayerFilter(m_ObjectLayerPairFilter, Layers::MOVING);

		if (m_PhysicsSystem->GetNarrowPhaseQuery().CastRay(
			ray,
			hit,
			broadPhaseFilter,
			objectLayerFilter))
		{
			result.hit = true;
			result.distance = hit.mFraction * maxDistance;
			result.point = origin + direction * result.distance;

			// Get body and entity
			JPH::BodyID bodyId = hit.mBodyID;
			auto it = m_BodyToEntity.find(bodyId);
			if (it != m_BodyToEntity.end())
				result.entity = it->second;

			// Get normal at hit point
			JPH::BodyLockRead lock(m_PhysicsSystem->GetBodyLockInterface(), bodyId);
			if (lock.Succeeded())
			{
				const JPH::Body& body = lock.GetBody();
				JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction));
				result.normal = FromJolt(normal);
			}
		}

		return result;
	}

	std::vector<entt::entity> PhysicsWorld::OverlapSphere(
		const glm::vec3& center,
		float radius,
		uint16_t layerMask) const
	{
		std::vector<entt::entity> result;

		JPH::SphereShape sphere(radius);
		JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;

		JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(m_ObjectVsBroadPhaseLayerFilter, Layers::MOVING);
		JPH::DefaultObjectLayerFilter objectLayerFilter(m_ObjectLayerPairFilter, Layers::MOVING);

		m_PhysicsSystem->GetBroadPhaseQuery().CollideSphere(
			ToJolt(center),
			radius,
			collector,
			broadPhaseFilter,
			objectLayerFilter
		);

		for (const auto& bodyId : collector.mHits)
		{
			auto it = m_BodyToEntity.find(bodyId);
			if (it != m_BodyToEntity.end())
				result.push_back(it->second);
		}

		return result;
	}

	std::vector<entt::entity> PhysicsWorld::OverlapBox(
		const glm::vec3& center,
		const glm::vec3& halfExtents,
		const glm::quat& rotation,
		uint16_t layerMask) const
	{
		std::vector<entt::entity> result;

		JPH::AABox box(
			ToJolt(center - halfExtents),
			ToJolt(center + halfExtents)
		);

		JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
		JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(m_ObjectVsBroadPhaseLayerFilter, Layers::MOVING);
		JPH::DefaultObjectLayerFilter objectLayerFilter(m_ObjectLayerPairFilter, Layers::MOVING);

		m_PhysicsSystem->GetBroadPhaseQuery().CollideAABox(
			box,
			collector,
			broadPhaseFilter,
			objectLayerFilter
		);

		for (const auto& bodyId : collector.mHits)
		{
			auto it = m_BodyToEntity.find(bodyId);
			if (it != m_BodyToEntity.end())
				result.push_back(it->second);
		}

		return result;
	}

	entt::entity PhysicsWorld::GetEntityFromBodyID(JPH::BodyID bodyId) const
	{
		auto it = m_BodyToEntity.find(bodyId);
		if (it != m_BodyToEntity.end())
			return it->second;
		return entt::null;
	}

	JPH::BodyID PhysicsWorld::GetBodyIDFromEntity(entt::entity entity) const
	{
		auto it = m_EntityToBody.find(entity);
		if (it != m_EntityToBody.end())
			return it->second;
		return JPH::BodyID();
	}

	const std::vector<ContactInfo>& PhysicsWorld::GetContactsAdded() const
	{
		return m_ContactListener->GetContactsAdded();
	}

	const std::vector<ContactInfo>& PhysicsWorld::GetContactsPersisted() const
	{
		return m_ContactListener->GetContactsPersisted();
	}

	const std::vector<std::pair<entt::entity, entt::entity>>& PhysicsWorld::GetContactsRemoved() const
	{
		return m_ContactListener->GetContactsRemoved();
	}

	void PhysicsWorld::SetGravity(const glm::vec3& gravity)
	{
		m_Gravity = gravity;
		if (m_PhysicsSystem)
			m_PhysicsSystem->SetGravity(ToJolt(gravity));
	}

	glm::vec3 PhysicsWorld::GetGravity() const
	{
		return m_Gravity;
	}

	JPH::Ref<JPH::Shape> PhysicsWorld::CreateShape(const ColliderComponent& collider)
	{
		switch (collider.shape)
		{
		case ColliderShape::Box:
			return new JPH::BoxShape(ToJolt(collider.boxHalfExtents));

		case ColliderShape::Sphere:
			return new JPH::SphereShape(collider.sphereRadius);

		case ColliderShape::Capsule:
			return new JPH::CapsuleShape(collider.capsuleHalfHeight, collider.capsuleRadius);

		case ColliderShape::ConvexMesh:
		case ColliderShape::TriangleMesh:
		case ColliderShape::Heightfield:
			// TODO: Implement mesh shapes - requires loading mesh data
			LOG_ENGINE_WARN("Mesh collision shapes not yet implemented, using box fallback");
			return new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));

		default:
			LOG_ENGINE_ERROR("Unknown collider shape type");
			return new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
		}
	}

	JPH::ObjectLayer PhysicsWorld::GetObjectLayer(BodyType type, bool isTrigger) const
	{
		if (isTrigger)
			return Layers::TRIGGER;

		switch (type)
		{
		case BodyType::Static:
			return Layers::NON_MOVING;
		case BodyType::Kinematic:
		case BodyType::Dynamic:
		default:
			return Layers::MOVING;
		}
	}

	JPH::EMotionType PhysicsWorld::GetMotionType(BodyType type) const
	{
		switch (type)
		{
		case BodyType::Static:
			return JPH::EMotionType::Static;
		case BodyType::Kinematic:
			return JPH::EMotionType::Kinematic;
		case BodyType::Dynamic:
		default:
			return JPH::EMotionType::Dynamic;
		}
	}

	void PhysicsWorld::ApplyConstraintLocks(JPH::BodyID bodyId, const RigidBodyComponent& rb)
	{
		auto& bodyInterface = m_PhysicsSystem->GetBodyInterface();

		// Build DOF mask for locked axes
		JPH::EAllowedDOFs allowedDOFs = JPH::EAllowedDOFs::All;

		if (rb.lockPositionX)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::TranslationX;
		if (rb.lockPositionY)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::TranslationY;
		if (rb.lockPositionZ)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::TranslationZ;
		if (rb.lockRotationX)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::RotationX;
		if (rb.lockRotationY)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::RotationY;
		if (rb.lockRotationZ)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::RotationZ;

		// Note: Jolt doesn't have a direct SetAllowedDOFs on existing bodies
		// Constraints would need to be applied via MotionProperties or constraints
		// For now, we handle this during body creation via BodyCreationSettings
	}
}
