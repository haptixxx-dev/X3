#pragma once

#include "lrpch.h"
#include <array>
#include <filesystem>
#include "Core/GUID.h"
#include "Project/Assets/AssetTypes.h"
#include "Project/Assets/BVHAccel.h"
#include "Project/Assets/MeshUtils.h"

#include <optional>

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
		/// Only set for textures carried inside a model file. Synthetic for
		/// embedded images, which is why they are never given a .lrmeta.
		std::string sourceKey;
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

		/// Permanently removes an asset from the project. DESTRUCTIVE AND WITHOUT
		/// UNDO -- callers must confirm with the user first (the editor's Assets
		/// panel routes this through ConfirmAndExecute()).
		///
		/// - Drops the metadata entry, which is the part that actually sticks:
		///   SaveAssetPoolToFolder() rebuilds every .lrmeta from this map, so an
		///   asset still in the map is re-serialized on the next save no matter
		///   what was deleted from disk.
		/// - For a mesh, COMPACTS the shared pool buffers and rewrites every later
		///   asset's offsets (see CompactMeshOut) so the pool stays self-consistent,
		///   then bumps the matching update versions so the renderer re-uploads.
		/// - For a texture, drops the pixels. Nothing caches a texture by index
		///   across frames -- Renderer resolves GUID -> table slot every frame via
		///   TextureTable -- so no rewrite is needed, only the version bump that
		///   makes the table drop its stale uploads.
		/// - Deletes the .lrmeta sidecar, and the source file itself ONLY when it
		///   lives inside `projectFolder`. Assets are referenced in place rather
		///   than copied in on import (see the TestProject sidecars, which point at
		///   ../SampleModels), so unlinking an out-of-tree source would destroy a
		///   file shared with every other project on the machine.
		///
		/// `projectFolder` is where the .lrmeta sidecars live -- the same folder
		/// SaveAssetPoolToFolder() is given. Passing nothing removes the asset from
		/// memory only and leaves the filesystem alone.
		///
		/// Refuses (returns false, logs) for unknown GUIDs and for the built-in
		/// primitives, which have no file behind them and are only ever recreated
		/// by CreatePrimitiveMeshes() at project open.
		bool RemoveAsset(LR_GUID guid, const std::filesystem::path& projectFolder = {});

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

		// ====================================================================
		// "LOAD COOKED" MODE -- Phase 9's editor-side switch.
		// --------------------------------------------------------------------
		// The plan is explicit that the EDITOR KEEPS THE SOURCE WORKFLOW and the
		// export cooks (decision 11), so this is OFF by default and importing a
		// model still means running assimp. What it buys when it is on: the
		// editor loads exactly the bytes a shipped build would, so a bug that
		// only reproduces against cooked data can be reproduced without doing an
		// export first. Without it, the cooked path is only ever exercised by
		// the thing that has no debugger attached.
		//
		// SWITCHED BY THE ENVIRONMENT, `X3_LOAD_COOKED=1`, with SetLoadCookedMode
		// as the programmatic override for a future editor menu item. An env var
		// rather than a project setting or a command-line flag because:
		//   - it must be settable for a run that is ALREADY failing, without
		//     editing (and thereby dirtying) the project file being debugged;
		//   - the editor already takes X3_OPEN_PROJECT the same way, so a
		//     reproduction recipe is one line and looks like the ones already
		//     written down;
		//   - it is per-process, so one shell can run the cooked editor and
		//     another the source editor against the same project at the same
		//     time, which is how a cook-only difference is actually bisected.
		//
		// It is a MODE, not a format switch: a cooked file that is missing,
		// stale, corrupt or semantically bad falls back to the importer and logs
		// why. Turning this on can therefore never stop a project from opening.
		// ====================================================================

		/// True when mesh import should prefer a fresh cooked sibling.
		///
		/// Reads the env var once, on first call, and caches it. Callable from
		/// job-system workers -- LoadAssetPoolFromFolder's parallel decode asks
		/// this on every thread.
		static bool LoadCookedModeEnabled();

		/// Programmatic override, for the editor UI. Takes effect on the next
		/// import; nothing already merged is reloaded.
		static void SetLoadCookedMode(bool enabled);

		/// Where a cooked sibling for `source` is expected to live:
		/// "Bistro.glb" -> "Bistro.glb.x3mesh".
		///
		/// APPENDED, not substituted. `replace_extension` would map both
		/// "chair.obj" and "chair.fbx" onto one "chair.x3mesh", so importing one
		/// would silently serve the other's geometry -- and a mesh that is
		/// plausible but wrong is the worst outcome this whole path can produce.
		/// The stamp's recorded filename catches that too, but only after it has
		/// already happened; the naming rule prevents it.
		static std::filesystem::path CookedSiblingPath(const std::filesystem::path& source);

		/// Check if a GUID refers to a primitive mesh
		static bool IsPrimitiveMesh(LR_GUID guid);

		/// Get the display name for a primitive mesh GUID
		static const char* GetPrimitiveMeshName(LR_GUID guid);

	private:
		/// One decoded mesh asset, with every index MESH-LOCAL. Produced by
		/// DecodeMesh, consumed by MergeMesh.
		///
		/// This split is what lets asset loading run on the job system: decoding
		/// (assimp import, attribute build, BVH construction, texture decode) is
		/// the expensive part and touches nothing shared, while merging is cheap
		/// and is the only writer of the AssetPool. Keeping merge serial means the
		/// pool needs no locking AND the resulting buffer layout is deterministic
		/// regardless of the order the parallel decodes happened to finish in.
		struct MeshImportResult {
			std::vector<Gpu::Vertex>            vertices;
			std::vector<Gpu::TriRef>            triRefs;      // mesh-local vertex indices
			std::vector<Gpu::TrianglePositions> triPositions; // lockstep with triRefs
			std::vector<BVHAccel::Node>         nodes;
			std::vector<uint32_t>               primIndex;
			uint32_t                            nodeCount = 0;

			std::vector<SubmeshInfo>  submeshes;
			std::vector<MaterialDesc> importedMaterials;
			/// Textures found inside the model file, keyed by their derived GUID.
			std::unordered_map<LR_GUID, TexturePixels> textures;

			std::filesystem::path sourcePath;
			uintmax_t fileSizeInBytes = 0;
			double    decodeTimeMs = 0.0;
		};

		std::shared_ptr<AssetPool> m_AssetPool;

		/// Internal: Dispatches to the appropriate asset loader using the file extension.
		/// The given GUID is used to identify the asset in the AssetPool.
		bool LoadAssetFile(const std::filesystem::path& assetpath, LR_GUID guid);

		// Loaders
		bool LoadMesh(const std::filesystem::path& assetpath, LR_GUID guid);

		/// The parallel-safe half of mesh import. Touches nothing this object
		/// owns, so many of these run concurrently on the job system.
		std::optional<MeshImportResult> DecodeMesh(const std::filesystem::path& assetpath);

		/// THE SINGLE MESH DECODE ENTRY POINT. Every path that produces a mesh
		/// goes through here -- LoadMesh for a one-off import, and
		/// LoadAssetPoolFromFolder's parallel pass for a project open -- so there
		/// is exactly one place that decides between cooked and imported. A
		/// second such decision is how the two paths drift apart.
		///
		/// `assetpath` may be a source model or a .x3mesh. For a source model in
		/// "load cooked" mode, a PROVABLY FRESH cooked sibling wins; anything
		/// else falls back to DecodeMesh with a log line naming the reason.
		/// Parallel-safe for the same reason DecodeMesh is.
		std::optional<MeshImportResult> DecodeMeshAsset(const std::filesystem::path& assetpath);

		/// Read a .x3mesh into the SAME shape DecodeMesh produces, so MergeMesh
		/// consumes the two identically and there is no second merge path.
		///
		/// `sourcePath` is what the resulting MeshMetadataExtension records, and
		/// it is the SOURCE MODEL rather than the cooked file whenever there is
		/// one: SaveAssetPoolToFolder writes the .lrmeta from that field, so
		/// recording the .x3mesh would rewrite the project to point at the cache
		/// instead of the asset -- and the next open would then have nothing to
		/// check freshness against, permanently. Pass the cooked path itself only
		/// when there is genuinely no source (a cooked-only project).
		///
		/// Returns std::nullopt on ANY problem, having logged it. Never asserts:
		/// the file is untrusted input.
		std::optional<MeshImportResult> DecodeCookedMesh(const std::filesystem::path& cookedPath,
		                                                const std::filesystem::path& sourcePath);

		/// The serial half. The ONLY writer of the AssetPool's mesh buffers.
		bool MergeMesh(MeshImportResult& result, LR_GUID guid);

		/// The inverse of MergeMesh: cuts one mesh's slice out of the five shared
		/// pool buffers and repairs everything that pointed past it.
		///
		/// MergeMesh only ever appends, so import needs exactly ONE rebase (TriRef
		/// vertex indices). Removal is the hard direction: every asset merged AFTER
		/// this one sits at a lower offset once the hole closes, so both the
		/// surviving TriRefs and every later MeshMetadata have to be rewritten.
		/// `guid` is the mesh being removed and is skipped by that rewrite; its
		/// Metadata entry is erased by the caller afterwards.
		void CompactMeshOut(const MeshMetadata& mesh, LR_GUID guid);
		bool LoadTexture(const std::filesystem::path& assetpath, LR_GUID guid,
		                 const int channels = 4, bool isSRGB = true);

		/// Reads one aiMaterial into the authoring-side MaterialDesc, importing
		/// any textures it references (including ones embedded in the model file)
		/// along the way. Texture references come back as GUIDs; the resolve to
		/// GPU table indices happens in Renderer::Parse.
		/// Textures are written into `out` rather than into the AssetPool, so this
		/// stays callable from a decode running off the main thread.
		MaterialDesc ImportMaterial(const aiScene* scene, const aiMaterial* mat,
		                            const std::filesystem::path& modelDir,
		                            std::unordered_map<LR_GUID, TexturePixels>& out);

		/// Imports a texture referenced from inside a model file, from an embedded
		/// blob or from a path relative to the model. The GUID is derived from the
		/// resolved key rather than random, so re-importing the same model reuses
		/// the same asset and a texture shared by several materials is decoded
		/// once. Returns LR_GUID::INVALID if it could not be read; that is a
		/// warning, never a failed import.
		LR_GUID ResolveModelTexture(const aiScene* scene, const aiString& texPath,
		                            const std::filesystem::path& modelDir, bool isSRGB,
		                            std::unordered_map<LR_GUID, TexturePixels>& out);

		// Primitive mesh generators
		void CreatePrimitiveMesh(LR_GUID guid,
		                         const std::vector<Gpu::Vertex>& vertices,
		                         const std::vector<Gpu::TriRef>& tris,
		                         const char* name);
	};
} 