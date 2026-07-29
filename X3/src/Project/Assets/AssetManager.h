#pragma once

#include "lrpch.h"
#include <array>
#include <filesystem>
#include "Core/GUID.h"
#include "Project/Assets/AssetTypes.h"
#include "Project/Assets/BVHAccel.h"
#include "Project/Assets/MeshUtils.h"

struct aiScene;
struct aiMaterial;
struct aiString;

constexpr const char* SUPPORTED_MESH_FILE_FORMATS[]		= { ".fbx", ".obj" ,".gltf", ".glb" };
constexpr const char* SUPPORTED_TEXTURE_FILE_FORMATS[]	= { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" };

namespace X3
{
	// ============================================================================
	// PRIMITIVE MESH GUIDS
	// ----------------------------------------------------------------------------
	// Well-known GUIDs for built-in primitive meshes.
	// These are generated once on startup and always available.
	// ============================================================================
	namespace PrimitiveMeshGUIDs {
		// Use fixed values derived from simple hash-like numbers
		constexpr uint64_t CUBE     = 0x0000000000000001ULL;
		constexpr uint64_t SPHERE   = 0x0000000000000002ULL;
		constexpr uint64_t PLANE    = 0x0000000000000003ULL;
		constexpr uint64_t CYLINDER = 0x0000000000000004ULL;
		constexpr uint64_t CAPSULE  = 0x0000000000000005ULL;
		constexpr uint64_t CONE     = 0x0000000000000006ULL;
	}

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
	/// Decoded pixels for one texture asset, keyed by GUID in the AssetPool.
	///
	/// This replaces the single flat `std::vector<unsigned char> TextureBuffer`
	/// that every texture used to be appended into with a `texStartIdx` offset.
	/// One 4K RGBA8 albedo is 64 MB; a handful of them made every subsequent
	/// insert() copy hundreds of megabytes, and a texture could never be freed
	/// without invalidating every offset after it.
	struct TexturePixels {
		std::vector<unsigned char> data;
		int32_t width = 0, height = 0, channels = 0;
		bool    isSRGB = true;
	};

	struct AssetPool {
	public:
		/// Maps GUIDs to their associated metadata and optional metadata extension.
		std::unordered_map<LR_GUID, MetadataPair> Metadata; // (polymorphic type)

		// --- Mesh data. TriPositionBuffer and TriRefBuffer are appended in
		// lockstep and are always the same length: entry i of one describes the
		// same triangle as entry i of the other. Positions are de-referenced for
		// the BVH; attributes are indexed. See Gpu::TrianglePositions.
		std::vector<Gpu::TrianglePositions> TriPositionBuffer;
		std::vector<Gpu::TriRef>            TriRefBuffer;
		std::vector<Gpu::Vertex>            VertexBuffer;

		/// Permutation produced by the BVH build: indirection between
		/// BVHAccel::Node's triangle range and TriPositionBuffer. Named
		/// BvhPrimIndexBuffer rather than IndexBuffer so it is not confused with
		/// a mesh index buffer, which is what TriRefBuffer's i0/i1/i2 are.
		std::vector<uint32_t> BvhPrimIndexBuffer;
		std::vector<BVHAccel::Node> NodeBuffer;

		std::unordered_map<LR_GUID, TexturePixels> Textures;

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
			TriPositionBuffer,
			TriRefBuffer,
			VertexBuffer,
			BvhPrimIndexBuffer,
			NodeBuffer,
			Textures,
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

		/// Creates built-in primitive meshes (cube, sphere, plane, etc.)
		/// Called automatically on construction. Can be called again to recreate if needed.
		void CreatePrimitiveMeshes();

		/// Check if a GUID refers to a primitive mesh
		static bool IsPrimitiveMesh(LR_GUID guid);

		/// Get the display name for a primitive mesh GUID
		static const char* GetPrimitiveMeshName(LR_GUID guid);

	private:
		std::shared_ptr<AssetPool> m_AssetPool;

		/// Internal: Dispatches to the appropriate asset loader using the file extension.
		/// The given GUID is used to identify the asset in the AssetPool.
		bool LoadAssetFile(const std::filesystem::path& assetpath, LR_GUID guid);

		// Loaders
		bool LoadMesh(const std::filesystem::path& assetpath, LR_GUID guid);
		bool LoadTexture(const std::filesystem::path& assetpath, LR_GUID guid,
		                 const int channels = 4, bool isSRGB = true);

		/// Reads one aiMaterial into the authoring-side MaterialDesc, importing
		/// any textures it references (including ones embedded in the model file)
		/// along the way. Texture references come back as GUIDs; the resolve to
		/// GPU table indices happens in Renderer::Parse.
		MaterialDesc ImportMaterial(const aiScene* scene, const aiMaterial* mat,
		                            const std::filesystem::path& modelDir);

		/// Imports a texture referenced from inside a model file, from an embedded
		/// blob or from a path relative to the model. The GUID is derived from the
		/// resolved key rather than random, so re-importing the same model reuses
		/// the same asset and a texture shared by several materials is decoded
		/// once. Returns LR_GUID::INVALID if it could not be read; that is a
		/// warning, never a failed import.
		LR_GUID ResolveModelTexture(const aiScene* scene, const aiString& texPath,
		                            const std::filesystem::path& modelDir, bool isSRGB);

		// Primitive mesh generators
		void CreatePrimitiveMesh(LR_GUID guid,
		                         const std::vector<Gpu::Vertex>& vertices,
		                         const std::vector<Gpu::TriRef>& tris,
		                         const char* name);
	};
} 