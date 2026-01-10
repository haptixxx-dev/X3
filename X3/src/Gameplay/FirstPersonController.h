#pragma once

#include "lrpch.h"
#include "Project/Scene/Components.h"
#include "Physics/PhysicsWorld.h"
#include "Core/IWindow.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace X3
{
	class Scene;

	// First-Person Controller
	// Handles camera look, movement input, and integrates with CharacterController physics
	class FirstPersonController
	{
	public:
		FirstPersonController() = default;

		// Initialize controller with window reference for input
		void Initialize(IWindow* window);

		// Update camera look (call each frame)
		// Returns the look direction for physics movement
		void UpdateCamera(
			FirstPersonCameraComponent& camera,
			TransformComponent& transform,
			float deltaTime
		);

		// Process movement input and update character state
		// Returns desired velocity for physics system
		glm::vec3 UpdateMovement(
			CharacterControllerComponent& character,
			const FirstPersonCameraComponent& camera,
			float deltaTime
		);

		// Apply physics results back to transform
		void SyncTransformFromPhysics(
			TransformComponent& transform,
			PhysicsCharacterController* physicsController,
			const FirstPersonCameraComponent& camera
		);

		// Input state management
		void OnMouseMove(float xpos, float ypos);
		void SetCursorLocked(bool locked);
		bool IsCursorLocked() const { return m_CursorLocked; }

		// Settings
		void SetMouseSensitivity(float sensitivity) { m_MouseSensitivity = sensitivity; }

	private:
		// Calculate forward/right vectors from yaw (ignoring pitch for movement)
		glm::vec3 GetMovementForward(float yaw) const;
		glm::vec3 GetMovementRight(float yaw) const;

		// Handle different movement states
		void UpdateGroundedMovement(CharacterControllerComponent& cc, const glm::vec3& inputDir, float speed, float dt);
		void UpdateAirMovement(CharacterControllerComponent& cc, const glm::vec3& inputDir, float dt);
		void UpdateSliding(CharacterControllerComponent& cc, const glm::vec3& moveDir, float dt);
		void UpdateWallRun(CharacterControllerComponent& cc, PhysicsWorld* physics, const glm::vec3& moveDir, float dt);

		// State transitions
		void TryJump(CharacterControllerComponent& cc);
		void TrySlide(CharacterControllerComponent& cc, const glm::vec3& moveDir);
		void TryWallRun(CharacterControllerComponent& cc, PhysicsWorld* physics, const glm::vec3& moveDir);
		void EndSlide(CharacterControllerComponent& cc);
		void EndWallRun(CharacterControllerComponent& cc);

		// Wall detection
		bool DetectWall(PhysicsWorld* physics, const glm::vec3& pos, const glm::vec3& dir, float dist, glm::vec3& outNormal);

	private:
		IWindow* m_Window = nullptr;
		bool m_CursorLocked = false;

		// Mouse state
		float m_MouseSensitivity = 0.15f;
		float m_LastMouseX = 0.0f;
		float m_LastMouseY = 0.0f;
		float m_MouseDeltaX = 0.0f;
		float m_MouseDeltaY = 0.0f;
		bool m_FirstMouse = true;

		// Input state (set from window polling)
		bool m_Forward = false;
		bool m_Back = false;
		bool m_Left = false;
		bool m_Right = false;
		bool m_Jump = false;
		bool m_JumpPressed = false;  // Edge detection
		bool m_Crouch = false;
		bool m_Sprint = false;
	};

	// ============================================================================
	// FLOW SYSTEM (helper functions for FlowStateComponent defined in Components.h)
	// ============================================================================

	// Flow system helper functions
	class FlowSystem
	{
	public:
		static void Update(FlowStateComponent& flow, const CharacterControllerComponent& cc, float deltaTime);
		static void AddFlow(FlowStateComponent& flow, float amount);
		static void OnDamage(FlowStateComponent& flow);
		static float GetJumpBonus(const FlowStateComponent& flow);
		static float GetWallRunBonus(const FlowStateComponent& flow);
		static float GetMantleSpeedMultiplier(const FlowStateComponent& flow);
		static bool IsHighFlow(const FlowStateComponent& flow);
		static bool IsMaxFlow(const FlowStateComponent& flow);
	};
}
