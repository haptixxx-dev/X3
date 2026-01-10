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

            // When starting Alt+LMB orbit, calculate focus point from current camera state
            if (pressed && m_AltPressed) {
                glm::vec3 forward = GetForwardVector();
                m_FocusPoint = m_Position + forward * m_DistanceToFocus;
            }
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
        // Shift + scroll: Adjust camera movement speed
        if (m_KeyShift) {
            float speedMultiplier = 1.0f + yoffset * 0.1f;
            MovementSpeed = glm::clamp(MovementSpeed * speedMultiplier, 0.5f, 100.0f);
            return;
        }

        // Normal scroll: Zoom (move camera forward/back)
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
            case 342: m_AltPressed = pressed; break; // Left Alt
            case 346: m_AltPressed = pressed; break; // Right Alt
        }
    }

    glm::mat4 EditorCameraController::GetViewMatrix() const
    {
        // View matrix is the inverse of transform matrix
        return glm::inverse(GetTransformMatrix());
    }

    glm::mat4 EditorCameraController::GetTransformMatrix() const
    {
        // Build transform: translate to position, then apply yaw (Y), then pitch (X)
        // This matches the engine's convention where +Z is forward
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), m_Position);
        glm::mat4 rotationY = glm::rotate(glm::mat4(1.0f), m_Rotation.y, glm::vec3(0.0f, 1.0f, 0.0f)); // Yaw
        glm::mat4 rotationX = glm::rotate(glm::mat4(1.0f), m_Rotation.x, glm::vec3(1.0f, 0.0f, 0.0f)); // Pitch
        return translation * rotationY * rotationX;
    }

    glm::vec3 EditorCameraController::GetForwardVector() const
    {
        // Extract forward (Z-axis) directly from rotation matrix for consistency
        glm::mat4 rotationY = glm::rotate(glm::mat4(1.0f), m_Rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 rotationX = glm::rotate(glm::mat4(1.0f), m_Rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
        glm::mat4 rotation = rotationY * rotationX;
        return glm::normalize(glm::vec3(rotation[2])); // Z column = forward
    }

    glm::vec3 EditorCameraController::GetRightVector() const
    {
        // Extract right (X-axis) directly from rotation matrix for consistency
        glm::mat4 rotationY = glm::rotate(glm::mat4(1.0f), m_Rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 rotationX = glm::rotate(glm::mat4(1.0f), m_Rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
        glm::mat4 rotation = rotationY * rotationX;
        return glm::normalize(glm::vec3(rotation[0])); // X column = right
    }

    glm::vec3 EditorCameraController::GetUpVector() const
    {
        // Extract up (Y-axis) directly from rotation matrix for consistency
        glm::mat4 rotationY = glm::rotate(glm::mat4(1.0f), m_Rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 rotationX = glm::rotate(glm::mat4(1.0f), m_Rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
        glm::mat4 rotation = rotationY * rotationX;
        return glm::normalize(glm::vec3(rotation[1])); // Y column = up
    }
}
