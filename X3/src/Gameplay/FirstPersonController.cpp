#include "Gameplay/FirstPersonController.h"
#include "Core/Time.h"
#include "Core/Events/KeyEvents.h"
#include <algorithm>

namespace X3
{
	void FirstPersonController::Initialize(IWindow* window)
	{
		m_Window = window;
		m_FirstMouse = true;
	}

	void FirstPersonController::OnMouseMove(float xpos, float ypos)
	{
		if (m_FirstMouse) {
			m_LastMouseX = xpos;
			m_LastMouseY = ypos;
			m_FirstMouse = false;
			return;
		}

		m_MouseDeltaX = xpos - m_LastMouseX;
		m_MouseDeltaY = ypos - m_LastMouseY;
		m_LastMouseX = xpos;
		m_LastMouseY = ypos;
	}

	void FirstPersonController::SetCursorLocked(bool locked)
	{
		m_CursorLocked = locked;
		// Note: Actual cursor lock/hide should be done by the window system
		// The caller should call glfwSetInputMode after this
	}

	void FirstPersonController::UpdateCamera(
		FirstPersonCameraComponent& camera,
		TransformComponent& transform,
		float deltaTime)
	{
		if (!m_CursorLocked) {
			m_MouseDeltaX = 0.0f;
			m_MouseDeltaY = 0.0f;
			return;
		}

		// Apply mouse look
		float sensitivity = camera.mouseSensitivity;
		camera.currentYaw -= m_MouseDeltaX * sensitivity * 0.01f;  // Convert to radians
		camera.currentPitch -= m_MouseDeltaY * sensitivity * 0.01f;

		// Clamp pitch
		float pitchMinRad = glm::radians(camera.pitchMin);
		float pitchMaxRad = glm::radians(camera.pitchMax);
		camera.currentPitch = glm::clamp(camera.currentPitch, pitchMinRad, pitchMaxRad);

		// Wrap yaw
		if (camera.currentYaw > glm::pi<float>()) camera.currentYaw -= glm::two_pi<float>();
		if (camera.currentYaw < -glm::pi<float>()) camera.currentYaw += glm::two_pi<float>();

		// Update transform rotation (yaw only for entity, pitch is camera-local)
		transform.SetRotation(glm::vec3(
			glm::degrees(camera.currentPitch),
			glm::degrees(camera.currentYaw),
			0.0f
		));

		// Clear mouse delta for next frame
		m_MouseDeltaX = 0.0f;
		m_MouseDeltaY = 0.0f;

		// FOV interpolation would happen here based on movement state
		// (handled externally based on CharacterControllerComponent state)
	}

	glm::vec3 FirstPersonController::GetMovementForward(float yaw) const
	{
		return glm::vec3(
			sin(yaw),
			0.0f,
			cos(yaw)
		);
	}

	glm::vec3 FirstPersonController::GetMovementRight(float yaw) const
	{
		return glm::vec3(
			cos(yaw),
			0.0f,
			-sin(yaw)
		);
	}

