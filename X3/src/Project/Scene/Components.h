#pragma once

#include "lrpch.h"
#include "Core/GUID.h"
#include "Project/Assets/MaterialDesc.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <entt/entt.hpp>

namespace X3
{

	struct IDComponent {
		IDComponent() = default;
		IDComponent(LR_GUID guid)
			: guid(guid) {};

		LR_GUID guid;
	};

	struct TagComponent {
		TagComponent() = default;
		TagComponent(const std::string& tag)
			: Tag(tag) {};

		std::string Tag;
	};

	struct TransformComponent {
	public:
		TransformComponent();
		operator glm::mat4() const;

		inline glm::vec3 GetRotation() const { return glm::degrees(m_Rotation); }
		inline glm::vec3 GetTranslation() const { return m_Translation; }
		inline glm::vec3 GetScale() const { return m_Scale; }

		// Returns the 4x4 Local to World Matrix
		glm::mat4 GetMatrix() const;

		void SetRotation(const glm::vec3& angles);
		void SetTranslation(const glm::vec3& translation);
		void SetScale(const glm::vec3& scale);

		void IncrementRotation(const glm::vec3& delta);
		void IncrementTranslation(const glm::vec3& delta);
		void IncrementScale(const glm::vec3& delta);

	private:
		mutable bool m_MatrixDirty;
		mutable glm::mat4 m_ModelMatrix;

		glm::vec3 m_Rotation;
		glm::vec3 m_Translation;
		glm::vec3 m_Scale;
	};

	struct MeshComponent {
		LR_GUID guid = LR_GUID::INVALID;
		std::string sourceName = "";
	};

	// ONE MATERIAL PER SUBMESH, not one per entity.
	//
	// The fields that used to live directly here are now MaterialDesc, and this
	// component is a vector of them: slot i overrides the material the model file
	// shipped for submesh slot i. Multi-material meshes are extremely common and
	// a single flat material could not express one.
	//
	// The vector is NEVER EMPTY -- the default constructor gives one slot, so
	// every path that reads slots[0] is safe without a guard. Slot count is
	// reconciled against MeshMetadata::materialSlotCount when a mesh is assigned
	// in the inspector; extra slots beyond the mesh's count are ignored rather
	// than trimmed, so swapping a mesh out and back does not lose edits.
	struct MaterialComponent {
		std::vector<MaterialDesc> slots{ MaterialDesc{} };
	};

	struct CameraComponent {
		CameraComponent() = default;
		CameraComponent(float fov)
			: fov(fov) {
		};

		bool isMain{ false };
		float fov{ 90.0f };
		// since we transform the size of the screen in the compute shader to "normalized device coordinates" or NDC for short (-1, 1)
		// half of the screen width is 1. Therefore (screen width / 2) / tan(FOV in radians / 2) can be simplified to 1 / tan(FOV_rad / 2)
		inline const float GetFocalLength() const { return 1.0f/tan(glm::radians(fov)/2.0f); };
	};

	// ============================================================================
	// FIRST-PERSON CAMERA COMPONENT (for runtime gameplay)
	// ============================================================================

	struct FirstPersonCameraComponent {
		FirstPersonCameraComponent() = default;

		// Mouse look settings
		float mouseSensitivity = 0.15f;      // Degrees per pixel
		float pitchMin = -89.0f;              // Look down limit (degrees)
		float pitchMax = 89.0f;               // Look up limit (degrees)

		// FOV settings
		float baseFOV = 90.0f;                // Default FOV
		float sprintFOV = 100.0f;             // FOV when sprinting
		float slideFOV = 105.0f;              // FOV when sliding
		float fovLerpSpeed = 8.0f;            // FOV transition speed

		// Camera offset from character position (for eye height)
		glm::vec3 eyeOffset = glm::vec3(0.0f, 0.7f, 0.0f);  // ~1.7m eye height for 1.8m capsule

		// Head bob (subtle, for grounding)
		bool enableHeadBob = true;
		float bobFrequency = 10.0f;           // Hz when walking
		float bobAmplitude = 0.02f;           // Meters

		// Runtime state (not serialized)
		float currentPitch = 0.0f;            // Radians
		float currentYaw = 0.0f;              // Radians
		float currentFOV = 90.0f;
		float bobPhase = 0.0f;
	};

	enum class LightType {
		DIRECTIONAL = 0,
		POINT = 1,
		SPOT = 2
	};

	struct LightComponent {
		LightType type = LightType::DIRECTIONAL;
		glm::vec3 color = {1.0f, 1.0f, 1.0f};
		float intensity = 1.0f;

