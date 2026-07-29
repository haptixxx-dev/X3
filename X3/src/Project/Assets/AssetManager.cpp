#include "Project/Assets/AssetManager.h"
#include "Project/Assets/AssetCook.h"
#include "Project/ProjectUtilities.h"
#include "Core/JobSystem.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stb_image/stb_image.h>
#include <yaml-cpp/yaml.h>
#include <cctype>
#include <cstdlib>
#include <optional>

namespace X3
{

	// META FILE ------------------------------------------------------------------------------
    bool SaveMetaFile(const std::filesystem::path& metafilePath, const AssetMetaFile& assetMetafile) {
		if (!(metafilePath.has_extension() && metafilePath.extension() == ASSET_META_FILE_EXTENSION)) {
			LOG_ENGINE_WARN("SaveMetaFile: invalid file extension '{}'.", metafilePath.string());
			return false;
		}

		if (!std::filesystem::exists(metafilePath.parent_path())) {
			LOG_ENGINE_WARN("SaveMetaFile: parent directory '{}' does not exist.", metafilePath.parent_path().string());
			return false;
		}

        YAML::Emitter out;
        out << YAML::BeginMap 
            << YAML::Key << "Guid" << YAML::Value << (uint64_t)assetMetafile.guid 
			<< YAML::Key << "SourcePath" << YAML::Value << assetMetafile.sourcePath.string()
            << YAML::EndMap;

		std::ofstream fout(metafilePath);
		if (!fout.is_open()) {
			LOG_ENGINE_ERROR("SaveMetaFile: could not open {0} for writing - permissions or path invalid", metafilePath.string());
			return false;
		}
		fout << out.c_str();
		LOG_ENGINE_INFO("SaveMetaFile: wrote metadata for GUID {0}", (uint64_t)assetMetafile.guid);
		return true;
    }


    std::optional<AssetMetaFile> LoadMetaFile(const std::filesystem::path& metafilePath) {
		if (!(std::filesystem::exists(metafilePath) && std::filesystem::is_regular_file(metafilePath) && 
			metafilePath.has_extension() && metafilePath.extension() == ASSET_META_FILE_EXTENSION))
		{
			LOG_ENGINE_WARN("LoadMetaFile: invalid or missing meta file: {0}", metafilePath.string());
			return std::nullopt;
		}

        YAML::Node root;
        try {
            root = YAML::LoadFile(metafilePath.string());
            AssetMetaFile metafile;
            metafile.guid = (LR_GUID)root["Guid"].as<uint64_t>();
			metafile.sourcePath = std::filesystem::path{root["SourcePath"].as<std::string>()};

			LOG_ENGINE_INFO("LoadMetaFile: loaded metadata for GUID {0}", (uint64_t)metafile.guid);
            return std::make_optional(metafile);
        }
        catch (const std::exception& e) {
			LOG_ENGINE_WARN("LoadMetaFile: failed to load {0}: {1}", metafilePath.string(), e.what());
	        return std::nullopt;
        }
    }


     

    // ASSET MANAGER ---------------------------------------------------------------------------
    AssetManager::AssetManager()
        : m_AssetPool(std::make_shared<AssetPool>()) {
    }


    LR_GUID AssetManager::ImportAsset(const std::filesystem::path& assetpath) {
        if (!std::filesystem::exists(assetpath) || std::filesystem::is_directory(assetpath)) {
			LOG_ENGINE_WARN("ImportAsset: invalid asset path: {0}", assetpath.string());
            return LR_GUID::INVALID;
        }

        LR_GUID guid;
        if (!LoadAssetFile(assetpath, guid)) {
			LOG_ENGINE_WARN("ImportAsset: failed to load asset after saving metafile, removed metafile {0}", assetpath.string());
            return LR_GUID::INVALID;
        }

		LOG_ENGINE_INFO("ImportAsset: successfully imported asset {0} with GUID {1}", assetpath.string(), (uint64_t)guid);
        return guid;
    }


	namespace {
		/// True when `file` resolves to somewhere at or under `folder`.
		///
		/// relative() is the portable way to ask, because it returns a path whose
		/// first component is ".." exactly when it had to climb out of `folder`,
		/// and an empty path when the two share no root at all (different drives on
		/// Windows). Comparing the strings is NOT a substitute:
		/// "<proj>/../other/a.png" has the project folder as a prefix and is not
		/// inside it -- and getting that wrong here means unlinking someone else's
		/// file.
		bool IsInsideFolder(const std::filesystem::path& file, const std::filesystem::path& folder) {
			if (folder.empty() || file.empty()) return false;
			std::error_code ec;
			const std::filesystem::path rel = std::filesystem::relative(file, folder, ec);
			if (ec || rel.empty()) return false;
			return rel.begin()->string() != "..";
		}

		/// True for the source model formats the importer understands. NOT true
		/// for .x3mesh -- a cooked file is a mesh asset but not a source, and the
		/// two take different decoders. One predicate rather than a copy of the
		/// loop at each site, because the same question is asked in three places
		/// and the third copy is how one of them ends up missing a format.
		bool IsSupportedMeshSource(const std::string& extension) {
			for (const auto& fmt : SUPPORTED_MESH_FILE_FORMATS)
				if (extension == fmt) return true;
			return false;
		}

		bool IsCookedMeshExtension(const std::string& extension) {
			return extension == COOKED_MESH_FILE_EXTENSION;
		}
	}


	bool AssetManager::RemoveAsset(LR_GUID guid, const std::filesystem::path& projectFolder) {
		if (!m_AssetPool) {
			LOG_ENGINE_CRITICAL("RemoveAsset: called without a valid AssetPool");
			return false;
		}

		// THE PRIMITIVES ARE NOT ASSETS IN THE REMOVABLE SENSE. They have no file
		// behind them and are only ever produced by CreatePrimitiveMeshes() at
		// project open, so removing one cannot be undone from the UI and would
		// leave every MeshComponent naming it pointing at a dead GUID until the
		// project is reopened.
		if (IsPrimitiveMesh(guid)) {
			LOG_ENGINE_WARN("RemoveAsset: refusing to remove built-in primitive '{0}' (GUID {1})",
				GetPrimitiveMeshName(guid), (uint64_t)guid);
			return false;
		}

		auto it = m_AssetPool->Metadata.find(guid);
		if (it == m_AssetPool->Metadata.end()) {
			LOG_ENGINE_WARN("RemoveAsset: no asset with GUID {0} in the pool", (uint64_t)guid);
			return false;
		}

		// COPIES, not references into the map node: the erase below destroys that
		// node, and the source path is still needed afterwards to find the files on
		// disk. Holding the shared_ptrs also keeps the metadata alive across
		// CompactMeshOut, which reads it while rewriting everything around it.
		const std::shared_ptr<Metadata>          metadata          = it->second.first;
		const std::shared_ptr<MetadataExtension> metadataExtension = it->second.second;
		const std::filesystem::path sourcePath =
			metadataExtension ? metadataExtension->sourcePath : std::filesystem::path{};
		const bool ownedByModel = metadataExtension && metadataExtension->ownedByModel;

		if (const auto mesh = std::dynamic_pointer_cast<MeshMetadata>(metadata)) {
			// The expensive, dangerous half. See CompactMeshOut.
			//
			// The textures this model carried are deliberately LEFT IN THE POOL.
			// Their GUIDs are content-addressed (ResolveModelTexture), so the same
			// image shared with another model is the same entry, and a
			// MaterialComponent override on some other entity may name one
			// directly. Dropping them would need a reference count this class does
			// not have -- and the cost of keeping them is memory, whereas the cost
			// of dropping a live one is a material that silently loses its maps.
			CompactMeshOut(*mesh, guid);
		}
		else if (std::dynamic_pointer_cast<TextureMetadata>(metadata)) {
			// TEXTURES ARE NOT OFFSETS INTO ANYTHING, which is what makes this the
			// easy case: TexturePixels are keyed by GUID (the flat TextureBuffer +
			// texStartIdx design is gone -- see the comment on TexturePixels), and
			// the GUID -> GPU table slot resolve happens per frame in
			// TextureTable::resolve(). Nothing holds a slot index across frames, so
			// there is nothing to renumber. A material still naming this GUID falls
			// back to its scalar factor and warns, which is the intended outcome of
			// deleting a texture out from under it.
			m_AssetPool->Textures.erase(guid);
			m_AssetPool->MarkUpdated(AssetPool::AssetType::Textures);
		}

		m_AssetPool->Metadata.erase(guid);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		// THE ERASE ABOVE IS WHAT MAKES THE DELETION STICK, not the unlink below.
		// SaveAssetPoolToFolder() rewrites every .lrmeta from the in-memory
		// metadata map and deletes only the sidecars whose GUID is no longer in it,
		// so an asset removed from disk but left in the pool is simply written back
		// out on the next project save. The filesystem work here is cleanup; the
		// map is the source of truth.
		if (!projectFolder.empty() && !sourcePath.empty() && !ownedByModel) {
			std::error_code ec;

			// Same path SaveAssetPoolToFolder() writes: the sidecar sits in the
			// project folder and is named after the source file, not after the GUID.
			const std::filesystem::path metapath =
				projectFolder / (sourcePath.filename().string() + ASSET_META_FILE_EXTENSION);
			if (std::filesystem::remove(metapath, ec)) {
				LOG_ENGINE_INFO("RemoveAsset: deleted metafile {0}", metapath.string());
			}
			else if (ec) {
				LOG_ENGINE_WARN("RemoveAsset: could not delete metafile {0}: {1}",
					metapath.string(), ec.message());
			}

			// THE SOURCE FILE IS ONLY UNLINKED FROM INSIDE THE PROJECT. Import
			// references an asset where it lies rather than copying it in, so a
			// project's sourcePath routinely points outside the project folder --
			// the shipped TestProject's sidecars say ../SampleModels and
			// ../SampleSkyboxes. Deleting those would destroy files shared with
			// every other project on the machine to satisfy a removal from one of
			// them. The asset still goes away for THIS project, because that is
			// decided by the metadata map, not by the file.
			if (IsInsideFolder(sourcePath, projectFolder)) {
				if (std::filesystem::remove(sourcePath, ec)) {
					LOG_ENGINE_INFO("RemoveAsset: deleted source file {0}", sourcePath.string());
				}
				else if (ec) {
					LOG_ENGINE_WARN("RemoveAsset: could not delete source file {0}: {1}",
						sourcePath.string(), ec.message());
				}
			}
			else {
				LOG_ENGINE_INFO("RemoveAsset: left source file {0} on disk -- it is outside the "
				                "project folder {1} and may be shared with other projects",
					sourcePath.string(), projectFolder.string());
			}
		}

		LOG_ENGINE_INFO("RemoveAsset: removed asset {0} (GUID {1}) from the pool",
			sourcePath.string(), (uint64_t)guid);
		return true;
	}