	glm::vec3 FirstPersonController::UpdateMovement(
		CharacterControllerComponent& cc,
		const FirstPersonCameraComponent& camera,
		float deltaTime)
	{
		if (!m_Window) return glm::vec3(0.0f);

		// Poll input state
		m_Forward = m_Window->isKeyPressed(Key::W);
		m_Back = m_Window->isKeyPressed(Key::S);
		m_Left = m_Window->isKeyPressed(Key::A);
		m_Right = m_Window->isKeyPressed(Key::D);
		bool jumpHeld = m_Window->isKeyPressed(Key::SPACE);
		m_Crouch = m_Window->isKeyPressed(Key::LEFT_CONTROL) || m_Window->isKeyPressed(Key::C);
		m_Sprint = m_Window->isKeyPressed(Key::LEFT_SHIFT);

		// Edge detection for jump
		bool jumpJustPressed = jumpHeld && !m_JumpPressed;
		m_JumpPressed = jumpHeld;

		// Build input direction
		glm::vec3 inputDir(0.0f);
		glm::vec3 forward = GetMovementForward(camera.currentYaw);
		glm::vec3 right = GetMovementRight(camera.currentYaw);

		if (m_Forward) inputDir += forward;
		if (m_Back) inputDir -= forward;
		if (m_Right) inputDir += right;
		if (m_Left) inputDir -= right;

		if (glm::length(inputDir) > 0.001f) {
			inputDir = glm::normalize(inputDir);
		}
		cc.inputDirection = inputDir;

		// Update timers
		if (cc.coyoteTimer > 0.0f) cc.coyoteTimer -= deltaTime;
		if (cc.jumpBufferTimer > 0.0f) cc.jumpBufferTimer -= deltaTime;
		if (cc.slideCooldownTimer > 0.0f) cc.slideCooldownTimer -= deltaTime;
		if (cc.wallRunTimer > 0.0f) cc.wallRunTimer -= deltaTime;

		// Ground state update
		if (cc.isGrounded) {
			cc.coyoteTimer = cc.coyoteTime;
			cc.jumpsRemaining = cc.maxJumps;
		}

		// Jump buffering
		if (jumpJustPressed) {
			cc.jumpBufferTimer = cc.jumpBufferTime;
			cc.wantsToJump = true;
		}

		// Sprint/crouch intent
		cc.wantsToSprint = m_Sprint;
		cc.wantsToCrouch = m_Crouch;

		// Determine target speed
		float targetSpeed = cc.walkSpeed;
		if (m_Sprint && !cc.isCrouching) {
			targetSpeed = cc.sprintSpeed;
			cc.isSprinting = true;
		} else if (cc.isCrouching) {
			targetSpeed = cc.crouchSpeed;
			cc.isSprinting = false;
		} else if (glm::length(inputDir) > 0.5f) {
			targetSpeed = cc.runSpeed;
			cc.isSprinting = false;
		}

		// State machine
		MovementState prevState = cc.state;

		switch (cc.state) {
			case MovementState::Idle:
			case MovementState::Walking:
			case MovementState::Running:
			case MovementState::Sprinting:
				if (!cc.isGrounded) {
					cc.state = MovementState::Falling;
				} else if (cc.wantsToJump || cc.jumpBufferTimer > 0.0f) {
					TryJump(cc);
				} else if (m_Crouch && glm::length(cc.velocity) > cc.slideMinSpeed && cc.slideCooldownTimer <= 0.0f) {
					TrySlide(cc, inputDir);
				} else if (m_Crouch) {
					cc.state = MovementState::Crouching;
					cc.isCrouching = true;
				} else {
					cc.isCrouching = false;
					UpdateGroundedMovement(cc, inputDir, targetSpeed, deltaTime);

					if (glm::length(inputDir) < 0.1f) {
						cc.state = MovementState::Idle;
					} else if (cc.isSprinting) {
						cc.state = MovementState::Sprinting;
					} else if (targetSpeed >= cc.runSpeed) {
						cc.state = MovementState::Running;
					} else {
						cc.state = MovementState::Walking;
					}
				}
				break;

			case MovementState::Crouching:
				if (!m_Crouch) {
					cc.isCrouching = false;
					cc.state = MovementState::Idle;
				} else if (!cc.isGrounded) {
					cc.state = MovementState::Falling;
				} else if (cc.wantsToJump) {
					TryJump(cc);
					cc.isCrouching = false;
				} else {
					UpdateGroundedMovement(cc, inputDir, cc.crouchSpeed, deltaTime);
				}
				break;

			case MovementState::Jumping:
			case MovementState::Falling:
				UpdateAirMovement(cc, inputDir, deltaTime);

				// Double jump
				if (jumpJustPressed && cc.jumpsRemaining > 0 && cc.state != MovementState::Jumping) {
					TryJump(cc);
				}

				// Land detection
				if (cc.isGrounded) {
					if (m_Crouch && glm::length(cc.velocity) > cc.slideMinSpeed) {
						TrySlide(cc, inputDir);
					} else {
						cc.state = MovementState::Idle;
					}
				}
				break;

			case MovementState::Sliding:
				UpdateSliding(cc, inputDir, deltaTime);

				if (!cc.isGrounded) {
					EndSlide(cc);
					cc.state = MovementState::Falling;
				} else if (!m_Crouch || glm::length(cc.velocity) < cc.slideMinSpeed) {
					EndSlide(cc);
				} else if (jumpJustPressed) {
					EndSlide(cc);
					TryJump(cc);
				}
				break;

			case MovementState::WallRunningLeft:
			case MovementState::WallRunningRight:
				// Wall run timer handled in UpdateWallRun
				if (jumpJustPressed) {
					// Wall jump
					float cosAngle = static_cast<float>(cos(glm::radians(cc.wallJumpAngle)));
					float sinAngle = static_cast<float>(sin(glm::radians(cc.wallJumpAngle)));
					glm::vec3 wallJumpDir = cc.wallNormal * cosAngle +
					                        glm::vec3(0, 1, 0) * sinAngle;
					cc.velocity = glm::normalize(wallJumpDir) * cc.wallJumpForce +
					              glm::vec3(0, cc.jumpForce * 0.8f, 0);
					EndWallRun(cc);
					cc.state = MovementState::Jumping;
				} else if (cc.wallRunTimer <= 0.0f || cc.isGrounded) {
					EndWallRun(cc);
				}
				break;

			case MovementState::Mantling:
				// Mantle animation would be handled here
				// For now, just transition back to idle when complete
				if (cc.isGrounded) {
					cc.state = MovementState::Idle;
				}
				break;
		}

		cc.wantsToJump = false;
		return cc.velocity;
	}

