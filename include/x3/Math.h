#pragma once

#include <cmath>
#include <algorithm>

namespace x3 {

struct Vector3 {
    float x, y, z;

    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vector3 operator+(const Vector3& other) const { return {x + other.x, y + other.y, z + other.z}; }
    Vector3 operator-(const Vector3& other) const { return {x - other.x, y - other.y, z - other.z}; }
    Vector3 operator*(float scalar) const { return {x * scalar, y * scalar, z * scalar}; }
    
    Vector3& operator+=(const Vector3& other) { x += other.x; y += other.y; z += other.z; return *this; }
    Vector3& operator-=(const Vector3& other) { x -= other.x; y -= other.y; z -= other.z; return *this; }

    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    
    Vector3 Normalized() const {
        float len = Length();
        if (len > 0) return {x / len, y / len, z / len};
        return {0, 0, 0};
    }

    static float Dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    
    static Vector3 Cross(const Vector3& a, const Vector3& b) {
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }
};

struct Matrix4 {
    float m[4][4];

    static Matrix4 Identity() {
        Matrix4 res = {};
        res.m[0][0] = 1; res.m[1][1] = 1; res.m[2][2] = 1; res.m[3][3] = 1;
        return res;
    }

    static Matrix4 Perspective(float fov, float aspect, float near, float far) {
        Matrix4 res = {};
        float tanHalfFov = std::tan(fov / 2.0f);
        res.m[0][0] = 1.0f / (aspect * tanHalfFov);
        res.m[1][1] = 1.0f / tanHalfFov;
        res.m[2][2] = -far / (far - near);
        res.m[2][3] = -1.0f;
        res.m[3][2] = -(far * near) / (far - near);
        return res;
    }

    static Matrix4 LookAt(const Vector3& eye, const Vector3& center, const Vector3& up) {
        Vector3 f = (center - eye).Normalized();
        Vector3 s = Vector3::Cross(f, up).Normalized();
        Vector3 u = Vector3::Cross(s, f);

        Matrix4 res = Identity();
        res.m[0][0] = s.x; res.m[1][0] = s.y; res.m[2][0] = s.z;
        res.m[0][1] = u.x; res.m[1][1] = u.y; res.m[2][1] = u.z;
        res.m[0][2] = -f.x; res.m[1][2] = -f.y; res.m[2][2] = -f.z;
        res.m[3][0] = -Vector3::Dot(s, eye);
        res.m[3][1] = -Vector3::Dot(u, eye);
        res.m[3][2] = Vector3::Dot(f, eye);
        return res;
    }

    Vector3 Multiply(const Vector3& v) const {
        float x = v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0] + m[3][0];
        float y = v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1] + m[3][1];
        float z = v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2] + m[3][2];
        float w = v.x * m[0][3] + v.y * m[1][3] + v.z * m[2][3] + m[3][3];
        
        if (w != 0.0f) {
            return {x / w, y / w, z / w};
        }
        return {x, y, z};
    }
};

} // namespace x3
