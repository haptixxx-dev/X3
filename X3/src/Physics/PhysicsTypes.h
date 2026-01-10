#pragma once

#include "lrpch.h"
#include <glm/glm.hpp>
#include <entt/entt.hpp>

// Jolt includes
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

namespace X3
{
	// Conversion helpers between GLM and Jolt types
	inline JPH::Vec3 ToJolt(const glm::vec3& v) {
		return JPH::Vec3(v.x, v.y, v.z);
	}

	inline JPH::Quat ToJolt(const glm::quat& q) {
		return JPH::Quat(q.x, q.y, q.z, q.w);
	}

	inline JPH::Mat44 ToJolt(const glm::mat4& m) {
		return JPH::Mat44(
			JPH::Vec4(m[0][0], m[0][1], m[0][2], m[0][3]),
			JPH::Vec4(m[1][0], m[1][1], m[1][2], m[1][3]),
			JPH::Vec4(m[2][0], m[2][1], m[2][2], m[2][3]),
			JPH::Vec4(m[3][0], m[3][1], m[3][2], m[3][3])
		);
	}

	inline glm::vec3 FromJolt(const JPH::Vec3& v) {
		return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
	}

	inline glm::quat FromJolt(const JPH::Quat& q) {
		return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
	}

	inline glm::mat4 FromJolt(const JPH::Mat44& m) {
		glm::mat4 result;
		for (int col = 0; col < 4; col++) {
			JPH::Vec4 c = m.GetColumn4(col);
			result[col] = glm::vec4(c.GetX(), c.GetY(), c.GetZ(), c.GetW());
		}
		return result;
	}

	// Raycast result structure
	struct RaycastHit {
		bool hit = false;
		entt::entity entity = entt::null;
		glm::vec3 point = glm::vec3(0.0f);
		glm::vec3 normal = glm::vec3(0.0f);
		float distance = 0.0f;
	};

	// Contact information for collision events
	struct ContactInfo {
		entt::entity entityA = entt::null;
		entt::entity entityB = entt::null;
		glm::vec3 contactPoint = glm::vec3(0.0f);
		glm::vec3 contactNormal = glm::vec3(0.0f);
		float penetrationDepth = 0.0f;
	};
}