	void FirstPersonController::UpdateGroundedMovement(
		CharacterControllerComponent& cc,
		const glm::vec3& inputDir,
		float targetSpeed,
		float dt)
	{
		glm::vec3 targetVel = inputDir * targetSpeed;
		glm::vec3 currentHorizontal(cc.velocity.x, 0.0f, cc.velocity.z);

		// Smooth acceleration/deceleration
		float friction = cc.groundFriction;
		glm::vec3 newHorizontal = currentHorizontal + (targetVel - currentHorizontal) * std::min(friction * dt, 1.0f);

		cc.velocity.x = newHorizontal.x;
		cc.velocity.z = newHorizontal.z;
		// Y velocity handled by physics
	}

	void FirstPersonController::UpdateAirMovement(
		CharacterControllerComponent& cc,
		const glm::vec3& inputDir,
		float dt)
	{
		// Full air control (platformer-style)
		glm::vec3 currentHorizontal(cc.velocity.x, 0.0f, cc.velocity.z);
		float currentSpeed = glm::length(currentHorizontal);

		// Air acceleration
		glm::vec3 wishVel = inputDir * cc.sprintSpeed;
		glm::vec3 accel = (wishVel - currentHorizontal) * cc.airAcceleration * dt;

		// Limit acceleration to not exceed desired speed
		if (glm::length(currentHorizontal + accel) > std::max(currentSpeed, cc.sprintSpeed)) {
			accel = glm::normalize(currentHorizontal + accel) * std::max(currentSpeed, cc.sprintSpeed) - currentHorizontal;
		}

		cc.velocity.x += accel.x;
		cc.velocity.z += accel.z;

		// Apply gravity
		cc.velocity.y -= cc.gravity * dt;
	}

	void FirstPersonController::UpdateSliding(
		CharacterControllerComponent& cc,
		const glm::vec3& moveDir,
		float dt)
	{
		// Maintain slide direction, apply friction
		glm::vec3 slideDir = glm::normalize(glm::vec3(cc.velocity.x, 0, cc.velocity.z));
		float speed = glm::length(glm::vec3(cc.velocity.x, 0, cc.velocity.z));

		speed -= cc.slideFriction * dt;
		speed = std::max(speed, 0.0f);

		cc.velocity.x = slideDir.x * speed;
		cc.velocity.z = slideDir.z * speed;
	}

	void FirstPersonController::TryJump(CharacterControllerComponent& cc)
	{
		bool canJump = cc.coyoteTimer > 0.0f || cc.jumpsRemaining > 0;

		if (canJump) {
			float jumpForce = (cc.jumpsRemaining == cc.maxJumps) ? cc.jumpForce : cc.doubleJumpForce;
			cc.velocity.y = jumpForce;
			cc.jumpsRemaining--;
			cc.coyoteTimer = 0.0f;
			cc.jumpBufferTimer = 0.0f;
			cc.state = MovementState::Jumping;
		}
	}

	void FirstPersonController::TrySlide(CharacterControllerComponent& cc, const glm::vec3& moveDir)
	{
		if (cc.slideCooldownTimer > 0.0f) return;

		// Boost velocity in current direction
		float speed = glm::length(glm::vec3(cc.velocity.x, 0, cc.velocity.z));
		glm::vec3 slideDir = (speed > 0.1f)
			? glm::normalize(glm::vec3(cc.velocity.x, 0, cc.velocity.z))
			: moveDir;

		cc.velocity.x = slideDir.x * speed * cc.slideInitialBoost;
		cc.velocity.z = slideDir.z * speed * cc.slideInitialBoost;

		cc.state = MovementState::Sliding;
		cc.isCrouching = true;
		cc.slideTimer = 0.0f;
	}

