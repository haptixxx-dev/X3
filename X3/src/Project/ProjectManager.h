#pragma once 

#include "lrpch.h"
#include "Renderer/RenderSettings.h"
#include "Core/GUID.h"

// Forward declarations
namespace X3 {
    class SceneManager;
    class AssetManager;
}

namespace X3 
{

	// ============================================================================ 
	// PROJECT FILE (.lrproj)
	// ----------------------------------------------------------------------------
	// Provides serialization and deserialization of project settings.
	// Used internally by the ProjectManager to persist global project data.
	// ============================================================================
	#define PROJECT_FILE_EXTENSION ".lrproj"

	struct ProjectFile {
		LR_GUID bootSceneGuid = LR_GUID::INVALID;
		RenderSettings runtimeRenderSettings{};

		/// World gravity handed to the PhysicsWorld when simulation starts.
		/// Project-wide rather than per-scene: Jolt has one gravity per
		/// JPH::PhysicsSystem and PhysicsLayer owns exactly one world for the
		/// whole session, so a per-scene field would have nowhere to apply on a
		/// scene switch mid-simulation.
		/// MUST stay equal to PhysicsWorld::m_Gravity's initializer
		/// (PhysicsWorld.h) -- projects saved before this field existed omit the
		/// key and fall back to this default, and they must keep simulating
		/// exactly as they did.
		glm::vec3 physicsGravity{ 0.0f, -9.81f, 0.0f };

		ProjectFile(LR_GUID bootSceneGuid = LR_GUID::INVALID) : bootSceneGuid(bootSceneGuid) {}
	};
	
	/// Serialize the 'projectFile' as-is at the location 'projectFilepath'.
	/// Returns true on success.
	bool SaveProjectFile(const std::filesystem::path& projectFilepath, const ProjectFile& projectFile);

	/// Deserialize from 'projectFilepath' and return 'ProjectFile'.
	/// Returns std::nullopt if unsuccessful.
	std::optional<ProjectFile> LoadProjectFile(const std::filesystem::path& projectFilepath);

	/// Computes the absolute path to the project file (.lrproj) given the project folder.
	/// Example: Input folder `/MyProject/` -> Output `/MyProject/MyProject.lrproj`
	inline std::filesystem::path ComposeProjectFilepath(const std::filesystem::path& folderpath) {
		if (folderpath.empty()) {
			LOG_ENGINE_ERROR("ComposeProjectFilepath: folderpath is empty");
			return std::filesystem::path{};
		}
		if (!std::filesystem::exists(folderpath)) {
			LOG_ENGINE_ERROR("ComposeProjectFilepath: folderpath does not exist: {}", folderpath.string());
			return std::filesystem::path{};
		}
		std::string folderName = folderpath.filename().string();
		if (folderName.empty()) {
			LOG_ENGINE_ERROR("ComposeProjectFilepath: could not extract folder name from: {}", folderpath.string());
			return std::filesystem::path{};
		}
		return folderpath / (folderName + PROJECT_FILE_EXTENSION);
	}




	// ============================================================================
	// PROJECT MANAGER
	// ----------------------------------------------------------------------------
	// High-level system responsible for creating, opening, saving, and managing
	// the lifetime of a project.
	// Owns the AssetManager and SceneManager used throughout the editor session.
	// ============================================================================
	class ProjectManager {
	public:
		ProjectManager() = default;
		~ProjectManager() = default;

		/// Creates a new project in the given folder.
		/// Initializes the AssetManager and SceneManager.
		/// Saves an initial project file.
		/// Returns true on success.
		bool NewProject(const std::filesystem::path& folderpath);

		/// Opens an existing project from the given folder.
		/// Loads the project file and initializes managers.
		/// Returns true on success.
		bool OpenProject(const std::filesystem::path& projectfilePath);

		/// Saves the current project file (.lrproj) into the project folder.
		/// Returns true on success.
		bool SaveProject();

		/// Shuts down managers and clears project data without saving.
		void CloseProject();

		inline bool ProjectIsOpen() const { return !m_ProjectFolder.empty(); }
		inline std::shared_ptr<SceneManager> GetSceneManager() const { return m_SceneManager; }
		inline std::shared_ptr<AssetManager> GetAssetManager() const { return m_AssetManager; }

		inline void SetBootSceneGuid(LR_GUID guid) { m_ProjectFile.bootSceneGuid = guid; }
		inline LR_GUID GetBootSceneGuid() const { return m_ProjectFile.bootSceneGuid; }
		inline bool IsBootScene(LR_GUID guid) { return m_ProjectFile.bootSceneGuid == guid; }

		inline std::string GetProjectName() { return m_ProjectFolder.filename().string(); }
		inline std::filesystem::path GetProjectFolder() { return m_ProjectFolder; }

		inline RenderSettings& GetMutableRuntimeRenderSettings() { return m_ProjectFile.runtimeRenderSettings; }

		/// Physics world gravity, persisted in the .lrproj.
		/// The mutable overload is what the editor's Physics Settings panel binds
		/// its DragFloat3 to; the const one is what PhysicsLayer reads at
		/// StartSimulation. Writing through the mutable reference only changes the
		/// in-memory project -- it reaches the running solver via
		/// SetPhysicsGravityEvent and the file via SaveProject(), same as the
		/// runtime render settings above.
		inline glm::vec3& GetMutablePhysicsGravity() { return m_ProjectFile.physicsGravity; }
		inline const glm::vec3& GetPhysicsGravity() const { return m_ProjectFile.physicsGravity; }
	private:
		/// Filesystem path to the current project folder (where .lrproj lives).
		std::filesystem::path m_ProjectFolder;
		ProjectFile m_ProjectFile;

		std::shared_ptr<SceneManager> m_SceneManager = nullptr;
		std::shared_ptr<AssetManager> m_AssetManager = nullptr;
	};
}