	void AssetManager::SaveAssetPoolToFolder(const std::filesystem::path& folderpath) const {
		// Delete all existing metafiles which don't have GUID within the asset pool
		for (const auto& metapath : FindFilesInFolder(folderpath, ASSET_META_FILE_EXTENSION)) {
			const auto maybeMetafile = LoadMetaFile(metapath);
			if (!maybeMetafile.has_value()) {
				LOG_ENGINE_WARN("SaveAssetPoolToFolder: unable to read metafile {0}", metapath.string());
				continue;
			}
			LR_GUID guid = maybeMetafile->guid;
			if (m_AssetPool->Metadata.find(guid) == m_AssetPool->Metadata.end()) {
				std::filesystem::remove(metapath);
				LOG_ENGINE_INFO("SaveAssetPoolToFolder: removed stale metafile {0}", metapath.string());
			}
		}

		// Save metafiles for all assets in the asset pool
		for (const auto& [guid, metadataPair] : m_AssetPool->Metadata) {
			const auto& [metadata, metadataExtension] = metadataPair;

			// Textures unpacked from inside a model file are owned by that model,
			// not independent assets. Their sourcePath is synthetic (an embedded
			// image has no file), so a .lrmeta written here would point at
			// nothing and the next LoadAssetPoolFromFolder would warn on every
			// single one. They are re-imported with the model instead.
			if (metadataExtension && metadataExtension->ownedByModel)
				continue;

			if (metadataExtension && std::filesystem::exists(metadataExtension->sourcePath)) {
				// Record the source path relative to the project folder when one can be
				// expressed, so a project folder stays valid when the tree is checked out
				// or moved somewhere else. LoadAssetPoolFromFolder resolves it back.
				// Absolute paths are still written (and still read) when no relative path
				// exists, e.g. a different drive on Windows.
				std::error_code ec;
				std::filesystem::path recordedPath =
					std::filesystem::relative(metadataExtension->sourcePath, folderpath, ec);
				if (ec || recordedPath.empty()) {
					recordedPath = metadataExtension->sourcePath;
				}
				AssetMetaFile metafile{ guid, recordedPath };

				// save .lrmeta in the project root next to .lrproj file with filename same as the original asset + .lrmeta extension
				auto metapath = folderpath / (metadataExtension->sourcePath.filename().string() + ASSET_META_FILE_EXTENSION);
				if (!SaveMetaFile(metapath, metafile)) {
					LOG_ENGINE_WARN("SaveAssetPoolToFolder: failed to save metafile {0}", metapath.string());
				}
				else {
					LOG_ENGINE_INFO("SaveAssetPoolToFolder: saved metafile {0}", metapath.string());
				}
			} else {
				LOG_ENGINE_WARN("SaveAssetPoolToFolder: asset does not exist {0}", metadataExtension->sourcePath.string());
			}
		}
	}


	void AssetManager::LoadAssetPoolFromFolder(const std::filesystem::path& folderpath) {
		// PASS 1: resolve every .lrmeta to a (guid, path) pair. Cheap, serial, and
		// it is what gives the parallel pass below a fixed work list.
		struct PendingAsset {
			LR_GUID guid = LR_GUID::INVALID;
			std::filesystem::path sourcePath;
			bool isMesh = false;
		};
		std::vector<PendingAsset> pending;

		for (const auto& metapath : FindFilesInFolder(folderpath, ASSET_META_FILE_EXTENSION)) {
			auto maybeMetafile = LoadMetaFile(metapath);
			if (!maybeMetafile.has_value()) {
				LOG_ENGINE_WARN("LoadAssetPoolFromFolder: failed to load metafile {0}", metapath.string());
				continue;
			}

			auto sourcePath = maybeMetafile->sourcePath;

			// A relative source path is relative to the project folder, not to the
			// process working directory. Resolve it here so the asset pool always
			// holds absolute paths in memory.
			if (sourcePath.is_relative()) {
				sourcePath = (folderpath / sourcePath).lexically_normal();
			}

			if (!std::filesystem::exists(sourcePath)) {
				LOG_ENGINE_WARN("LoadAssetPoolFromFolder: missing asset file for metafile {0}", metapath.string());
				continue;
			}

			bool isMesh = false;
			if (sourcePath.has_extension()) {
				const std::string ext = sourcePath.extension().string();
				// .x3mesh counts: a sidecar in an exported project names a cooked
				// file, and it must take the same PARALLEL decode path as a source
				// model rather than falling through to the serial LoadAssetFile
				// branch below. Reading and validating a large cooked mesh is not
				// free, and the whole point of the phase is that project open gets
				// faster, not that it moves onto the main thread.
				isMesh = IsSupportedMeshSource(ext) || IsCookedMeshExtension(ext);
			}

			pending.push_back({ maybeMetafile->guid, std::move(sourcePath), isMesh });
		}

		// PASS 2: decode meshes CONCURRENTLY. This is the expensive part -- assimp
		// import, attribute construction and BVH build -- and none of it touches
		// the AssetPool, which is what makes it safe to run off the main thread.
		// Before the job system this was fully serial and blocking, with a BVH
		// rebuild per mesh on every project open.
		std::vector<std::optional<MeshImportResult>> decoded(pending.size());
		JobSystem::ParallelForEach(static_cast<uint32_t>(pending.size()), [&](uint32_t i) {
			if (!pending[i].isMesh) return;
			// DecodeMeshAsset, not DecodeMesh: this is where "load cooked" mode
			// pays for itself, since a project open is exactly the N-BVH-builds
			// case Phase 9 exists to delete. It is parallel-safe for the same
			// reason DecodeMesh is -- it touches no member of this object.
			decoded[i] = DecodeMeshAsset(pending[i].sourcePath);
		});

		// PASS 3: merge, SERIALLY AND IN THE ORIGINAL ORDER. Serial because the
		// AssetPool is single-writer by design; in order because the resulting
		// buffer layout must not depend on which decode happened to finish first.
		// Non-mesh assets take the ordinary loader here, so their cost stays on
		// the main thread -- image decode is a fraction of a mesh import and
		// parallelising it would mean a second staging path for no real gain.
		for (size_t i = 0; i < pending.size(); ++i) {
			const PendingAsset& asset = pending[i];
			bool ok = false;

			if (asset.isMesh) {
				ok = decoded[i].has_value() && MergeMesh(*decoded[i], asset.guid);
			} else {
				ok = LoadAssetFile(asset.sourcePath, asset.guid);
			}

			if (!ok) {
				LOG_ENGINE_WARN("LoadAssetPoolFromFolder: failed to load asset {0}", asset.sourcePath.string());
				continue;
			}
			LOG_ENGINE_INFO("LoadAssetPoolFromFolder: loaded asset {0} with GUID {1}",
				asset.sourcePath.string(), (uint64_t)asset.guid);
		}
	}


	// ============================================================================
	// "LOAD COOKED" MODE. See the block comment on AssetManager::LoadCookedModeEnabled.
	// ============================================================================
	namespace {
		/// -1 = not overridden (use the environment), 0 = off, 1 = on.
		///
		/// ATOMIC, not a plain bool: LoadAssetPoolFromFolder's decode pass reads
		/// this from every job-system worker while the main thread may be
		/// flipping it from a menu. That is a data race on a plain bool -- and
		/// the symptom would be half a project loading cooked and half imported,
		/// which is precisely the state this mode exists to make impossible to
		/// end up in by accident.
		std::atomic<int> g_LoadCookedOverride{ -1 };

		bool ReadLoadCookedEnv() {
			const char* value = std::getenv("X3_LOAD_COOKED");
			if (!value || value[0] == '\0') return false;

			std::string v(value);
			std::transform(v.begin(), v.end(), v.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			// Accept the spellings people actually type. "0" and "false" are
			// listed explicitly rather than falling out of a truthiness test,
			// because `X3_LOAD_COOKED=0` meaning ON is the kind of thing nobody
			// checks and everybody assumes.
			const bool on = (v == "1" || v == "true" || v == "yes" || v == "on");
			if (!on && v != "0" && v != "false" && v != "no" && v != "off") {
				LOG_ENGINE_WARN("X3_LOAD_COOKED is set to '{0}', which is not a recognised value -- "
				                "treating it as off. Use 1 or 0.", value);
			}
			LOG_ENGINE_INFO("X3_LOAD_COOKED={0}: mesh import will {1} a fresh cooked sibling",
				value, on ? "prefer" : "ignore");
			return on;
		}
	}

	bool AssetManager::LoadCookedModeEnabled() {
		const int overridden = g_LoadCookedOverride.load(std::memory_order_relaxed);
		if (overridden >= 0) return overridden != 0;

		// Function-local static: initialised exactly once, thread-safely, on
		// first use. The env var is read ONCE for the process rather than per
		// import, so a project open cannot straddle a change to it -- and the log
		// line above appears once instead of per mesh.
		static const bool fromEnvironment = ReadLoadCookedEnv();
		return fromEnvironment;
	}

	void AssetManager::SetLoadCookedMode(bool enabled) {
		g_LoadCookedOverride.store(enabled ? 1 : 0, std::memory_order_relaxed);
		LOG_ENGINE_INFO("AssetManager: load-cooked mode {0} (overriding X3_LOAD_COOKED)",
			enabled ? "ENABLED" : "disabled");
	}

