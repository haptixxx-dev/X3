#pragma once

#include "lrpch.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace X3
{
    class EditorCameraController
    {
    public:
        EditorCameraController();

        // Update camera based on input (call each frame)
        void Update(float deltaTime);

        // Input handling
        void OnMouseMove(float xpos, float ypos);
        void OnMouseButton(int button, bool pressed);
        void OnScroll(float yoffset);
        void SetKeyState(int keycode, bool pressed);

        // Camera state
        glm::vec3 GetPosition() const { return m_Position; }
        glm::vec3 GetRotation() const { return m_Rotation; } // pitch, yaw, roll in radians
        glm::mat4 GetViewMatrix() const;
        glm::mat4 GetTransformMatrix() const; // For rendering (inverse of view matrix)
        float GetFOV() const { return m_FOV; }

        void SetPosition(const glm::vec3& position) { m_Position = position; }
        void SetRotation(const glm::vec3& rotation) { m_Rotation = rotation; }
        void SetFOV(float fov) { m_FOV = fov; }

        // Settings
        float MovementSpeed = 5.0f;
        float RotationSpeed = 0.002f;
        float ScrollSpeed = 2.0f;

    private:
        glm::vec3 GetForwardVector() const;
        glm::vec3 GetRightVector() const;
        glm::vec3 GetUpVector() const;

        glm::vec3 m_Position;
        glm::vec3 m_Rotation; // pitch, yaw, roll in radians
        float m_FOV;

        // Input state
        bool m_RightMousePressed = false;
        bool m_MiddleMousePressed = false;
        bool m_LeftMousePressed = false;
        bool m_AltPressed = false;
        float m_LastMouseX = 0.0f;
        float m_LastMouseY = 0.0f;
        bool m_FirstMouse = true;

        // Key states
        bool m_KeyW = false;
        bool m_KeyA = false;
        bool m_KeyS = false;
        bool m_KeyD = false;
        bool m_KeyQ = false; // Down
        bool m_KeyE = false; // Up
        bool m_KeyShift = false; // Speed boost

        // Orbit/focus point for Alt+LMB orbit
        glm::vec3 m_FocusPoint = glm::vec3(0.0f);
        float m_DistanceToFocus = 10.0f;
    };
}