		// Point light specific
		float range = 10.0f;
		float attenuation = 1.0f;

		// Spot light specific
		float innerConeAngle = 30.0f;
		float outerConeAngle = 45.0f;
	};

	// ============================================================================
	// PHYSICS COMPONENTS
	// ============================================================================

	enum class BodyType {
		Static = 0,    // Immovable world geometry
		Kinematic = 1, // Script-controlled bodies that affect others
		Dynamic = 2    // Physics-simulated objects
	};

	enum class ColliderShape {
		Box = 0,
		Sphere = 1,
		Capsule = 2,
		ConvexMesh = 3,
		TriangleMesh = 4,
		Heightfield = 5
	};

	struct RigidBodyComponent {
		BodyType bodyType = BodyType::Dynamic;

		// Mass properties
		float mass = 1.0f;              // kg (ignored for static)
		float linearDamping = 0.0f;     // 0 = no damping
		float angularDamping = 0.05f;   // slight default angular damping

		// Material properties
		float friction = 0.5f;          // 0 = frictionless
		float restitution = 0.0f;       // 0 = no bounce, 1 = perfect bounce

		// Rotation constraints
		bool lockRotationX = false;
		bool lockRotationY = false;
		bool lockRotationZ = false;

		// Position constraints
		bool lockPositionX = false;
		bool lockPositionY = false;
		bool lockPositionZ = false;

		// Collision filtering
		uint16_t collisionLayer = 1;     // Default to MOVING layer
		uint16_t collisionMask = 0xFFFF; // Collide with everything by default

		// Gravity
		float gravityScale = 1.0f;

		// Continuous Collision Detection (prevents tunneling for fast objects)
		bool useCCD = false;
	};

	struct ColliderComponent {
		ColliderShape shape = ColliderShape::Box;

		// Shape parameters (use based on shape type)
		glm::vec3 boxHalfExtents = glm::vec3(0.5f);  // Box
		float sphereRadius = 0.5f;                    // Sphere
		float capsuleRadius = 0.25f;                  // Capsule
		float capsuleHalfHeight = 0.5f;               // Capsule
		LR_GUID meshGuid = LR_GUID::INVALID;          // ConvexMesh, TriangleMesh

		// Local offset from entity transform
		glm::vec3 offset = glm::vec3(0.0f);
		glm::vec3 rotationOffset = glm::vec3(0.0f);   // Euler degrees

		// Trigger mode (no collision response, just events)
		bool isTrigger = false;
	};

	// Movement state for advanced character movement
	enum class MovementState {
		Idle,
		Walking,
		Running,
		Sprinting,
		Jumping,
		Falling,
		Sliding,
		WallRunningLeft,
		WallRunningRight,
		Mantling,
		Crouching
	};

	struct CharacterControllerComponent {
		// Shape
		float capsuleRadius = 0.3f;
		float capsuleHeight = 1.8f;        // Total height standing
		float crouchHeight = 1.0f;         // Height when crouching

		// Movement speeds (m/s)
		float walkSpeed = 5.0f;
		float runSpeed = 7.0f;
		float sprintSpeed = 10.0f;
		float crouchSpeed = 3.0f;
		float slideSpeed = 12.0f;          // Initial slide velocity
		float slideMinSpeed = 4.0f;        // Minimum speed to maintain slide
		float wallRunSpeed = 9.0f;

		// Jump settings
		float jumpForce = 6.0f;            // Initial upward velocity
		float doubleJumpForce = 5.5f;      // Slightly weaker second jump
		int maxJumps = 2;                  // Double jump by default
		float coyoteTime = 0.15f;          // Seconds of grace after leaving ground
		float jumpBufferTime = 0.1f;       // Pre-land jump buffer

		// Air control
		float airAcceleration = 15.0f;     // Full platformer-style air control
		float airFriction = 0.5f;

		// Slide settings
		float slideInitialBoost = 1.2f;    // Multiplier to current speed
		float slideFriction = 2.0f;        // Deceleration rate
		float slideCooldown = 0.3f;        // Seconds before can slide again

		// Wall-run settings
		float wallRunDuration = 1.5f;      // Max time on wall
		float wallRunGravity = 2.0f;       // Reduced gravity during wall-run
		float wallJumpForce = 6.0f;        // Jump off wall
		float wallJumpAngle = 45.0f;       // Degrees away from wall
		float wallDetectDistance = 0.4f;   // Ray distance to detect walls

