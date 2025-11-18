#pragma once

#include "x3/Math.h"

namespace x3 {

class Camera {
public:
    Vector3 position;
    Vector3 front;
    Vector3 up;
    Vector3 right;
    Vector3 worldUp;

    float yaw;
    float pitch;
    float movementSpeed;
    float mouseSensitivity;
    float fov;

    Camera(Vector3 startPos = Vector3(0.0f, 0.0f, 3.0f), Vector3 startUp = Vector3(0.0f, 1.0f, 0.0f), float startYaw = -90.0f, float startPitch = 0.0f) 
        : front(Vector3(0.0f, 0.0f, -1.0f)), movementSpeed(2.5f), mouseSensitivity(0.1f), fov(45.0f) {
        position = startPos;
        worldUp = startUp;
        yaw = startYaw;
        pitch = startPitch;
        UpdateCameraVectors();
    }

    Matrix4 GetViewMatrix() {
        return Matrix4::LookAt(position, position + front, up);
    }

    void ProcessKeyboard(int direction, float deltaTime) {
        float velocity = movementSpeed * deltaTime;
        if (direction == 0) // FORWARD
            position += front * velocity;
        if (direction == 1) // BACKWARD
            position -= front * velocity;
        if (direction == 2) // LEFT
            position -= right * velocity;
        if (direction == 3) // RIGHT
            position += right * velocity;
    }

    void ProcessMouseMovement(float xoffset, float yoffset, bool constrainPitch = true) {
        xoffset *= mouseSensitivity;
        yoffset *= mouseSensitivity;

        yaw += xoffset;
        pitch -= yoffset;

        if (constrainPitch) {
            if (pitch > 89.0f) pitch = 89.0f;
            if (pitch < -89.0f) pitch = -89.0f;
        }

        UpdateCameraVectors();
    }

private:
    void UpdateCameraVectors() {
        Vector3 newFront;
        newFront.x = std::cos(yaw * (M_PI / 180.0f)) * std::cos(pitch * (M_PI / 180.0f));
        newFront.y = std::sin(pitch * (M_PI / 180.0f));
        newFront.z = std::sin(yaw * (M_PI / 180.0f)) * std::cos(pitch * (M_PI / 180.0f));
        front = newFront.Normalized();
        right = Vector3::Cross(front, worldUp).Normalized();
        up = Vector3::Cross(right, front).Normalized();
    }
};

} // namespace x3
