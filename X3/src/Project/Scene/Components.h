#pragma once

#include "lrpch.h"
#include "Core/GUID.h"
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

	struct MaterialComponent {
		glm::vec4 emission = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz: color, w: strength
		glm::vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};    // xyz: albedo/color, w: padding

		// PBR parameters
		float metallic = 0.0f;   // 0.0 = dielectric, 1.0 = metal
		float roughness = 0.5f;  // 0.0 = smooth, 1.0 = rough
		float ao = 1.0f;         // Ambient occlusion (1.0 = no occlusion)
		float _padding = 0.0f;   // Padding for alignment
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

	struct CharacterControllerComponent {
		// Shape
		float capsuleRadius = 0.3f;
		float capsuleHeight = 1.8f;   // Total height

		// Movement
		float maxSlopeAngle = 45.0f;  // Degrees - max slope character can walk up
		float maxStepHeight = 0.3f;   // Meters - max step character can climb
		float walkSpeed = 5.0f;       // m/s
		float sprintSpeed = 8.0f;     // m/s
		float jumpForce = 5.0f;       // m/s initial velocity

		// Physics
		float mass = 70.0f;           // kg
		float skinWidth = 0.02f;      // Collision skin

		// Runtime state (not serialized)
		glm::vec3 velocity = glm::vec3(0.0f);
		bool isGrounded = false;
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