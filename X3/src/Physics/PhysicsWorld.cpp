#include "Physics/PhysicsWorld.h"
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
// Constraint includes
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

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

		// Configure physics settings for better collision resolution
		JPH::PhysicsSettings settings;
		settings.mNumVelocitySteps = 10;          // More velocity solver iterations (default 10)
		settings.mNumPositionSteps = 2;           // More position solver iterations (default 2)
		settings.mPenetrationSlop = 0.01f;        // Reduce allowed penetration (default 0.02)
		settings.mSpeculativeContactDistance = 0.02f;  // Smaller speculative contact distance
		m_PhysicsSystem->SetPhysicsSettings(settings);

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

			// Step physics with multiple collision sub-steps for better accuracy
			m_PhysicsSystem->Update(
				fixedDt,
				2, // Collision steps per update (more = better edge collision handling)
				m_TempAllocator.get(),
				m_JobSystem.get()
			);

			m_TimeAccumulator -= fixedDt;
			collisionSteps++;
		}
	}

	void PhysicsWorld::RebuildFromScene(Scene* scene, const AssetPool* assetPool)
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

			CreateBody(entity, rigidBody, collider, transform.GetMatrix(), assetPool);
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

		// Create constraints (after all bodies are created)
		auto constraintView = registry->view<ConstraintComponent>();
		for (auto entity : constraintView)
		{
			auto& constraint = constraintView.get<ConstraintComponent>(entity);
			CreateConstraint(entity, constraint, scene);
		}

		LOG_ENGINE_INFO("Physics world rebuilt: {} bodies, {} character controllers, {} constraints",
			m_EntityToBody.size(), m_CharacterControllers.size(), m_Constraints.size());
	}

	void PhysicsWorld::ClearWorld()
	{
		// Remove all constraints first (before bodies)
		for (auto& [entity, constraint] : m_Constraints)
		{
			if (constraint)
			{
				m_PhysicsSystem->RemoveConstraint(constraint);
			}
		}
		m_Constraints.clear();

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

		// Clear cached mesh shapes
		m_CachedMeshShapes.clear();

		m_TimeAccumulator = 0.0f;
	}

	JPH::BodyID PhysicsWorld::CreateBody(
		entt::entity entity,
		const RigidBodyComponent& rigidBody,
		const ColliderComponent& collider,
		const glm::mat4& transform,
		const AssetPool* assetPool)
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

		// Create shape with entity scale applied
		JPH::Ref<JPH::Shape> shape = CreateShape(collider, scale, assetPool);
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

			// Enable CCD if requested (prevents tunneling for fast objects, has performance cost)
			if (rigidBody.useCCD)
				bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
		}

		bodySettings.mFriction = rigidBody.friction;
		bodySettings.mRestitution = rigidBody.restitution;
		bodySettings.mLinearDamping = rigidBody.linearDamping;
		bodySettings.mAngularDamping = rigidBody.angularDamping;
		bodySettings.mGravityFactor = rigidBody.gravityScale;
		bodySettings.mIsSensor = collider.isTrigger;

		// Apply DOF constraint locks (must be set before body creation)
		JPH::EAllowedDOFs allowedDOFs = JPH::EAllowedDOFs::All;
		if (rigidBody.lockPositionX)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::TranslationX;
		if (rigidBody.lockPositionY)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::TranslationY;
		if (rigidBody.lockPositionZ)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::TranslationZ;
		if (rigidBody.lockRotationX)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::RotationX;
		if (rigidBody.lockRotationY)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::RotationY;
		if (rigidBody.lockRotationZ)
			allowedDOFs = allowedDOFs & ~JPH::EAllowedDOFs::RotationZ;
		bodySettings.mAllowedDOFs = allowedDOFs;

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

	void PhysicsWorld::CreateConstraint(
		entt::entity entity,
		const ConstraintComponent& constraint,
		Scene* scene)
	{
		// Get body A (the entity that owns the constraint)
		auto itA = m_EntityToBody.find(entity);
		if (itA == m_EntityToBody.end())
		{
			LOG_ENGINE_WARN("CreateConstraint: Entity has no physics body");
			return;
		}
		JPH::BodyID bodyIdA = itA->second;

		// Get body B (connected entity or world)
		JPH::BodyID bodyIdB;
		bool attachedToWorld = (constraint.connectedEntity == entt::null);
		if (!attachedToWorld)
		{
			auto itB = m_EntityToBody.find(constraint.connectedEntity);
			if (itB == m_EntityToBody.end())
			{
				LOG_ENGINE_WARN("CreateConstraint: Connected entity has no physics body");
				return;
			}
			bodyIdB = itB->second;
		}

		auto& bodyInterface = m_PhysicsSystem->GetBodyInterface();

		// Get body pointers (locked)
		JPH::BodyLockWrite lockA(m_PhysicsSystem->GetBodyLockInterface(), bodyIdA);
		if (!lockA.Succeeded())
		{
			LOG_ENGINE_ERROR("CreateConstraint: Failed to lock body A");
			return;
		}
		JPH::Body& bodyA = lockA.GetBody();
		JPH::Body* bodyBPtr = nullptr;

		std::unique_ptr<JPH::BodyLockWrite> lockB;
		if (!attachedToWorld)
		{
			lockB = std::make_unique<JPH::BodyLockWrite>(m_PhysicsSystem->GetBodyLockInterface(), bodyIdB);
			if (!lockB->Succeeded())
			{
				LOG_ENGINE_ERROR("CreateConstraint: Failed to lock body B");
				return;
			}
			bodyBPtr = &lockB->GetBody();
		}

		// Create the appropriate constraint type
		JPH::Ref<JPH::Constraint> joltConstraint;

		switch (constraint.type)
		{
		case ConstraintType::Fixed:
		{
			JPH::FixedConstraintSettings settings;
			settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
			settings.mPoint1 = ToJolt(constraint.anchorA);
			settings.mPoint2 = ToJolt(constraint.anchorB);

			if (attachedToWorld)
				joltConstraint = settings.Create(bodyA, JPH::Body::sFixedToWorld);
			else
				joltConstraint = settings.Create(bodyA, *bodyBPtr);
			break;
		}

		case ConstraintType::Hinge:
		{
			JPH::HingeConstraintSettings settings;
			settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
			settings.mPoint1 = ToJolt(constraint.anchorA);
			settings.mPoint2 = ToJolt(constraint.anchorB);
			settings.mHingeAxis1 = ToJolt(glm::normalize(constraint.axis));
			settings.mHingeAxis2 = ToJolt(glm::normalize(constraint.axis));
			settings.mNormalAxis1 = JPH::Vec3(1, 0, 0); // Perpendicular to hinge axis
			settings.mNormalAxis2 = JPH::Vec3(1, 0, 0);

			if (constraint.limitsEnabled)
			{
				settings.mLimitsMin = glm::radians(constraint.limitMin);
				settings.mLimitsMax = glm::radians(constraint.limitMax);
			}

			if (attachedToWorld)
				joltConstraint = settings.Create(bodyA, JPH::Body::sFixedToWorld);
			else
				joltConstraint = settings.Create(bodyA, *bodyBPtr);

			// Configure motor if enabled
			if (constraint.motorEnabled)
			{
				auto* hingeConstraint = static_cast<JPH::HingeConstraint*>(joltConstraint.GetPtr());
				hingeConstraint->SetMotorState(JPH::EMotorState::Velocity);
				hingeConstraint->SetTargetAngularVelocity(constraint.motorTargetVelocity);
				JPH::MotorSettings motorSettings;
				motorSettings.mMaxTorqueLimit = constraint.motorMaxForce;
				motorSettings.mMinTorqueLimit = -constraint.motorMaxForce;
				hingeConstraint->GetMotorSettings() = motorSettings;
			}
			break;
		}

		case ConstraintType::Slider:
		{
			JPH::SliderConstraintSettings settings;
			settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
			settings.mPoint1 = ToJolt(constraint.anchorA);
			settings.mPoint2 = ToJolt(constraint.anchorB);
			settings.mSliderAxis1 = ToJolt(glm::normalize(constraint.axis));
			settings.mSliderAxis2 = ToJolt(glm::normalize(constraint.axis));

			if (constraint.limitsEnabled)
			{
				settings.mLimitsMin = constraint.limitMin;
				settings.mLimitsMax = constraint.limitMax;
			}

			if (attachedToWorld)
				joltConstraint = settings.Create(bodyA, JPH::Body::sFixedToWorld);
			else
				joltConstraint = settings.Create(bodyA, *bodyBPtr);

			// Configure motor if enabled
			if (constraint.motorEnabled)
			{
				auto* sliderConstraint = static_cast<JPH::SliderConstraint*>(joltConstraint.GetPtr());
				sliderConstraint->SetMotorState(JPH::EMotorState::Velocity);
				sliderConstraint->SetTargetVelocity(constraint.motorTargetVelocity);
				JPH::MotorSettings motorSettings;
				motorSettings.mMaxForceLimit = constraint.motorMaxForce;
				motorSettings.mMinForceLimit = -constraint.motorMaxForce;
				sliderConstraint->GetMotorSettings() = motorSettings;
			}
			break;
		}

		case ConstraintType::Distance:
		{
			JPH::DistanceConstraintSettings settings;
			settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
			settings.mPoint1 = ToJolt(constraint.anchorA);
			settings.mPoint2 = ToJolt(constraint.anchorB);
			settings.mMinDistance = constraint.minDistance;
			settings.mMaxDistance = constraint.maxDistance;

			if (attachedToWorld)
				joltConstraint = settings.Create(bodyA, JPH::Body::sFixedToWorld);
			else
				joltConstraint = settings.Create(bodyA, *bodyBPtr);
			break;
		}

		case ConstraintType::Cone:
		{
			JPH::ConeConstraintSettings settings;
			settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
			settings.mPoint1 = ToJolt(constraint.anchorA);
			settings.mPoint2 = ToJolt(constraint.anchorB);
			settings.mTwistAxis1 = ToJolt(glm::normalize(constraint.axis));
			settings.mTwistAxis2 = ToJolt(glm::normalize(constraint.axis));
			settings.mHalfConeAngle = glm::radians(constraint.coneHalfAngle);

			if (attachedToWorld)
				joltConstraint = settings.Create(bodyA, JPH::Body::sFixedToWorld);
			else
				joltConstraint = settings.Create(bodyA, *bodyBPtr);
			break;
		}

		case ConstraintType::Point:
		{
			JPH::PointConstraintSettings settings;
			settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
			settings.mPoint1 = ToJolt(constraint.anchorA);
			settings.mPoint2 = ToJolt(constraint.anchorB);

			if (attachedToWorld)
				joltConstraint = settings.Create(bodyA, JPH::Body::sFixedToWorld);
			else
				joltConstraint = settings.Create(bodyA, *bodyBPtr);
			break;
		}

		case ConstraintType::SixDOF:
		{
			JPH::SixDOFConstraintSettings settings;
			settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
			settings.mPosition1 = ToJolt(constraint.anchorA);
			settings.mPosition2 = ToJolt(constraint.anchorB);

			// Configure axes based on limits
			if (constraint.limitsEnabled)
			{
				// Lock all axes by default, then free the ones we want
				for (int i = 0; i < 6; i++)
				{
					settings.MakeFreeAxis(static_cast<JPH::SixDOFConstraintSettings::EAxis>(i));
				}
			}

			if (attachedToWorld)
				joltConstraint = settings.Create(bodyA, JPH::Body::sFixedToWorld);
			else
				joltConstraint = settings.Create(bodyA, *bodyBPtr);
			break;
		}

		default:
			LOG_ENGINE_ERROR("CreateConstraint: Unknown constraint type");
			return;
		}

		if (!joltConstraint)
		{
			LOG_ENGINE_ERROR("CreateConstraint: Failed to create constraint");
			return;
		}

		// Add constraint to physics system
		m_PhysicsSystem->AddConstraint(joltConstraint);

		// Store in our map
		m_Constraints[entity] = joltConstraint;

		LOG_ENGINE_TRACE("Created constraint for entity");
	}

	void PhysicsWorld::DestroyConstraint(entt::entity entity)
	{
		auto it = m_Constraints.find(entity);
		if (it == m_Constraints.end())
			return;

		if (it->second)
		{
			m_PhysicsSystem->RemoveConstraint(it->second);
		}
		m_Constraints.erase(it);
	}

	bool PhysicsWorld::HasConstraint(entt::entity entity) const
	{
		return m_Constraints.find(entity) != m_Constraints.end();
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

	JPH::Ref<JPH::Shape> PhysicsWorld::CreateShape(const ColliderComponent& collider, const glm::vec3& scale, const AssetPool* assetPool)
	{
		switch (collider.shape)
		{
		case ColliderShape::Box:
		{
			// Use minimal convex radius for sharp edge collisions
			// Default 0.05m causes significant edge/corner phasing
			constexpr float boxConvexRadius = 0.001f;
			return new JPH::BoxShape(ToJolt(collider.boxHalfExtents * scale), boxConvexRadius);
		}

		case ColliderShape::Sphere:
			// Use max scale component for uniform sphere scaling
			return new JPH::SphereShape(collider.sphereRadius * glm::max(glm::max(scale.x, scale.y), scale.z));

		case ColliderShape::Capsule:
			// Scale height by Y, radius by max of X and Z
			return new JPH::CapsuleShape(collider.capsuleHalfHeight * scale.y, collider.capsuleRadius * glm::max(scale.x, scale.z));

		case ColliderShape::ConvexMesh:
		{
			// Check if we have asset pool and valid mesh GUID
			if (!assetPool || collider.meshGuid == LR_GUID::INVALID)
			{
				LOG_ENGINE_WARN("ConvexMesh collider requires valid mesh asset, using box fallback");
				return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));
			}

			// Check if shape is cached
			JPH::Ref<JPH::Shape> baseShape;
			auto cacheIt = m_CachedMeshShapes.find(collider.meshGuid);
			if (cacheIt != m_CachedMeshShapes.end())
			{
				baseShape = cacheIt->second;
			}
			else
			{
				// Get mesh metadata
				auto meshMeta = assetPool->find<MeshMetadata>(collider.meshGuid);
				if (!meshMeta || meshMeta->TriCount == 0)
				{
					LOG_ENGINE_WARN("ConvexMesh: Could not find mesh metadata for GUID, using box fallback");
					return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));
				}

				// Build vertex list from mesh triangles
				std::vector<JPH::Vec3> vertices;
				vertices.reserve(meshMeta->TriCount * 3);

				for (uint32_t i = 0; i < meshMeta->TriCount; i++)
				{
					const Gpu::TrianglePositions& tri = assetPool->TriPositionBuffer[meshMeta->firstTriIdx + i];
					vertices.emplace_back(tri.v0.x, tri.v0.y, tri.v0.z);
					vertices.emplace_back(tri.v1.x, tri.v1.y, tri.v1.z);
					vertices.emplace_back(tri.v2.x, tri.v2.y, tri.v2.z);
				}

				// Create convex hull from vertices
				JPH::ConvexHullShapeSettings settings(vertices.data(), static_cast<int>(vertices.size()));
				settings.mMaxConvexRadius = 0.05f; // Skin width for collision detection

				auto result = settings.Create();
				if (!result.IsValid())
				{
					LOG_ENGINE_ERROR("Failed to create ConvexHull shape: {}", result.GetError().c_str());
					return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));
				}

				baseShape = result.Get();
				m_CachedMeshShapes[collider.meshGuid] = baseShape;
				LOG_ENGINE_TRACE("Created ConvexMesh collision shape with {} vertices", vertices.size());
			}

			// Apply scale if not uniform (1,1,1)
			if (scale != glm::vec3(1.0f))
			{
				return new JPH::ScaledShape(baseShape, ToJolt(scale));
			}
			return baseShape;
		}

		case ColliderShape::TriangleMesh:
		{
			// Check if we have asset pool and valid mesh GUID
			if (!assetPool || collider.meshGuid == LR_GUID::INVALID)
			{
				LOG_ENGINE_WARN("TriangleMesh collider requires valid mesh asset, using box fallback");
				return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));
			}

			// Check if shape is cached
			JPH::Ref<JPH::Shape> baseShape;
			auto cacheIt = m_CachedMeshShapes.find(collider.meshGuid);
			if (cacheIt != m_CachedMeshShapes.end())
			{
				baseShape = cacheIt->second;
			}
			else
			{
				// Get mesh metadata
				auto meshMeta = assetPool->find<MeshMetadata>(collider.meshGuid);
				if (!meshMeta || meshMeta->TriCount == 0)
				{
					LOG_ENGINE_WARN("TriangleMesh: Could not find mesh metadata for GUID, using box fallback");
					return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));
				}

				// Build triangle list
				JPH::TriangleList triangles;
				triangles.reserve(meshMeta->TriCount);

				for (uint32_t i = 0; i < meshMeta->TriCount; i++)
				{
					const Gpu::TrianglePositions& tri = assetPool->TriPositionBuffer[meshMeta->firstTriIdx + i];
					triangles.push_back(JPH::Triangle(
						JPH::Float3(tri.v0.x, tri.v0.y, tri.v0.z),
						JPH::Float3(tri.v1.x, tri.v1.y, tri.v1.z),
						JPH::Float3(tri.v2.x, tri.v2.y, tri.v2.z)
					));
				}

				// Create mesh shape (optimized for static geometry)
				JPH::MeshShapeSettings settings(triangles);

				auto result = settings.Create();
				if (!result.IsValid())
				{
					LOG_ENGINE_ERROR("Failed to create TriangleMesh shape: {}", result.GetError().c_str());
					return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));
				}

				baseShape = result.Get();
				m_CachedMeshShapes[collider.meshGuid] = baseShape;
				LOG_ENGINE_TRACE("Created TriangleMesh collision shape with {} triangles", triangles.size());
			}

			// Apply scale if not uniform (1,1,1)
			if (scale != glm::vec3(1.0f))
			{
				return new JPH::ScaledShape(baseShape, ToJolt(scale));
			}
			return baseShape;
		}

		case ColliderShape::Heightfield:
			// Heightfield requires specialized data - not implemented yet
			LOG_ENGINE_WARN("Heightfield collision not yet implemented, using box fallback");
			return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));

		default:
			LOG_ENGINE_ERROR("Unknown collider shape type");
			return new JPH::BoxShape(ToJolt(glm::vec3(0.5f) * scale));
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
}
