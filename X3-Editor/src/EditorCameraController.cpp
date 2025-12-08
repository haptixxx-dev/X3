#include "EditorCameraController.h"

namespace X3
{
    EditorCameraController::EditorCameraController()
        : m_Position(5.0f, 5.0f, 5.0f)
        , m_Rotation(glm::radians(-30.0f), glm::radians(-45.0f), 0.0f) // pitch, yaw, roll
        , m_FOV(90.0f)
    {
    }

    void EditorCameraController::Update(float deltaTime)
    {
        // Unity-style: Only move with WASD when right mouse button is held
        if (m_RightMousePressed) {
            // Calculate movement speed (with shift boost)
            float speed = MovementSpeed * deltaTime;
            if (m_KeyShift) {
                speed *= 3.0f; // Unity uses 3x speed boost
            }

            // Get camera axes
            glm::vec3 forward = GetForwardVector();
            glm::vec3 right = GetRightVector();
            glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f); // World up

            // Apply movement (Unity-style FPS controls)
            if (m_KeyW) m_Position += forward * speed;
            if (m_KeyS) m_Position -= forward * speed;
            if (m_KeyD) m_Position += right * speed;
            if (m_KeyA) m_Position -= right * speed;
            if (m_KeyE) m_Position += up * speed;
            if (m_KeyQ) m_Position -= up * speed;
        }
    }

    void EditorCameraController::OnMouseMove(float xpos, float ypos)
    {
        if (m_FirstMouse) {
            m_LastMouseX = xpos;
            m_LastMouseY = ypos;
            m_FirstMouse = false;
        }

        float xoffset = xpos - m_LastMouseX;
        float yoffset = ypos - m_LastMouseY;

        m_LastMouseX = xpos;
        m_LastMouseY = ypos;

        // Unity-style: Right mouse button = FPS look
        if (m_RightMousePressed) {
            xoffset *= RotationSpeed;
            yoffset *= RotationSpeed;

            // Update yaw and pitch
            m_Rotation.y += xoffset; // yaw
            m_Rotation.x += yoffset; // pitch

            // Constrain pitch to avoid gimbal lock
            const float maxPitch = glm::radians(89.0f);
            if (m_Rotation.x > maxPitch)
                m_Rotation.x = maxPitch;
            if (m_Rotation.x < -maxPitch)
                m_Rotation.x = -maxPitch;
        }
        // Unity-style: Middle mouse button = Pan
        else if (m_MiddleMousePressed) {
            float panSpeed = 0.005f;
            glm::vec3 right = GetRightVector();
            glm::vec3 up = GetUpVector();

            m_Position -= right * xoffset * panSpeed * m_DistanceToFocus * 0.1f;
            m_Position += up * yoffset * panSpeed * m_DistanceToFocus * 0.1f;
        }
        // Unity-style: Alt + Left mouse button = Orbit around focus point
        else if (m_AltPressed && m_LeftMousePressed) {
            xoffset *= RotationSpeed;
            yoffset *= RotationSpeed;

            // Update yaw and pitch
            m_Rotation.y += xoffset;
            m_Rotation.x += yoffset;

            // Constrain pitch
            const float maxPitch = glm::radians(89.0f);
            if (m_Rotation.x > maxPitch)
                m_Rotation.x = maxPitch;
            if (m_Rotation.x < -maxPitch)
                m_Rotation.x = -maxPitch;

            // Position camera at distance from focus point
            glm::vec3 forward = GetForwardVector();
            m_Position = m_FocusPoint - forward * m_DistanceToFocus;
        }
    }

    void EditorCameraController::OnMouseButton(int button, bool pressed)
    {
        if (pressed) {
            m_FirstMouse = true; // Reset to avoid jump
        }

        if (button == 0) { // Left mouse button
            m_LeftMousePressed = pressed;
        }
        else if (button == 1) { // Right mouse button
            m_RightMousePressed = pressed;
        }
        else if (button == 2) { // Middle mouse button
            m_MiddleMousePressed = pressed;
        }
    }

    void EditorCameraController::OnScroll(float yoffset)
    {
        // Unity-style: Scroll to zoom (move camera forward/back)
        float zoomSpeed = 0.5f * m_DistanceToFocus * 0.1f;
        glm::vec3 forward = GetForwardVector();

        m_Position += forward * yoffset * zoomSpeed;

        // Update distance to focus point
        m_DistanceToFocus = glm::max(0.1f, m_DistanceToFocus - yoffset * zoomSpeed);
    }

    void EditorCameraController::SetKeyState(int keycode, bool pressed)
    {
        switch (keycode) {
            case 87: m_KeyW = pressed; break;      // W
            case 65: m_KeyA = pressed; break;      // A
            case 83: m_KeyS = pressed; break;      // S
            case 68: m_KeyD = pressed; break;      // D
            case 69: m_KeyE = pressed; break;      // E
            case 81: m_KeyQ = pressed; break;      // Q
            case 340: m_KeyShift = pressed; break; // Left Shift
            case 344: m_KeyShift = pressed; break; // Right Shift
        }
    }

    glm::mat4 EditorCameraController::GetViewMatrix() const
    {
        // Create view matrix: rotate then translate
        glm::mat4 rotation = glm::eulerAngleXY(-m_Rotation.x, -m_Rotation.y);
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), -m_Position);
        return rotation * translation;
    }

    glm::mat4 EditorCameraController::GetTransformMatrix() const
    {
        // Transform matrix is inverse of view matrix
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), m_Position);
        glm::mat4 rotation = glm::eulerAngleXY(m_Rotation.x, m_Rotation.y);
        return translation * rotation;
    }

    glm::vec3 EditorCameraController::GetForwardVector() const
    {
        // Calculate forward vector from yaw and pitch
        glm::vec3 forward;
        forward.x = cos(m_Rotation.y) * cos(m_Rotation.x);
        forward.y = sin(m_Rotation.x);
        forward.z = sin(m_Rotation.y) * cos(m_Rotation.x);
        return glm::normalize(forward);
    }

    glm::vec3 EditorCameraController::GetRightVector() const
    {
        glm::vec3 forward = GetForwardVector();
        glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f);
        return glm::normalize(glm::cross(forward, worldUp));
    }

    glm::vec3 EditorCameraController::GetUpVector() const
    {
        glm::vec3 forward = GetForwardVector();
        glm::vec3 right = GetRightVector();
        return glm::normalize(glm::cross(right, forward));
    }
}