	void FirstPersonController::EndSlide(CharacterControllerComponent& cc)
	{
		cc.slideCooldownTimer = cc.slideCooldown;
		cc.state = MovementState::Crouching;
	}

	void FirstPersonController::EndWallRun(CharacterControllerComponent& cc)
	{
		cc.wallNormal = glm::vec3(0.0f);
		cc.state = MovementState::Falling;
	}

	void FirstPersonController::SyncTransformFromPhysics(
		TransformComponent& transform,
		PhysicsCharacterController* physicsController,
		const FirstPersonCameraComponent& camera)
	{
		if (!physicsController) return;

		glm::vec3 pos = physicsController->GetPosition();
		pos += camera.eyeOffset;  // Add eye height offset

		transform.SetTranslation(pos);
	}

	// ============================================================================
	// FLOW SYSTEM IMPLEMENTATION
	// ============================================================================

	void FlowSystem::Update(FlowStateComponent& flow, const CharacterControllerComponent& cc, float deltaTime)
	{
		bool isActive = false;

		// Build flow based on movement state
		switch (cc.state) {
			case MovementState::WallRunningLeft:
			case MovementState::WallRunningRight:
				AddFlow(flow, flow.buildRateWallRun * deltaTime);
				isActive = true;
				break;

			case MovementState::Sliding:
				AddFlow(flow, flow.buildRateSlide * deltaTime);
				isActive = true;
				break;

			case MovementState::Sprinting:
				// Slight flow build while sprinting
				AddFlow(flow, flow.buildRateSlide * 0.3f * deltaTime);
				isActive = true;
				break;

			default:
				break;
		}

		// Check for movement
		float speed = glm::length(cc.velocity);
		if (speed < 1.0f) {
			isActive = false;
		}

		// Decay when not active
		if (!isActive) {
			flow.decayTimer += deltaTime;
			if (flow.decayTimer > flow.decayDelay) {
				flow.flowMeter -= flow.decayRateIdle * deltaTime;
				flow.flowMeter = std::max(flow.flowMeter, 0.0f);
			}
		} else {
			flow.decayTimer = 0.0f;
		}

		// Track max flow state change for events
		bool wasMaxFlow = flow.isAtMaxFlow;
		flow.isAtMaxFlow = flow.flowMeter >= flow.maxFlowThreshold;

		// Could dispatch event here when reaching max flow
		if (flow.isAtMaxFlow && !wasMaxFlow) {
			// TODO: Dispatch FlowMaxReached event for UI/audio
		}
	}

	void FlowSystem::AddFlow(FlowStateComponent& flow, float amount)
	{
		flow.flowMeter = std::min(flow.flowMeter + amount, flow.maxFlow);
		flow.decayTimer = 0.0f;
	}

	void FlowSystem::OnDamage(FlowStateComponent& flow)
	{
		flow.flowMeter -= flow.decayRateDamage;
		flow.flowMeter = std::max(flow.flowMeter, 0.0f);
		flow.isAtMaxFlow = false;
	}

	float FlowSystem::GetJumpBonus(const FlowStateComponent& flow)
	{
		if (flow.flowMeter >= flow.highFlowThreshold) {
			return 1.0f + flow.jumpHeightBonus;
		}
		return 1.0f;
	}

	float FlowSystem::GetWallRunBonus(const FlowStateComponent& flow)
	{
		if (flow.flowMeter >= flow.highFlowThreshold) {
			return flow.wallRunDurationBonus;
		}
		return 0.0f;
	}

	float FlowSystem::GetMantleSpeedMultiplier(const FlowStateComponent& flow)
	{
		if (flow.flowMeter >= flow.highFlowThreshold) {
			return 1.0f + flow.mantleSpeedBonus;
		}
		return 1.0f;
	}

	bool FlowSystem::IsHighFlow(const FlowStateComponent& flow)
	{
		return flow.flowMeter >= flow.highFlowThreshold;
	}

	bool FlowSystem::IsMaxFlow(const FlowStateComponent& flow)
	{
		return flow.isAtMaxFlow;
	}
}
