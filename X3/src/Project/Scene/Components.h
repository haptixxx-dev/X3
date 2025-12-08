#pragma once

#include "lrpch.h"
#include "Core/GUID.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

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
}