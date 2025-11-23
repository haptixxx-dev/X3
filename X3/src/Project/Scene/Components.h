#pragma once

#include "lrpch.h"
#include "Core/GUID.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace Laura
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

		inline glm::vec3 GetRotation() { return glm::degrees(m_Rotation); }
		inline glm::vec3 GetTranslation() { return m_Translation; }
		inline glm::vec3 GetScale() { return m_Scale; }

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
		glm::vec4 color = {1.0f, 1.0f, 1.0f, 1.0f}; // w: padding
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
}