	std::filesystem::path AssetManager::CookedSiblingPath(const std::filesystem::path& source) {
		std::filesystem::path cooked = source;
		cooked += COOKED_MESH_FILE_EXTENSION;   // see the header for why += and not replace_extension
		return cooked;
	}


	bool AssetManager::LoadAssetFile(const std::filesystem::path& assetpath, LR_GUID guid) {
		if (!std::filesystem::exists(assetpath) || !std::filesystem::is_regular_file(assetpath) || !assetpath.has_extension()) {
			LOG_ENGINE_ERROR("LoadAssetFile: invalid asset path {0}", assetpath.string());
			return false;
		}

		// choose loader based on the extension
		const std::string extension = assetpath.extension().string();
		// A COOKED FILE IS A FIRST-CLASS ASSET, not only a cache beside a source.
		// An exported project contains .x3mesh files and no models at all, so the
		// runtime has to be able to import one directly -- and the editor being
		// able to open one is what makes an exported project inspectable.
		if (IsSupportedMeshSource(extension) || IsCookedMeshExtension(extension)) {
			LOG_ENGINE_INFO("LoadAssetFile: loading mesh {0} for GUID {1}", assetpath.string(), (uint64_t)guid);
			return LoadMesh(assetpath, guid);
		}
		for (const auto& SUPPORTED_FORMAT : SUPPORTED_TEXTURE_FILE_FORMATS) {
			if (extension == SUPPORTED_FORMAT) {
				LOG_ENGINE_INFO("LoadAssetFile: loading texture {0} for GUID {1}", assetpath.string(), (uint64_t)guid);
				return LoadTexture(assetpath, guid, 4);
			}
		}

		LOG_ENGINE_WARN("LoadAssetFile: unsupported file extension {0}", extension);
		return false;
	}


	namespace {
		// assimp stores matrices row-major; glm is column-major. The transpose is
		// the conversion, not an orientation choice.
		inline glm::mat4 ToGlm(const aiMatrix4x4& m) {
			return glm::transpose(glm::mat4(
				m.a1, m.a2, m.a3, m.a4,
				m.b1, m.b2, m.b3, m.b4,
				m.c1, m.c2, m.c3, m.c4,
				m.d1, m.d2, m.d3, m.d4));
		}

		// One aiMesh as referenced by one aiNode, with that node's accumulated
		// world transform. A mesh referenced by two nodes appears twice, which is
		// correct -- those are two instances at two placements.
		struct MeshInstance {
			uint32_t  meshIndex = 0;
			glm::mat4 world{ 1.0f };
		};

		// Walks the node graph accumulating transforms.
		//
		// THE PRE-PHASE-2 IMPORTER DID NOT DO THIS. It iterated scene->mMeshes
		// flat and never touched mRootNode, so any file whose meshes sit under a
		// transformed node imported at the wrong position, orientation and scale.
		// It went unnoticed only because the sample .glb files are single-node.
		//
		// aiProcess_PreTransformVertices would also have fixed it, but it
		// destroys the node graph and merges meshes by material, which fights the
		// per-submesh material design. Fixed directly instead.
		void CollectMeshInstances(const aiNode* node, const glm::mat4& parent,
		                          std::vector<MeshInstance>& out) {
			if (!node) return;
			const glm::mat4 world = parent * ToGlm(node->mTransformation);
			for (unsigned int i = 0; i < node->mNumMeshes; ++i)
				out.push_back({ node->mMeshes[i], world });
			for (unsigned int i = 0; i < node->mNumChildren; ++i)
				CollectMeshInstances(node->mChildren[i], world, out);
		}
	}


