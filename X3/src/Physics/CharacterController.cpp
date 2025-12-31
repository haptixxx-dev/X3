#include "Physics/CharacterController.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/CollisionLayers.h"

namespace X3
{
	PhysicsCharacterController::PhysicsCharacterController(
		PhysicsWorld* world,
		entt::entity entity,
		const CharacterControllerComponent& config,
		const glm::vec3& position,
		const glm::quat& rotation)
		: m_World(world)
		, m_Entity(entity)
		, m_JumpForce(config.jumpForce)
	{
		// Create capsule shape for the character
		// Jolt capsule is defined by half-height (cylinder part) and radius
		float halfHeight = (config.capsuleHeight - 2.0f * config.capsuleRadius) * 0.5f;
		halfHeight = std::max(halfHeight, 0.01f); // Ensure positive half-height

		m_StandingShape = new JPH::CapsuleShape(halfHeight, config.capsuleRadius);

		// Create character settings
		JPH::CharacterVirtualSettings settings;
		settings.mMaxSlopeAngle = glm::radians(config.maxSlopeAngle);
		settings.mMaxStrength = config.mass * 10.0f; // Pushing force
		settings.mShape = m_StandingShape;
		settings.mMass = config.mass;
		settings.mPenetrationRecoverySpeed = 1.0f;
		settings.mPredictiveContactDistance = config.skinWidth;

		// Create the character
		m_Character = std::make_unique<JPH::CharacterVirtual>(
			&settings,
			ToJolt(position),
			ToJolt(rotation),
			0, // User data
			world->GetPhysicsSystem()
		);

		m_Character->SetListener(nullptr); // Could add character contact listener
	}

	PhysicsCharacterController::~PhysicsCharacterController()
	{
	}

	void PhysicsCharacterController::Update(float deltaTime, const glm::vec3& gravity)
	{
		if (!m_Character)
			return;

		JPH::PhysicsSystem* physicsSystem = m_World->GetPhysicsSystem();

		// Get current velocity and apply gravity if not grounded
		JPH::Vec3 currentVelocity = m_Character->GetLinearVelocity();
		JPH::Vec3 desiredVelocity = ToJolt(m_InputVelocity);

		// Apply gravity
		JPH::Vec3 gravityVec = ToJolt(gravity);

		// If grounded, project velocity onto ground plane
		if (m_Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround)
		{
			// Handle jump request
			if (m_WantsJump)
			{
				// Apply upward velocity for jump
				currentVelocity = JPH::Vec3(desiredVelocity.GetX(), m_JumpForce, desiredVelocity.GetZ());
				m_WantsJump = false;
			}
			else
			{
				// Preserve horizontal velocity, reset vertical
				currentVelocity = JPH::Vec3(desiredVelocity.GetX(), 0.0f, desiredVelocity.GetZ());
			}
		}
		else
		{
			// In air - apply gravity
			currentVelocity += gravityVec * deltaTime;
			// Preserve horizontal input
			currentVelocity = JPH::Vec3(desiredVelocity.GetX(), currentVelocity.GetY(), desiredVelocity.GetZ());
			// Clear jump request if we're already in the air
			m_WantsJump = false;
		}

		m_Character->SetLinearVelocity(currentVelocity);

		// Update character
		JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
		updateSettings.mStickToFloorStepDown = JPH::Vec3(0, -0.5f, 0); // Step down amount
		updateSettings.mWalkStairsStepUp = JPH::Vec3(0, 0.4f, 0);      // Step up amount

		// Create broad phase layer filter
		JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(
			m_World->GetObjectVsBroadPhaseLayerFilter(),
			Layers::CHARACTER
		);

		// Create object layer filter
		JPH::DefaultObjectLayerFilter objectLayerFilter(
			m_World->GetObjectLayerPairFilter(),
			Layers::CHARACTER
		);

		// Body filter that ignores our own body
		JPH::IgnoreMultipleBodiesFilter bodyFilter;

		// Shape filter
		JPH::ShapeFilter shapeFilter;

		m_Character->ExtendedUpdate(
			deltaTime,
			gravityVec,
			updateSettings,
			broadPhaseFilter,
			objectLayerFilter,
			bodyFilter,
			shapeFilter,
			*m_World->GetTempAllocator()
		);

		// Clear input velocity after update
		m_InputVelocity = glm::vec3(0.0f);
	}

	void PhysicsCharacterController::SetLinearVelocity(const glm::vec3& velocity)
	{
		if (m_Character)
			m_Character->SetLinearVelocity(ToJolt(velocity));
	}

	void PhysicsCharacterController::SetInputVelocity(const glm::vec3& desiredVelocity)
	{
		m_InputVelocity = desiredVelocity;
	}

	glm::vec3 PhysicsCharacterController::GetLinearVelocity() const
	{
		if (m_Character)
			return FromJolt(m_Character->GetLinearVelocity());
		return glm::vec3(0.0f);
	}

	glm::vec3 PhysicsCharacterController::GetPosition() const
	{
		if (m_Character)
			return FromJolt(m_Character->GetPosition());
		return glm::vec3(0.0f);
	}

	glm::quat PhysicsCharacterController::GetRotation() const
	{
		if (m_Character)
			return FromJolt(m_Character->GetRotation());
		return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	}

	void PhysicsCharacterController::SetPosition(const glm::vec3& position)
	{
		if (m_Character)
			m_Character->SetPosition(ToJolt(position));
	}

	void PhysicsCharacterController::SetRotation(const glm::quat& rotation)
	{
		if (m_Character)
			m_Character->SetRotation(ToJolt(rotation));
	}

	bool PhysicsCharacterController::IsGrounded() const
	{
		if (m_Character)
			return m_Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
		return false;
	}

	bool PhysicsCharacterController::IsOnSlope() const
	{
		if (m_Character)
			return m_Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnSteepGround;
		return false;
	}

	glm::vec3 PhysicsCharacterController::GetGroundNormal() const
	{
		if (m_Character)
			return FromJolt(m_Character->GetGroundNormal());
		return glm::vec3(0.0f, 1.0f, 0.0f);
	}

	float PhysicsCharacterController::GetGroundDistance() const
	{
		// CharacterVirtual doesn't expose ground distance directly
		// Return 0 if grounded, large value otherwise
		if (m_Character && m_Character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround)
			return 0.0f;
		return 1000.0f;
	}

	void PhysicsCharacterController::SetMaxSlopeAngle(float degrees)
	{
		if (m_Character)
			m_Character->SetMaxSlopeAngle(glm::radians(degrees));
	}

	void PhysicsCharacterController::SetMass(float mass)
	{
		if (m_Character)
			m_Character->SetMass(mass);
	}
}
