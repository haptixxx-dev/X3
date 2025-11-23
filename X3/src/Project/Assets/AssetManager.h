#pragma once

#include "lrpch.h"
#include <array>
#include <filesystem>
#include "Core/GUID.h"
#include "Project/Assets/AssetTypes.h"
#include "Project/Assets/BVHAccel.h"

constexpr const char* SUPPORTED_MESH_FILE_FORMATS[]		= { ".fbx", ".obj" ,".gltf", ".glb" };
constexpr const char* SUPPORTED_TEXTURE_FILE_FORMATS[]	= { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" };

namespace Laura
{

	// ============================================================================
	// ASSET POOL
	// ----------------------------------------------------------------------------
	// Centralized storage for all loaded asset data within a project.
	// Owned internally by the AssetManager and passed to renderer.
	// ============================================================================
	using MetadataPair = std::pair<
		std::shared_ptr<Metadata>, 
		std::shared_ptr<MetadataExtension>
	>;
	struct AssetPool { 
	public:
		/// Maps GUIDs to their associated metadata and optional metadata extension.
		std::unordered_map<LR_GUID, MetadataPair> Metadata; // (polymorphic type)
		std::vector<Triangle> MeshBuffer;
		std::vector<uint32_t> IndexBuffer; // indirection between BVHAccel::Node and Triangles in AssetPool::MeshBuffer
		std::vector<BVHAccel::Node> NodeBuffer;
		std::vector<unsigned char> TextureBuffer;

		template <typename T>
		std::shared_ptr<T> find(const LR_GUID& guid) const {
			auto it = Metadata.find(guid);
			if (it == Metadata.end()) {
				return nullptr;
			}
			const auto& [metadata, metadataExtension] = it->second;
			return std::dynamic_pointer_cast<T>(metadata);
		}

		// Versioning system to track buffer updates across multiple listeners (e.g., renderer).
		// Listeners compare a static `lastUpdateId` against `GetUpdateVersion()` to detect changes.
		enum struct AssetType {
			Metadata,
			MeshBuffer,
			IndexBuffer,
			NodeBuffer,
			TextureBuffer,
			COUNT
		};
		inline void MarkUpdated(AssetType type) { m_UpdateVersions[static_cast<size_t>(type)]++; }
		inline uint32_t GetUpdateVersion(AssetType type) const { return m_UpdateVersions[static_cast<size_t>(type)]; }
	private:
		std::array<uint32_t, static_cast<size_t>(AssetType::COUNT)> m_UpdateVersions = {}; // initialize with 0s
	};
	
	


	// ============================================================================
	// ASSET META FILE (.lrmeta)
	// ----------------------------------------------------------------------------
	// Provides serialization and deserialization of asset metadata.
	// Used internally by the AssetManager to persist asset identity (GUIDs).
	// ============================================================================
	#define ASSET_META_FILE_EXTENSION ".lrmeta"

	struct AssetMetaFile {
		AssetMetaFile(LR_GUID guid = LR_GUID::INVALID, std::filesystem::path sourcePath = "")
			: guid(guid), sourcePath(std::move(sourcePath)) {
		}

		LR_GUID guid = LR_GUID::INVALID;
		std::filesystem::path sourcePath;
	};

	/// Serialize the 'assetMetafile' as-is at the location 'metapath'.
	/// Returns true on success.
	bool SaveMetaFile(const std::filesystem::path& metapath, const AssetMetaFile& assetMetafile);

	/// Deserialize from 'metapath' and return 'AssetMetaFile'.
	/// Returns std::nullopt if unsuccessful.
	std::optional<AssetMetaFile> LoadMetaFile(const std::filesystem::path& metapath);




	// ============================================================================
	// ASSET MANAGER
	// ----------------------------------------------------------------------------
	// High-level system responsible for loading, importing, and managing assets.
	// Owns the AssetPool and handles metadata persistence (.lrmeta).
	// ============================================================================
	class AssetManager {
	public:
		AssetManager();
		~AssetManager() = default;

		/// Imports a new asset file and adds it to the project.
		/// - Assigns a new GUID.
		/// - Saves the .lrmeta file.
		/// - Loads the asset.
		/// - Returns the new asset LR_GUID on success
		/// - Returns LR_GUID::INVALID if unsuccessful
		LR_GUID ImportAsset(const std::filesystem::path& assetpath);
		bool RemoveAsset(LR_GUID guid);

		/// Writes current metadata (not asset files) back into .lrmeta files.
		/// Removes orphaned .lrmeta files that no longer have corresponding assets.
		/// Logs warnings/errors but never throws or fails.
		void SaveAssetPoolToFolder(const std::filesystem::path& folderpath) const;

		/// Loads assets with their .lrmeta files in the folder.
		/// Skips and warns if matching asset files are missing.
		/// Populates the AssetPool and loads as many assets as possible.
		void LoadAssetPoolFromFolder(const std::filesystem::path& folderpath);

		inline std::shared_ptr<const AssetPool> GetAssetPool() const { return m_AssetPool; }

	private:
		std::shared_ptr<AssetPool> m_AssetPool;

		/// Internal: Dispatches to the appropriate asset loader using the file extension.
		/// The given GUID is used to identify the asset in the AssetPool.
		bool LoadAssetFile(const std::filesystem::path& assetpath, LR_GUID guid);

		// Loaders
		bool LoadMesh(const std::filesystem::path& assetpath, LR_GUID guid);
		bool LoadTexture(const std::filesystem::path& assetpath, LR_GUID guid, const int channels = 4);
	};
} 