	// DECODE. Touches NOTHING the AssetManager owns -- no m_AssetPool, no
	// metadata map, no shared counters -- so several of these run concurrently on
	// the job system. Everything it produces uses MESH-LOCAL indices; MergeMesh
	// rebases them.
	//
	// The BVH is built HERE, in the parallel phase, which is the point: BVH
	// construction dominates mesh import and it was previously synchronous and
	// blocking inside the serial load loop. It needs no rebasing because the BVH
	// data is already entirely mesh-relative -- traversal reaches nodes through
	// entityHandle.rootNodeIdx and primitives through rootTriIdx, so a merge is a
	// plain append.
	std::optional<AssetManager::MeshImportResult>
	AssetManager::DecodeMesh(const std::filesystem::path& assetpath) {
		auto timerStart = std::chrono::high_resolution_clock::now();

		MeshImportResult result;

		Assimp::Importer importer;
		// The preset already supplies everything this importer needs, verified
		// against postprocess.h: CalcTangentSpace, GenSmoothNormals,
		// JoinIdenticalVertices (which is what makes VertexBuffer de-duplicated),
		// Triangulate, GenUVCoords, SortByPType, OptimizeMeshes.
		//
		// Two things it does NOT guarantee, and the fallbacks below exist for:
		// CalcTangentSpace bails and leaves mTangents null when a mesh has no
		// UV0, and GenUVCoords only converts declared non-UV mapping modes -- it
		// does not invent texture coordinates for a mesh authored without them.
		const aiScene* scene = importer.ReadFile(assetpath.string(), aiProcessPreset_TargetRealtime_MaxQuality);
		if (!scene) {
			LOG_ENGINE_CRITICAL("DecodeMesh: failed to load assimp scene from {0}", assetpath.string());
			return std::nullopt;
		}

		std::vector<MeshInstance> instances;
		CollectMeshInstances(scene->mRootNode, glm::mat4(1.0f), instances);
		if (instances.empty()) {
			LOG_ENGINE_WARN("DecodeMesh: {0} has no mesh instances under its node graph", assetpath.string());
			return std::nullopt;
		}

		auto& triPositions = result.triPositions;
		auto& triRefs      = result.triRefs;
		auto& vertices     = result.vertices;

		// --- Pass 1: size everything up front -------------------------------
		size_t totalVerts = 0, totalTris = 0;
		for (const MeshInstance& inst : instances) {
			const aiMesh* m = scene->mMeshes[inst.meshIndex];
			if ((m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) continue;
			totalVerts += m->mNumVertices;
			for (unsigned int f = 0; f < m->mNumFaces; ++f)
				if (m->mFaces[f].mNumIndices == 3) ++totalTris;
		}
		vertices.reserve(vertices.size() + totalVerts);
		triPositions.reserve(triPositions.size() + totalTris);
		triRefs.reserve(triRefs.size() + totalTris);

		// --- Pass 2: dense material slot table ------------------------------
		// aiMaterial indices are scene-global and sparse; the GPU wants a dense
		// per-mesh slot so TriRef::materialSlot stays small and
		// MeshEntityHandle::materialSlotCount bounds it.
		std::unordered_map<uint32_t, uint32_t> materialSlotOf;
		const std::filesystem::path modelDir = assetpath.parent_path();
		for (const MeshInstance& inst : instances) {
			const aiMesh* m = scene->mMeshes[inst.meshIndex];
			if ((m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) continue;
			if (materialSlotOf.contains(m->mMaterialIndex)) continue;

			const uint32_t slot = static_cast<uint32_t>(result.importedMaterials.size());
			materialSlotOf[m->mMaterialIndex] = slot;
			result.importedMaterials.push_back(
				ImportMaterial(scene, scene->mMaterials[m->mMaterialIndex], modelDir, result.textures));
		}
		// materialSlotCount is derived on merge from importedMaterials.size()

		// --- Pass 3: emit vertices and triangles -----------------------------
		for (const MeshInstance& inst : instances) {
			const aiMesh* m = scene->mMeshes[inst.meshIndex];
			// SortByPType has already split points and lines into their own
			// meshes, so a non-triangle mesh here has nothing to contribute.
			if ((m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) continue;

			const uint32_t instanceFirstVertex = static_cast<uint32_t>(vertices.size());
			const uint32_t instanceFirstTri    = static_cast<uint32_t>(triPositions.size());
			const uint32_t slot = materialSlotOf[m->mMaterialIndex];

			const bool hasNormals  = m->HasNormals();
			const bool hasUVs      = m->HasTextureCoords(0);
			const bool hasTangents = m->HasTangentsAndBitangents();

			if (!hasUVs) {
				LOG_ENGINE_WARN("LoadMesh: submesh '{0}' of {1} has no UV0; textures and normal "
				                "mapping are unavailable for it",
				                m->mName.C_Str(), assetpath.string());
			}

			// Normals transform by the inverse transpose; tangents transform by
			// the model matrix itself. Using one matrix for both is the classic
			// silent bug -- it only shows up under non-uniform scale, as a
			// subtly skewed TBN.
			const glm::mat3 normalMat = glm::mat3(glm::transpose(glm::inverse(inst.world)));
			const glm::mat3 tangentMat = glm::mat3(inst.world);

			for (unsigned int v = 0; v < m->mNumVertices; ++v) {
				Gpu::Vertex out{};

				const glm::vec3 p = glm::vec3(inst.world * glm::vec4(
					m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z, 1.0f));

				glm::vec2 uv(0.0f);
				if (hasUVs) {
					// mNumUVComponents may say 3; only the first two are used.
					uv = { m->mTextureCoords[0][v].x, m->mTextureCoords[0][v].y };
				}

				glm::vec3 n(0.0f, 1.0f, 0.0f);
				if (hasNormals) {
					n = normalMat * glm::vec3(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z);
					const float len = glm::length(n);
					n = (len > 1e-8f) ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
				}

				out.positionU = glm::vec4(p, uv.x);
				out.normalV   = glm::vec4(n, uv.y);

				if (hasTangents) {
					glm::vec3 t = tangentMat * glm::vec3(
						m->mTangents[v].x, m->mTangents[v].y, m->mTangents[v].z);
					const float len = glm::length(t);
					if (len > 1e-8f) {
						t /= len;
						const glm::vec3 b = tangentMat * glm::vec3(
							m->mBitangents[v].x, m->mBitangents[v].y, m->mBitangents[v].z);
						const float handedness = (glm::dot(glm::cross(n, t), b) < 0.0f) ? -1.0f : 1.0f;
						out.tangent = glm::vec4(t, handedness);
					}
					// else: leave tangent at vec4(0) -- w == 0 is the sentinel.
				}

				vertices.push_back(out);
			}

			for (unsigned int f = 0; f < m->mNumFaces; ++f) {
				const aiFace& face = m->mFaces[f];
				if (face.mNumIndices != 3) continue;
				const unsigned int* idx = face.mIndices;

				// MESH-LOCAL vertex indices here; MergeMesh adds the pool's vertex
				// base to make them global, which is what Gpu::TriRef's contract
				// requires and what saves the shader an add per hit.
				triRefs.push_back(Gpu::TriRef{
					instanceFirstVertex + idx[0],
					instanceFirstVertex + idx[1],
					instanceFirstVertex + idx[2],
					slot });

				// De-referenced world-space positions, written in lockstep with
				// the TriRef above so index i of each names the same triangle.
				triPositions.push_back(Gpu::TrianglePositions{
					vertices[instanceFirstVertex + idx[0]].positionU * glm::vec4(1, 1, 1, 0),
					vertices[instanceFirstVertex + idx[1]].positionU * glm::vec4(1, 1, 1, 0),
					vertices[instanceFirstVertex + idx[2]].positionU * glm::vec4(1, 1, 1, 0) });
			}

			const uint32_t instanceTriCount = static_cast<uint32_t>(triPositions.size()) - instanceFirstTri;

			if (!hasNormals) {
				LOG_ENGINE_WARN("LoadMesh: submesh '{0}' of {1} arrived with no normals despite "
				                "GenSmoothNormals; computing them on the CPU",
				                m->mName.C_Str(), assetpath.string());
				ComputeSmoothNormals(vertices, triRefs, instanceFirstTri, instanceTriCount);
			}
			if (!hasTangents && hasUVs) {
				ComputeTangents(vertices, triRefs, instanceFirstTri, instanceTriCount);
			}

			result.submeshes.push_back(SubmeshInfo{
				instanceFirstTri,      // already mesh-local: the decode buffers start empty
				instanceTriCount,
				slot,
				m->mName.C_Str() });
		}

		assert(triPositions.size() == triRefs.size() &&
		       "triPositions and triRefs must be appended in lockstep");

		// Build the BVH over positions only, exactly as before. The BVH never
		// sees an attribute and must not start to.
		uint32_t firstNodeIdx = 0;
		BVHAccel bvh(triPositions, 0, static_cast<uint32_t>(triPositions.size()));
		bvh.Build(result.nodes, result.primIndex, firstNodeIdx, result.nodeCount);
		assert(firstNodeIdx == 0 && "the decode buffers start empty");

		result.sourcePath = assetpath;
		std::error_code ec;
		result.fileSizeInBytes = std::filesystem::file_size(assetpath, ec);
		result.decodeTimeMs = std::chrono::duration<double, std::milli>(
			std::chrono::high_resolution_clock::now() - timerStart).count();
		return result;
	}


	// MERGE. Serial by construction -- it is the only thing that writes the
	// AssetPool, so the pool needs no locking and the resulting layout is
	// deterministic regardless of what order the parallel decodes finished in.
	bool AssetManager::MergeMesh(MeshImportResult& result, LR_GUID guid) {
		if (!m_AssetPool) {
			LOG_ENGINE_CRITICAL("MergeMesh: called without a valid AssetPool");
			return false;
		}

		auto& poolPositions = m_AssetPool->TriPositionBuffer;
		auto& poolRefs      = m_AssetPool->TriRefBuffer;
		auto& poolVertices  = m_AssetPool->VertexBuffer;
		auto& poolNodes     = m_AssetPool->NodeBuffer;
		auto& poolPrimIdx   = m_AssetPool->BvhPrimIndexBuffer;

		auto metadata = std::make_shared<MeshMetadata>();
		metadata->firstTriIdx    = static_cast<uint32_t>(poolPositions.size());
		metadata->firstVertexIdx = static_cast<uint32_t>(poolVertices.size());
		metadata->firstNodeIdx   = static_cast<uint32_t>(poolNodes.size());
		metadata->TriCount       = static_cast<uint32_t>(result.triPositions.size());
		metadata->vertexCount    = static_cast<uint32_t>(result.vertices.size());
		metadata->nodeCount      = result.nodeCount;
		metadata->materialSlotCount = static_cast<uint32_t>(result.importedMaterials.size());
		metadata->submeshes         = std::move(result.submeshes);
		metadata->importedMaterials = std::move(result.importedMaterials);

		auto metadataExtension = std::make_shared<MeshMetadataExtension>();
		metadataExtension->sourcePath = result.sourcePath;
		metadataExtension->fileSizeInBytes = result.fileSizeInBytes;
		metadataExtension->loadTimeMs = static_cast<float>(result.decodeTimeMs);

		poolVertices.insert(poolVertices.end(), result.vertices.begin(), result.vertices.end());
		poolPositions.insert(poolPositions.end(), result.triPositions.begin(), result.triPositions.end());

		// THE ONE REBASE. TriRef indices are global by contract, so the mesh's
		// vertex base is added here rather than in the shader. Everything else --
		// BVH nodes, the primitive permutation, submesh ranges -- is already
		// mesh-relative and appends unchanged.
		poolRefs.reserve(poolRefs.size() + result.triRefs.size());
		for (Gpu::TriRef& t : result.triRefs) {
			poolRefs.push_back(Gpu::TriRef{
				metadata->firstVertexIdx + t.i0,
				metadata->firstVertexIdx + t.i1,
				metadata->firstVertexIdx + t.i2,
				t.materialSlot });
		}

		poolNodes.insert(poolNodes.end(), result.nodes.begin(), result.nodes.end());
		poolPrimIdx.insert(poolPrimIdx.end(), result.primIndex.begin(), result.primIndex.end());

		m_AssetPool->MarkUpdated(AssetPool::AssetType::TriPositionBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::TriRefBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::VertexBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::NodeBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::BvhPrimIndexBuffer);

		// Textures the model carried. Registered here rather than at decode time
		// so the pool stays single-writer; a texture already present (shared with
		// another model, or a re-import) keeps its existing entry.
		for (auto& [texGuid, pixels] : result.textures) {
			if (m_AssetPool->Textures.contains(texGuid)) continue;

			auto texMeta = std::make_shared<TextureMetadata>();
			texMeta->width    = pixels.width;
			texMeta->height   = pixels.height;
			texMeta->channels = pixels.channels;
			texMeta->isSRGB   = pixels.isSRGB;

			auto texExt = std::make_shared<TextureMetadataExtension>();
			texExt->sourcePath = pixels.sourceKey;
			// OWNED BY THE MODEL. SaveAssetPoolToFolder must not write a .lrmeta
			// for this: the path is synthetic for embedded images, so the sidecar
			// would point at nothing and the next project open would warn on
			// every one.
			texExt->ownedByModel = true;

			m_AssetPool->Textures[texGuid] = std::move(pixels);
			m_AssetPool->Metadata[texGuid] = { texMeta, texExt };
		}
		if (!result.textures.empty()) {
			m_AssetPool->MarkUpdated(AssetPool::AssetType::Textures);
		}

		m_AssetPool->Metadata[guid] = { metadata, metadataExtension };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("LoadMesh: loaded {0} triangles / {1} vertices / {2} submeshes / {3} materials "
		                "from {4} (GUID {5}), decoded in {6:.2f} ms",
			metadata->TriCount, metadata->vertexCount, metadata->submeshes.size(),
			metadata->materialSlotCount, result.sourcePath.string(), (uint64_t)guid,
			result.decodeTimeMs);
		return true;
	}


	// COMPACT. The inverse of MergeMesh, and the reason removing an asset is not a
	// map erase.
	//
	// MergeMesh only appends, so import needs exactly ONE rebase: TriRef's vertex
	// indices. Removal has to close a hole in the MIDDLE of five buffers, which
	// moves every asset merged after this one to a lower offset. Two things then
	// point at the wrong data unless they are rewritten here:
	//   1. the surviving TriRefs, which hold GLOBAL vertex indices, and
	//   2. every later MeshMetadata's first*Idx.
	// Getting (1) wrong does not produce a missing mesh, it produces triangles
	// built from three unrelated vertices -- geometry noise that reads as a
	// renderer bug rather than as an asset one.
	void AssetManager::CompactMeshOut(const MeshMetadata& mesh, LR_GUID guid) {
		AssetPool& pool = *m_AssetPool;

		const uint32_t firstTri  = mesh.firstTriIdx,    triCount  = mesh.TriCount;
		const uint32_t firstVtx  = mesh.firstVertexIdx, vtxCount  = mesh.vertexCount;
		const uint32_t firstNode = mesh.firstNodeIdx,   nodeCount = mesh.nodeCount;

		// Clamped rather than trusted. A metadata offset that has drifted out of
		// range is a bug somewhere else, but it must not turn into an erase() run
		// off the end of a vector here.
		auto eraseRange = [](auto& buffer, size_t first, size_t count) {
			if (count == 0 || first >= buffer.size()) return;
			count = std::min(count, buffer.size() - first);
			const auto begin = buffer.begin() + static_cast<ptrdiff_t>(first);
			buffer.erase(begin, begin + static_cast<ptrdiff_t>(count));
		};

		// --- 1. THE THREE TRIANGLE-PARALLEL BUFFERS --------------------------
		// TriPositionBuffer, TriRefBuffer and BvhPrimIndexBuffer are three views of
		// ONE list of triangles: entry i of each describes triangle i, and a mesh
		// owns [firstTriIdx, firstTriIdx + TriCount) of all three. They must be cut
		// on the SAME range or the views stop agreeing.
		//
		// That the BVH permutation is one of those views is the part worth stating,
		// because it has no offset of its own to give it away: BVHAccel::Build()
		// writes it through &indexBuffer[firstTriIdx], and Trace.slang reads it as
		// BvhPrimIndex[rootTriIdx + first + i]. Its base IS the triangle base.
		eraseRange(pool.TriPositionBuffer,  firstTri, triCount);
		eraseRange(pool.TriRefBuffer,       firstTri, triCount);
		eraseRange(pool.BvhPrimIndexBuffer, firstTri, triCount);

		// --- 2. NODES AND VERTICES, on their own ranges ----------------------
		eraseRange(pool.NodeBuffer,   firstNode, nodeCount);
		eraseRange(pool.VertexBuffer, firstVtx,  vtxCount);

		// --- 3. THE ONE VALUE REBASE -----------------------------------------
		// Gpu::TriRef holds GLOBAL vertex indices -- MergeMesh added the mesh's
		// firstVertexIdx at import to save the shader an add per hit -- so closing
		// the hole in VertexBuffer repoints every later triangle unless the indices
		// come down with it. Each mesh indexes only its own vertex range, so a
		// surviving index is either below the hole (untouched) or above it (shifted
		// down by exactly vtxCount).
		//
		// Tested against the END of the removed range rather than its start: the
		// subtraction then cannot underflow, and it is the same shape as the
		// metadata rewrite below, so the two cannot drift apart.
		if (vtxCount > 0) {
			const uint32_t vtxEnd = firstVtx + vtxCount;
			auto rebase = [&](uint32_t& idx) {
				// A surviving triangle pointing INTO the hole would mean two meshes
				// sharing vertices, which no import path can produce and which no
				// amount of rebasing could repair.
				assert(!(idx >= firstVtx && idx < vtxEnd) &&
				       "surviving TriRef indexes the removed mesh's vertices");
				if (idx >= vtxEnd) idx -= vtxCount;
			};
			for (Gpu::TriRef& t : pool.TriRefBuffer) {
				rebase(t.i0);
				rebase(t.i1);
				rebase(t.i2);
			}
		}

		// NOTHING ELSE STORES A GLOBAL INDEX. Verified rather than assumed, because
		// a missed one is silent:
		//   BVHAccel::Node::leftChild_Or_FirstTri -- mesh-local in both meanings.
		//     Build() hands SubDivide a node array based at firstNodeIdx and numbers
		//     children from 0, and a leaf's firstTri counts from the mesh's slice of
		//     BvhPrimIndexBuffer.
		//   BvhPrimIndexBuffer's VALUES -- mesh-local: Build() fills them with
		//     0..N-1, and Trace.slang adds rootTriIdx when it dereferences them.
		//   SubmeshInfo::firstTriIdx -- mesh-local by contract; Renderer::Parse adds
		//     MeshMetadata::firstTriIdx when it builds the draw list.
		// All three ride the erase unchanged.

		// --- 4. EVERY LATER ASSET'S OFFSETS ----------------------------------
		for (auto& [otherGuid, metadataPair] : pool.Metadata) {
			if (otherGuid == guid) continue;   // erased by the caller
			const auto other = std::dynamic_pointer_cast<MeshMetadata>(metadataPair.first);
			if (!other) continue;

			// ">= the end of the removed range" is what identifies a mesh merged
			// after this one, and keeps the subtraction underflow-free. An empty
			// mesh sharing a boundary offset is the only ambiguous case, and it
			// shifts by zero either way.
			if (other->firstTriIdx    >= firstTri  + triCount)  other->firstTriIdx    -= triCount;
			if (other->firstNodeIdx   >= firstNode + nodeCount) other->firstNodeIdx   -= nodeCount;
			if (other->firstVertexIdx >= firstVtx  + vtxCount)  other->firstVertexIdx -= vtxCount;
		}

		// --- 5. THE VERSION BUMPS --------------------------------------------
		// Without these the renderer keeps its stale upload: Renderer::SetupGPUResources
		// re-uploads a device-local buffer ONLY when its counter has moved, so a
		// missed bump leaves the GPU tracing triangles whose vertices no longer
		// exist. All five are bumped unconditionally -- a mesh owns a slice of each
		// by construction, and one redundant re-upload on a removal is cheaper than
		// reasoning about which buffer was allowed to skip. The rasterizer's index
		// buffer is derived from TriRefBuffer under the TriRefBuffer counter, so it
		// is covered too.
		pool.MarkUpdated(AssetPool::AssetType::TriPositionBuffer);
		pool.MarkUpdated(AssetPool::AssetType::TriRefBuffer);
		pool.MarkUpdated(AssetPool::AssetType::VertexBuffer);
		pool.MarkUpdated(AssetPool::AssetType::NodeBuffer);
		pool.MarkUpdated(AssetPool::AssetType::BvhPrimIndexBuffer);

		LOG_ENGINE_INFO("CompactMeshOut: removed {0} triangles / {1} vertices / {2} BVH nodes "
		                "for GUID {3}; buffers now {4} tris / {5} verts / {6} nodes",
			triCount, vtxCount, nodeCount, (uint64_t)guid,
			pool.TriPositionBuffer.size(), pool.VertexBuffer.size(), pool.NodeBuffer.size());
	}


	// ============================================================================
	// THE COOKED LOAD PATH
	//
	// A .x3mesh is UNTRUSTED DATA -- it is a file this process did not write, and
	// in a shipped build it is a file the user could have replaced. Nothing below
	// asserts on its contents and nothing below can crash on them: every check is
	// a nullopt return that the caller turns into a fall back to the importer,
	// which is why turning the mode on can never stop a project from opening.
	//
	// THE INDEX CONTRACT, verified against both ends rather than assumed:
	//   - ExtractCookedMesh SUBTRACTS the mesh's firstVertexIdx from every TriRef
	//     before writing (AssetCook.cpp, "THE ONE UN-REBASE"), so the file holds
	//     MESH-LOCAL vertex indices;
	//   - MergeMesh ADDS the pool's new firstVertexIdx back on merge
	//     (AssetManager.cpp, "THE ONE REBASE"), for cooked and imported meshes
	//     alike, because it cannot tell them apart.
	// So a MeshImportResult built here carries mesh-local indices exactly as
	// DecodeMesh's does, and needs no adjustment of its own. Everything else --
	// BVH nodes, the primitive permutation, submesh ranges -- is mesh-relative by
	// construction at both ends and appends unchanged.
	// ============================================================================
	std::optional<AssetManager::MeshImportResult>
	AssetManager::DecodeCookedMesh(const std::filesystem::path& cookedPath,
	                               const std::filesystem::path& sourcePath) {
		auto timerStart = std::chrono::high_resolution_clock::now();

		// Framing: magic, version, struct-layout fingerprint, section extents,
		// payload hash. Logs which guard fired.
		std::optional<CookedMesh> cooked = ReadCookedMesh(cookedPath);
		if (!cooked) return std::nullopt;

		// Meaning: every index inside the file checked against the array it
		// indexes. ReadCookedMesh proves the file is INTACT, which is a different
		// claim from the mesh being loadable -- see CookedMeshIsSelfConsistent.
		// This is the last point at which a bad index is cheap to catch; after
		// MergeMesh it is in a GPU buffer.
		std::string whyNot;
		if (!CookedMeshIsSelfConsistent(*cooked, &whyNot)) {
			LOG_ENGINE_WARN("DecodeCookedMesh: {0} is internally inconsistent ({1}) -- refusing it",
				cookedPath.string(), whyNot);
			return std::nullopt;
		}

		MeshImportResult result;
		result.vertices     = std::move(cooked->vertices);
		result.triRefs      = std::move(cooked->triRefs);        // mesh-local; MergeMesh rebases
		result.triPositions = std::move(cooked->triPositions);
		result.nodes        = std::move(cooked->nodes);
		result.primIndex    = std::move(cooked->primIndex);
		result.submeshes    = std::move(cooked->submeshes);
		result.importedMaterials = std::move(cooked->materials);

		// nodeCount IS nodes.size() for a cooked mesh, and that is a property of
		// how the file was produced rather than a coincidence: BVHAccel::Build
		// over-allocates 2N-1 nodes and then resizes down to exactly the number it
		// used, and ExtractCookedMesh slices precisely [firstNodeIdx, +nodeCount).
		// So the file never contains the unused tail. DecodeMesh's own nodeCount
		// comes out of Build for the same reason.
		result.nodeCount = static_cast<uint32_t>(result.nodes.size());

		// NO TEXTURES. Cooking textures is the BC7 half of Phase 9 and does not
		// exist yet, so the materials carry texture GUIDs pointing at assets this
		// file does not contain. Those resolve normally when the texture is in the
		// pool from another import, and fall back to the material's scalar factors
		// when it is not -- TextureTable::resolve already logs "references texture
		// GUID N which has no pixels" for exactly this case. A cooked mesh loaded
		// on its own therefore renders untextured, which is a KNOWN limitation
		// that announces itself rather than a silent one.

		// The SOURCE, not the cooked file: MeshMetadataExtension::sourcePath is
		// what SaveAssetPoolToFolder writes into the .lrmeta and what RemoveAsset
		// deletes. Recording the cache here would rewrite the project to reference
		// the cache, after which there is no source left to check freshness
		// against and no way back to the importer. See the header.
		result.sourcePath = sourcePath.empty() ? cookedPath : sourcePath;
		std::error_code ec;
		result.fileSizeInBytes = std::filesystem::file_size(result.sourcePath, ec);
		if (ec) result.fileSizeInBytes = 0;   // display-only; never worth failing a load over
		result.decodeTimeMs = std::chrono::duration<double, std::milli>(
			std::chrono::high_resolution_clock::now() - timerStart).count();

		LOG_ENGINE_INFO("DecodeCookedMesh: loaded {0} triangles / {1} vertices / {2} BVH nodes from "
		                "cooked {3} in {4:.2f} ms (no assimp parse, no BVH build)",
			result.triPositions.size(), result.vertices.size(), result.nodeCount,
			cookedPath.string(), result.decodeTimeMs);
		return result;
	}


	std::optional<AssetManager::MeshImportResult>
	AssetManager::DecodeMeshAsset(const std::filesystem::path& assetpath) {
		const std::string extension = assetpath.has_extension() ? assetpath.extension().string() : std::string{};

		// A .x3mesh asked for BY NAME. There is no source to compare against, so
		// there is no freshness question to answer -- and no importer to fall back
		// to either, which is the one case where a rejected cooked file is a
		// failed import rather than a slow one.
		if (IsCookedMeshExtension(extension)) {
			return DecodeCookedMesh(assetpath, /*sourcePath*/ assetpath);
		}

		if (LoadCookedModeEnabled()) {
			const std::filesystem::path cookedPath = CookedSiblingPath(assetpath);
			std::string why;
			const CookedFreshness freshness = CheckCookedMeshFreshness(cookedPath, assetpath, &why);

			if (freshness == CookedFreshness::Fresh) {
				if (auto result = DecodeCookedMesh(cookedPath, assetpath))
					return result;
				// FRESH BUT UNUSABLE. The freshness probe deliberately skips the
				// payload hash (it would cost more than the load it is deciding
				// about), so a corrupt-but-recent cooked file gets this far and is
				// caught by the full read. Falling through to the importer rather
				// than failing is the whole contract of the mode.
				LOG_ENGINE_WARN("DecodeMeshAsset: cooked file {0} is fresh but could not be loaded -- "
				                "importing {1} instead", cookedPath.string(), assetpath.string());
			}
			else if (freshness != CookedFreshness::NoCookedFile) {
				// NoCookedFile is the ordinary state of an uncooked project and is
				// not worth a line per mesh; anything else means a cooked file is
				// sitting there being ignored, and a developer who turned this mode
				// on to reproduce a cook-only bug needs to know it did not engage.
				LOG_ENGINE_WARN("DecodeMeshAsset: ignoring cooked file {0} ({1}: {2}) -- importing {3}",
					cookedPath.string(), CookedFreshnessToString(freshness), why, assetpath.string());
			}
		}

		return DecodeMesh(assetpath);
	}


	bool AssetManager::LoadMesh(const std::filesystem::path& assetpath, LR_GUID guid) {
		// The single-asset path: decode and merge back to back. The parallel path
		// is LoadAssetPoolFromFolder, which decodes many at once and then merges
		// them in order. Both go through DecodeMeshAsset, so both make the
		// cooked-or-imported decision in exactly one place.
		auto result = DecodeMeshAsset(assetpath);
		if (!result) return false;
		return MergeMesh(*result, guid);
	}


	bool AssetManager::LoadTexture(const std::filesystem::path& assetpath, LR_GUID guid, int channels, bool isSRGB) {
		auto timerStart = std::chrono::high_resolution_clock::now();

		if (!m_AssetPool) {
			LOG_ENGINE_CRITICAL("LoadTexture: called without a valid AssetPool for asset {0}", assetpath.string());
			return false;
		}

		int width, height, channelsInFile;
		// NO VERTICAL FLIP. The pre-Phase-2 code called
		// stbi_set_flip_vertically_on_load(1) with the comment "OpenGL-style
		// orientation". glTF and assimp UVs use a top-left origin (V down), which
		// is also Vulkan's convention -- flipping the image AND using assimp UVs
		// verbatim gives vertically mirrored textures on every model. The flip is
		// gone and the UVs are used as imported. The skybox's `v` in the shaders
		// was compensating for the flip and is corrected in the same change.
		//
		// THE _thread VARIANT, not the plain one: the plain setter writes a
		// process-wide global, and this decode path also runs from job-system
		// worker threads (see DecodeMesh). Two threads setting a shared int while
		// a third reads it mid-decode is a data race whose symptom would be an
		// occasional upside-down texture -- which reads as an importer bug.
		stbi_set_flip_vertically_on_load_thread(0);
		unsigned char* data = stbi_load(assetpath.string().c_str(), &width, &height, &channelsInFile, channels);
		if (!data) {
			LOG_ENGINE_CRITICAL("LoadTexture: failed to load texture from path={0} (requested channels={1}) for GUID={2}.",
				assetpath.string(), channels, (uint64_t)guid);
			return false;
		}

		const int actualChannels = (channels == 0) ? channelsInFile : channels;
		const size_t totalBytes = static_cast<size_t>(width) * height * actualChannels;

		TexturePixels pixels;
		pixels.data.assign(data, data + totalBytes);
		pixels.width    = width;
		pixels.height   = height;
		pixels.channels = actualChannels;
		pixels.isSRGB   = isSRGB;
		stbi_image_free(data);

		m_AssetPool->Textures[guid] = std::move(pixels);

		auto metadata = std::make_shared<TextureMetadata>();
		metadata->width    = width;
		metadata->height   = height;
		metadata->channels = actualChannels;
		metadata->isSRGB   = isSRGB;

		auto metadataExt = std::make_shared<TextureMetadataExtension>();
		metadataExt->sourcePath = assetpath;
		metadataExt->fileSizeInBytes = std::filesystem::file_size(assetpath);

		double loadTimeMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - timerStart).count();
		metadataExt->loadTimeMs = loadTimeMs;

		m_AssetPool->Metadata[guid] = { metadata, metadataExt };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Textures);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("LoadTexture: loaded texture {0} (GUID {1}) {2}x{3} with {4} channels, {5} in {6:.2f} ms",
			assetpath.string(), (uint64_t)guid, width, height, actualChannels,
			isSRGB ? "sRGB" : "linear", loadTimeMs);
		return true;
	}


	// ============================================================================
	// MATERIAL AND MODEL-OWNED TEXTURE IMPORT
	// ============================================================================

	LR_GUID AssetManager::ResolveModelTexture(const aiScene* scene, const aiString& texPath,
	                                          const std::filesystem::path& modelDir,
	                                          bool isSRGB,
	                                          std::unordered_map<LR_GUID, TexturePixels>& out) {
		// A stable, content-addressed GUID so re-importing the same model twice
		// resolves to the same texture asset instead of duplicating it, and so a
		// texture shared by five materials (a glTF ORM map typically is) uploads
		// once. Derived, not random -- LR_GUID's default constructor is random and
		// would defeat both.
		const std::string key = (modelDir / texPath.C_Str()).string();
		const LR_GUID guid{ std::hash<std::string>{}(key) | 0x8000000000000000ULL };

		if (out.contains(guid))
			return guid;   // already decoded, by this model or another material

		int width = 0, height = 0;
		std::vector<unsigned char> rgba;

		// EMBEDDED TEXTURES. Both sample .glb models carry their images inside
		// the file, where GetTexture returns a path like "*0" that resolves to
		// nothing on the filesystem. Before Phase 2 there was no path to reach
		// them at all.
		const aiTexture* embedded = scene ? scene->GetEmbeddedTexture(texPath.C_Str()) : nullptr;
		if (embedded) {
			if (embedded->mHeight == 0) {
				// Compressed blob (png/jpg bytes) of mWidth bytes.
				int channelsInFile = 0;
				stbi_set_flip_vertically_on_load_thread(0);   // per-thread; see LoadTexture
				unsigned char* decoded = stbi_load_from_memory(
					reinterpret_cast<const stbi_uc*>(embedded->pcData),
					static_cast<int>(embedded->mWidth), &width, &height, &channelsInFile, 4);
				if (!decoded) {
					LOG_ENGINE_WARN("ResolveModelTexture: could not decode embedded texture '{0}'", texPath.C_Str());
					return LR_GUID::INVALID;
				}
				rgba.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
				stbi_image_free(decoded);
			}
			else {
				// Raw aiTexel array, which assimp documents as BGRA8.
				width  = static_cast<int>(embedded->mWidth);
				height = static_cast<int>(embedded->mHeight);
				const size_t texelCount = static_cast<size_t>(width) * height;
				rgba.resize(texelCount * 4);
				for (size_t i = 0; i < texelCount; ++i) {
					rgba[i * 4 + 0] = embedded->pcData[i].r;
					rgba[i * 4 + 1] = embedded->pcData[i].g;
					rgba[i * 4 + 2] = embedded->pcData[i].b;
					rgba[i * 4 + 3] = embedded->pcData[i].a;
				}
			}
		}
		else {
			std::filesystem::path resolved = std::filesystem::path(texPath.C_Str());
			if (resolved.is_relative())
				resolved = (modelDir / resolved).lexically_normal();

			if (!std::filesystem::exists(resolved)) {
				LOG_ENGINE_WARN("ResolveModelTexture: texture '{0}' referenced by a model does not exist "
				                "(resolved to {1})", texPath.C_Str(), resolved.string());
				return LR_GUID::INVALID;
			}

			int channelsInFile = 0;
			stbi_set_flip_vertically_on_load_thread(0);   // per-thread; see LoadTexture
			unsigned char* decoded = stbi_load(resolved.string().c_str(), &width, &height, &channelsInFile, 4);
			if (!decoded) {
				LOG_ENGINE_WARN("ResolveModelTexture: stb_image failed on {0}", resolved.string());
				return LR_GUID::INVALID;
			}
			rgba.assign(decoded, decoded + static_cast<size_t>(width) * height * 4);
			stbi_image_free(decoded);
		}

		TexturePixels pixels;
		pixels.data     = std::move(rgba);
		pixels.width    = width;
		pixels.height   = height;
		pixels.channels = 4;
		pixels.isSRGB   = isSRGB;
		pixels.sourceKey = key;
		out[guid] = std::move(pixels);

		LOG_ENGINE_INFO("ResolveModelTexture: decoded {0} texture '{1}' {2}x{3} ({4}) as GUID {5}",
			embedded ? "embedded" : "external", texPath.C_Str(), width, height,
			isSRGB ? "sRGB" : "linear", (uint64_t)guid);
		return guid;
	}


	MaterialDesc AssetManager::ImportMaterial(const aiScene* scene, const aiMaterial* mat,
	                                          const std::filesystem::path& modelDir,
	                                          std::unordered_map<LR_GUID, TexturePixels>& out) {
		MaterialDesc desc{};
		if (!mat) return desc;

		// --- Scalar factors, glTF PBR keys first, legacy keys as fallback ----
		aiColor4D  color4{};
		aiColor3D  color3{};
		ai_real    scalar = 0.0f;

		if (mat->Get(AI_MATKEY_BASE_COLOR, color4) == AI_SUCCESS)
			desc.color = { color4.r, color4.g, color4.b, color4.a };
		else if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color3) == AI_SUCCESS)
			desc.color = { color3.r, color3.g, color3.b, 1.0f };