		// Mantle settings
		float mantleReach = 1.5f;          // How high can reach to grab ledge
		float mantleSpeed = 5.0f;          // Speed of mantle animation

		// Physics
		float maxSlopeAngle = 45.0f;       // Degrees
		float maxStepHeight = 0.3f;        // Meters
		float mass = 70.0f;                // kg
		float skinWidth = 0.02f;
		float groundFriction = 8.0f;
		float gravity = 20.0f;             // Custom gravity for snappy feel

		// Runtime state (not serialized in scene files)
		MovementState state = MovementState::Idle;
		glm::vec3 velocity = glm::vec3(0.0f);
		glm::vec3 inputDirection = glm::vec3(0.0f);  // Normalized input
		bool isGrounded = false;
		bool isSprinting = false;
		bool isCrouching = false;
		bool wantsToJump = false;
		bool wantsToCrouch = false;
		bool wantsToSprint = false;
		int jumpsRemaining = 2;
		float coyoteTimer = 0.0f;
		float jumpBufferTimer = 0.0f;
		float slideTimer = 0.0f;
		float slideCooldownTimer = 0.0f;
		float wallRunTimer = 0.0f;
		glm::vec3 wallNormal = glm::vec3(0.0f);      // Current wall normal
		float currentHeight = 1.8f;                  // For crouch interpolation
	};

	// ============================================================================
	// FLOW STATE COMPONENT (for Cascade momentum system)
	// ============================================================================

	struct FlowStateComponent
	{
		// Flow meter (0.0 - 1.0)
		float flowMeter = 0.0f;
		float maxFlow = 1.0f;

		// Build rates
		float buildRateWallRun = 0.3f;      // Per second while wall-running
		float buildRateSlide = 0.2f;        // Per second while sliding
		float buildRateAirChain = 0.15f;    // Per successful air transition
		float buildRateCombat = 0.25f;      // Per hit while moving

		// Decay
		float decayRateIdle = 0.4f;         // Per second when stationary
		float decayRateDamage = 0.5f;       // Instant on taking damage
		float decayDelay = 0.5f;            // Seconds before decay starts

		// Benefits at flow thresholds
		float lowFlowThreshold = 0.3f;      // Some benefits
		float highFlowThreshold = 0.7f;     // Major benefits
		float maxFlowThreshold = 1.0f;      // "In the zone"

		// Bonuses
		float wallRunDurationBonus = 0.5f;  // Extra seconds at high flow
		float jumpHeightBonus = 0.15f;      // % increase at high flow
		float mantleSpeedBonus = 0.3f;      // % faster at high flow

		// Runtime state (not serialized)
		float decayTimer = 0.0f;
		float lastFlowEvent = 0.0f;         // Time of last flow-building action
		bool isAtMaxFlow = false;           // For visual/audio feedback trigger
	};

	// ========================================================================
	// Constraints / Joints
	// ========================================================================

	enum class ConstraintType
	{
		Fixed,      // Locks two bodies together
		Hinge,      // Rotation around a single axis
		Slider,     // Translation along a single axis
		Distance,   // Maintains fixed distance between points
		Cone,       // Rotation within a cone
		Point,      // Ball-and-socket joint
		SixDOF      // Configurable 6 degrees of freedom
	};

	struct ConstraintComponent
	{
		ConstraintType type = ConstraintType::Fixed;

		// Connected entity (entt::null means attached to world)
		entt::entity connectedEntity = entt::null;

		// Anchor points in local space of each body
		glm::vec3 anchorA = glm::vec3(0.0f);  // On this entity
		glm::vec3 anchorB = glm::vec3(0.0f);  // On connected entity (or world)

		// Axis for hinge/slider (normalized direction in local space of body A)
		glm::vec3 axis = glm::vec3(0.0f, 1.0f, 0.0f);

		// Limits for hinge (degrees) or slider (meters)
		bool limitsEnabled = false;
		float limitMin = -180.0f;
		float limitMax = 180.0f;

		// Cone constraint angle (half-angle in degrees)
		float coneHalfAngle = 45.0f;

		// Distance constraint
		float minDistance = 0.0f;
		float maxDistance = 1.0f;

		// Motor
		bool motorEnabled = false;
		float motorTargetVelocity = 0.0f;     // rad/s for hinge, m/s for slider
		float motorMaxForce = 1000.0f;        // Nm for hinge, N for slider

		// Breaking
		bool breakable = false;
		float breakForce = 10000.0f;
		float breakTorque = 10000.0f;

		// Runtime state (not serialized)
		bool isBroken = false;
	};
}