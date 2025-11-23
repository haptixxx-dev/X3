#pragma once

#include <yaml-cpp/yaml.h>
#include "entt/entt.hpp"
#include "Project/Scene/Entity.h"
#include "Project/Scene/Components.h"

namespace Laura
{

	// ============================================================================
	// SCENE FILE (.lrscene)
	// ----------------------------------------------------------------------------
	// Represents a scene containing entities, components, and metadata.
	// Serialization and deserialization handled by free functions.
	// ============================================================================
	
	#define SCENE_FILE_EXTENSION ".lrscn"

	class Scene {
	public:
		explicit Scene() {
			m_Registry = new entt::registry();
		}

		~Scene() {
			delete m_Registry;
		}

		// non movable, non copyable
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		EntityHandle CreateEntity(const std::string& name = "Empty Entity");
		EntityHandle CreateEntityWithGuid(LR_GUID guid, const std::string& name);

		void DestroyEntity(EntityHandle entity);

		void OnStart();
		void OnUpdate();
		void OnShutdown();

		static std::shared_ptr<Scene> Copy(std::shared_ptr<Scene> other);

		inline entt::registry* GetRegistry() const { return m_Registry; }

		LR_GUID		guid;
		std::string name;

		LR_GUID		skyboxGuid;
		std::string skyboxName;

	private:
		entt::registry* m_Registry;

		friend bool SaveSceneFile(const std::filesystem::path& scenepath, std::shared_ptr<const Scene> scene);
		friend std::shared_ptr<Scene> LoadSceneFile(const std::filesystem::path& scenepath);
	};


	// ============================================================================
	// SERIALIZATION / DESERIALIZATION
	// ============================================================================

	/// Saves scene data to 'scenepath' (full path with filename and extension, e.g. "c:/dev/scene.lrscn").
	/// The file will be created or overwritten; it does not need to exist beforehand.
	/// The parent directory must exist; this function does not create directories.
	/// Returns true on success, false otherwise.
	bool SaveSceneFile(const std::filesystem::path& scenepath, std::shared_ptr<const Scene> scene);

	/// Loads a scene from the scene file at 'scenepath'.
	/// 'sceneFilePath' must be the full path including filename and extension.
	/// Returns a shared_ptr to the Scene or nullptr on failure.
	std::shared_ptr<Scene> LoadSceneFile(const std::filesystem::path& scenepath);
}