		if (mat->Get(AI_MATKEY_METALLIC_FACTOR, scalar) == AI_SUCCESS)
			desc.metallic = static_cast<float>(scalar);
		if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, scalar) == AI_SUCCESS)
			desc.roughness = static_cast<float>(scalar);

		if (mat->Get(AI_MATKEY_COLOR_EMISSIVE, color3) == AI_SUCCESS) {
			float strength = 1.0f;
			if (mat->Get(AI_MATKEY_EMISSIVE_INTENSITY, scalar) == AI_SUCCESS)
				strength = static_cast<float>(scalar);
			desc.emission = { color3.r, color3.g, color3.b, strength };
		}

		// --- Textures --------------------------------------------------------
		// COLOUR SPACE IS NOT COSMETIC. Base colour and emissive carry colour and
		// belong in an sRGB view; normal maps and ORM maps carry data, and
		// sampling them through an sRGB view applies an EOTF to numbers that are
		// not light. It looks like a shading bug, not a format bug.
		aiString texPath;

		if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS ||
		    mat->GetTexture(aiTextureType_DIFFUSE,    0, &texPath) == AI_SUCCESS)
			desc.baseColorTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ true, out);

		if (mat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS)
			desc.normalTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ false, out);

		// In glTF these two resolve to the same ORM image; whichever assimp
		// reports first is the one to take.
		if (mat->GetTexture(aiTextureType_METALNESS,         0, &texPath) == AI_SUCCESS ||
		    mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS)
			desc.metalRoughTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ false, out);

		if (mat->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == AI_SUCCESS)
			desc.emissiveTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ true, out);

		return desc;
	}



	// ============================================================================
	// PRIMITIVE MESH GENERATION
	// ----------------------------------------------------------------------------
	// The primitives are now built as (vertices, triangle refs) rather than bare
	// position triples, because everything downstream of Phase 2 expects normals
	// and UVs. Normals and UVs are ANALYTIC here -- a sphere's normal is
	// normalize(p), not something averaged out of its facets -- and tangents come
	// from the same ComputeTangents() the importer's fallback uses, so a cube and
	// an imported mesh light identically under one normal map.
	//
	// Vertices are NOT shared between faces where the normal differs (the cube,
	// the caps of the cylinder and cone). Sharing them would average two
	// perpendicular normals into a bevel that is not there.
	// ============================================================================

	bool AssetManager::IsPrimitiveMesh(LR_GUID guid) {
		uint64_t id = static_cast<uint64_t>(guid);
		return id >= PrimitiveMeshGUIDs::CUBE && id <= PrimitiveMeshGUIDs::CONE;
	}

	const char* AssetManager::GetPrimitiveMeshName(LR_GUID guid) {
		uint64_t id = static_cast<uint64_t>(guid);
		switch (id) {
			case PrimitiveMeshGUIDs::CUBE:     return "Cube";
			case PrimitiveMeshGUIDs::SPHERE:   return "Sphere";
			case PrimitiveMeshGUIDs::PLANE:    return "Plane";
			case PrimitiveMeshGUIDs::CYLINDER: return "Cylinder";
			case PrimitiveMeshGUIDs::CAPSULE:  return "Capsule";
			case PrimitiveMeshGUIDs::CONE:     return "Cone";
			default: return nullptr;
		}
	}

	void AssetManager::CreatePrimitiveMesh(LR_GUID guid,
	                                       const std::vector<Gpu::Vertex>& vertices,
	                                       const std::vector<Gpu::TriRef>& tris,
	                                       const char* name) {
		auto& poolPositions = m_AssetPool->TriPositionBuffer;
		auto& poolRefs      = m_AssetPool->TriRefBuffer;
		auto& poolVertices  = m_AssetPool->VertexBuffer;

		auto metadata = std::make_shared<MeshMetadata>();
		metadata->firstTriIdx    = static_cast<uint32_t>(poolPositions.size());
		metadata->TriCount       = static_cast<uint32_t>(tris.size());
		metadata->firstVertexIdx = static_cast<uint32_t>(poolVertices.size());
		metadata->vertexCount    = static_cast<uint32_t>(vertices.size());
		metadata->materialSlotCount = 1;
		metadata->importedMaterials.push_back(MaterialDesc{});
		metadata->submeshes.push_back(SubmeshInfo{ 0, static_cast<uint32_t>(tris.size()), 0, name });

		auto metadataExtension = std::make_shared<MeshMetadataExtension>();
		metadataExtension->sourcePath = std::string("primitive://") + name;
		metadataExtension->fileSizeInBytes =
			vertices.size() * sizeof(Gpu::Vertex) + tris.size() * sizeof(Gpu::TriRef);
		metadataExtension->loadTimeMs = 0.0f;

		poolVertices.insert(poolVertices.end(), vertices.begin(), vertices.end());

		// The generators index their own local vertex array; TriRef indices are
		// GLOBAL, so rebase here rather than making every generator carry the
		// offset. This is also where positions are de-referenced, so no generator
		// ever builds TriPositionBuffer by hand.
		poolPositions.reserve(poolPositions.size() + tris.size());
		poolRefs.reserve(poolRefs.size() + tris.size());
		for (const Gpu::TriRef& t : tris) {
			poolRefs.push_back(Gpu::TriRef{
				metadata->firstVertexIdx + t.i0,
				metadata->firstVertexIdx + t.i1,
				metadata->firstVertexIdx + t.i2,
				t.materialSlot });
			poolPositions.push_back(Gpu::TrianglePositions{
				vertices[t.i0].positionU * glm::vec4(1, 1, 1, 0),
				vertices[t.i1].positionU * glm::vec4(1, 1, 1, 0),
				vertices[t.i2].positionU * glm::vec4(1, 1, 1, 0) });
		}

		m_AssetPool->MarkUpdated(AssetPool::AssetType::TriPositionBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::TriRefBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::VertexBuffer);

		// Build BVH
		BVHAccel bvh(poolPositions, metadata->firstTriIdx, metadata->TriCount);
		bvh.Build(m_AssetPool->NodeBuffer, m_AssetPool->BvhPrimIndexBuffer, metadata->firstNodeIdx, metadata->nodeCount);

		m_AssetPool->MarkUpdated(AssetPool::AssetType::NodeBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::BvhPrimIndexBuffer);

		m_AssetPool->Metadata[guid] = { metadata, metadataExtension };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("CreatePrimitiveMesh: created '{}' with {} triangles / {} vertices (GUID {})",
			name, tris.size(), vertices.size(), static_cast<uint64_t>(guid));
	}

	void AssetManager::CreatePrimitiveMeshes() {
		const float PI  = glm::pi<float>();
		const float TAU = glm::two_pi<float>();

		// Local builder: appends a vertex and returns its index, so the
		// generators below read as "emit a triangle of three fresh corners"
		// wherever the normal is per-face, and can reuse indices where it is not.
		struct Builder {
			std::vector<Gpu::Vertex> vertices;
			std::vector<Gpu::TriRef> tris;

			uint32_t vert(glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
				Gpu::Vertex v{};
				v.positionU = glm::vec4(p, uv.x);
				v.normalV   = glm::vec4(glm::normalize(n), uv.y);
				v.tangent   = glm::vec4(0.0f);          // filled by ComputeTangents
				vertices.push_back(v);
				return static_cast<uint32_t>(vertices.size() - 1);
			}
			void tri(uint32_t a, uint32_t b, uint32_t c) {
				tris.push_back(Gpu::TriRef{ a, b, c, 0 });
			}
			// The common per-face case: three corners that share one normal and
			// are used by exactly one triangle.
			void face(glm::vec3 a, glm::vec3 b, glm::vec3 c,
			          glm::vec3 n, glm::vec2 ua, glm::vec2 ub, glm::vec2 uc) {
				tri(vert(a, n, ua), vert(b, n, ub), vert(c, n, uc));
			}
			// WINDING IS ENFORCED, NOT ASSUMED.
			//
			// IntersectTri culls back faces: it rejects a hit when
			// -dot(dir, cross(E1,E2)) is negative. So a triangle wound the wrong
			// way is INVISIBLE from outside, and what the ray actually hits is the
			// far interior wall -- which renders black rather than missing, so it
			// reads as a shading bug rather than a geometry one.
			//
			// The cylinder's sides were wound this way, and so were the cone's and
			// the capsule's body: cross(E1,E2) for a quad that steps around the
			// circle and then up points INWARD. Nothing noticed because nothing in
			// the engine had ever rendered one until the lights fixture did.
			//
			// Since every generator now supplies an analytic normal, the geometric
			// normal can simply be checked against it and the triangle flipped when
			// they disagree. That fixes every primitive at once and stops the next
			// one from having to get its winding right by inspection.
			void fixWinding() {
				for (Gpu::TriRef& t : tris) {
					const glm::vec3 p0 = glm::vec3(vertices[t.i0].positionU);
					const glm::vec3 p1 = glm::vec3(vertices[t.i1].positionU);
					const glm::vec3 p2 = glm::vec3(vertices[t.i2].positionU);
					const glm::vec3 geom = glm::cross(p1 - p0, p2 - p0);

					const glm::vec3 shading = glm::vec3(vertices[t.i0].normalV)
					                        + glm::vec3(vertices[t.i1].normalV)
					                        + glm::vec3(vertices[t.i2].normalV);

					if (glm::dot(geom, shading) < 0.0f)
						std::swap(t.i1, t.i2);
				}
			}

			void finish() {
				fixWinding();
				ComputeTangents(vertices, tris, 0, static_cast<uint32_t>(tris.size()));
			}
		};

		// ========================================
		// CUBE (unit cube centered at origin)
		// ========================================
		{
			Builder b;
			const float s = 0.5f;

			// Per face: the two in-plane axes remapped to [0,1]. Corners are not
			// shared across faces -- the normals differ.
			auto quad = [&](glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3, glm::vec3 n) {
				b.face(p0, p1, p2, n, {0,1}, {1,1}, {1,0});
				b.face(p0, p2, p3, n, {0,1}, {1,0}, {0,0});
			};
			quad({-s,-s, s}, { s,-s, s}, { s, s, s}, {-s, s, s}, { 0, 0, 1});  // front
			quad({ s,-s,-s}, {-s,-s,-s}, {-s, s,-s}, { s, s,-s}, { 0, 0,-1});  // back
			quad({ s,-s, s}, { s,-s,-s}, { s, s,-s}, { s, s, s}, { 1, 0, 0});  // right
			quad({-s,-s,-s}, {-s,-s, s}, {-s, s, s}, {-s, s,-s}, {-1, 0, 0});  // left
			quad({-s, s, s}, { s, s, s}, { s, s,-s}, {-s, s,-s}, { 0, 1, 0});  // top
			quad({-s,-s,-s}, { s,-s,-s}, { s,-s, s}, {-s,-s, s}, { 0,-1, 0});  // bottom

			b.finish();
			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CUBE), b.vertices, b.tris, "Cube");
		}

		// ========================================
		// SPHERE (UV sphere, radius 0.5)
		// ========================================
		{
			Builder b;
			const float radius = 0.5f;
			const int stacks = 16;
			const int slices = 24;

			// N = normalize(p); uv = (phi/2pi, theta/pi). Corners are emitted per
			// quad rather than shared, because the seam at phi = 0 == 2pi needs
			// two different U values for the same position.
			auto point = [&](float theta, float phi) {
				const glm::vec3 p = radius * glm::vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
				return b.vert(p, p, { phi / TAU, theta / PI });
			};

			for (int i = 0; i < stacks; ++i) {
				const float theta1 = PI * float(i) / float(stacks);
				const float theta2 = PI * float(i + 1) / float(stacks);
				for (int j = 0; j < slices; ++j) {
					const float phi1 = TAU * float(j) / float(slices);
					const float phi2 = TAU * float(j + 1) / float(slices);

					// Degenerate triangles at the poles are skipped exactly as the
					// pre-Phase-2 generator did.
					if (i != 0)          b.tri(point(theta1, phi1), point(theta1, phi2), point(theta2, phi2));
					if (i != stacks - 1) b.tri(point(theta1, phi1), point(theta2, phi2), point(theta2, phi1));
				}
			}

			b.finish();
			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::SPHERE), b.vertices, b.tris, "Sphere");
		}

		// ========================================
		// PLANE (1x1 plane on XZ, y = 0)
		// ========================================
		{
			Builder b;
			const float s = 0.5f;
			const glm::vec3 n{ 0, 1, 0 };
			b.face({-s,0, s}, { s,0, s}, { s,0,-s}, n, {0,1}, {1,1}, {1,0});
			b.face({-s,0, s}, { s,0,-s}, {-s,0,-s}, n, {0,1}, {1,0}, {0,0});

			b.finish();
			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE), b.vertices, b.tris, "Plane");
		}

		// ========================================
		// CYLINDER (radius 0.5, height 1)
		// ========================================
		{
			Builder b;
			const float radius = 0.5f;
			const float halfHeight = 0.5f;
			const int segments = 24;

			for (int i = 0; i < segments; ++i) {
				const float a1 = TAU * float(i) / float(segments);
				const float a2 = TAU * float(i + 1) / float(segments);
				const float u1 = float(i) / float(segments);
				const float u2 = float(i + 1) / float(segments);

				const glm::vec3 b1{ radius * cos(a1), -halfHeight, radius * sin(a1) };
				const glm::vec3 b2{ radius * cos(a2), -halfHeight, radius * sin(a2) };
				const glm::vec3 t1{ radius * cos(a1),  halfHeight, radius * sin(a1) };
				const glm::vec3 t2{ radius * cos(a2),  halfHeight, radius * sin(a2) };

				// Side: radial normal, uv = (angle/2pi, (y+h)/2h).
				const glm::vec3 n1{ cos(a1), 0, sin(a1) };
				const glm::vec3 n2{ cos(a2), 0, sin(a2) };
				b.tri(b.vert(b1, n1, {u1,0}), b.vert(b2, n2, {u2,0}), b.vert(t2, n2, {u2,1}));
				b.tri(b.vert(b1, n1, {u1,0}), b.vert(t2, n2, {u2,1}), b.vert(t1, n1, {u1,1}));

				// Caps: constant +/-Y normal, planar UV from the XZ position.
				auto capUV = [&](const glm::vec3& p) {
					return glm::vec2(p.x / (2 * radius) + 0.5f, p.z / (2 * radius) + 0.5f);
				};
				const glm::vec3 bc{ 0, -halfHeight, 0 };
				const glm::vec3 tc{ 0,  halfHeight, 0 };
				b.face(bc, b2, b1, { 0,-1, 0 }, capUV(bc), capUV(b2), capUV(b1));
				b.face(tc, t1, t2, { 0, 1, 0 }, capUV(tc), capUV(t1), capUV(t2));
			}

			b.finish();
			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CYLINDER), b.vertices, b.tris, "Cylinder");
		}

		// ========================================
		// CAPSULE (radius 0.25, total height 1)
		// ========================================
		{
			Builder b;
			const float radius = 0.25f;
			const float cylHalf = 0.25f;
			const int stacks = 8;
			const int slices = 16;

			// Cylinder body
			for (int i = 0; i < slices; ++i) {
				const float a1 = TAU * float(i) / float(slices);
				const float a2 = TAU * float(i + 1) / float(slices);
				const float u1 = float(i) / float(slices);
				const float u2 = float(i + 1) / float(slices);

				const glm::vec3 b1{ radius * cos(a1), -cylHalf, radius * sin(a1) };
				const glm::vec3 b2{ radius * cos(a2), -cylHalf, radius * sin(a2) };
				const glm::vec3 t1{ radius * cos(a1),  cylHalf, radius * sin(a1) };
				const glm::vec3 t2{ radius * cos(a2),  cylHalf, radius * sin(a2) };
				const glm::vec3 n1{ cos(a1), 0, sin(a1) };
				const glm::vec3 n2{ cos(a2), 0, sin(a2) };

				// V spans the middle third of the capsule; the hemispheres take
				// the outer thirds, so the parameterisation is continuous.
				b.tri(b.vert(b1, n1, {u1,1.f/3}), b.vert(b2, n2, {u2,1.f/3}), b.vert(t2, n2, {u2,2.f/3}));
				b.tri(b.vert(b1, n1, {u1,1.f/3}), b.vert(t2, n2, {u2,2.f/3}), b.vert(t1, n1, {u1,2.f/3}));
			}

			// Hemispheres: sphere about the cap centre. The normal is the offset
			// from that centre, NOT from the origin.
			auto hemi = [&](float centreY, int stackBegin, int stackEnd, float vBase, float vSpan) {
				auto point = [&](float theta, float phi) {
					const glm::vec3 off = radius * glm::vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
					return b.vert(glm::vec3(off.x, centreY + off.y, off.z), off,
					              { phi / TAU, vBase + vSpan * (theta / PI) });
				};
				for (int i = stackBegin; i < stackEnd; ++i) {
					const float theta1 = PI * float(i) / float(stacks);
					const float theta2 = PI * float(i + 1) / float(stacks);
					for (int j = 0; j < slices; ++j) {
						const float phi1 = TAU * float(j) / float(slices);
						const float phi2 = TAU * float(j + 1) / float(slices);
						if (i != 0)          b.tri(point(theta1, phi1), point(theta1, phi2), point(theta2, phi2));
						if (i != stacks - 1) b.tri(point(theta1, phi1), point(theta2, phi2), point(theta2, phi1));
					}
				}
			};
			hemi( cylHalf, 0,          stacks / 2, 1.0f, -1.0f);   // top    (theta 0 -> V 1)
			hemi(-cylHalf, stacks / 2, stacks,     1.0f, -1.0f);   // bottom (theta pi -> V 0)

			b.finish();
			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CAPSULE), b.vertices, b.tris, "Capsule");
		}

		// ========================================
		// CONE (radius 0.5, height 1)
		// ========================================
		{
			Builder b;
			const float radius = 0.5f;
			const float halfHeight = 0.5f;
			const float height = 2.0f * halfHeight;
			const int segments = 24;
			const glm::vec3 apex{ 0, halfHeight, 0 };

			for (int i = 0; i < segments; ++i) {
				const float a1 = TAU * float(i) / float(segments);
				const float a2 = TAU * float(i + 1) / float(segments);
				const float u1 = float(i) / float(segments);
				const float u2 = float(i + 1) / float(segments);

				const glm::vec3 p1{ radius * cos(a1), -halfHeight, radius * sin(a1) };
				const glm::vec3 p2{ radius * cos(a2), -halfHeight, radius * sin(a2) };

				// SIDE NORMAL IS NOT RADIAL. It tilts by the slope: for a cone of
				// radius r and height h the surface normal is
				// normalize(cos a, r/h, sin a). Using the radial normal makes the
				// cone shade like a cylinder.
				const glm::vec3 n1 = glm::normalize(glm::vec3(cos(a1), radius / height, sin(a1)));
				const glm::vec3 n2 = glm::normalize(glm::vec3(cos(a2), radius / height, sin(a2)));
				const glm::vec3 nApex = glm::normalize(n1 + n2);

				b.tri(b.vert(p1, n1, {u1,0}), b.vert(p2, n2, {u2,0}),
				      b.vert(apex, nApex, {0.5f * (u1 + u2), 1}));

				// Bottom cap
				auto capUV = [&](const glm::vec3& p) {
					return glm::vec2(p.x / (2 * radius) + 0.5f, p.z / (2 * radius) + 0.5f);
				};
				const glm::vec3 bc{ 0, -halfHeight, 0 };
				b.face(bc, p2, p1, { 0,-1, 0 }, capUV(bc), capUV(p2), capUV(p1));
			}

			b.finish();
			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CONE), b.vertices, b.tris, "Cone");
		}

		LOG_ENGINE_INFO("CreatePrimitiveMeshes: created all primitive meshes");
	}
}
