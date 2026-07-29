#include "Project/Assets/AssetManager.h"
#include "Project/ProjectUtilities.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stb_image/stb_image.h>
#include <yaml-cpp/yaml.h>
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

			// check if the asset exists at the path specified by the .lrmeta file
			if (!std::filesystem::exists(sourcePath)) {
				LOG_ENGINE_WARN("LoadAssetPoolFromFolder: missing asset file for metafile {0}", metapath.string());
				continue;
			}

			// if yes then load the asset from that file
			if (!LoadAssetFile(sourcePath, maybeMetafile->guid)) {
				LOG_ENGINE_WARN("LoadAssetPoolFromFolder: failed to load asset {0}", sourcePath.string());
				continue;
			}

			LOG_ENGINE_INFO("LoadAssetPoolFromFolder: loaded asset {0} with GUID {1}", sourcePath.string(), (uint64_t)maybeMetafile->guid);
		}
	}


	bool AssetManager::LoadAssetFile(const std::filesystem::path& assetpath, LR_GUID guid) {
		if (!std::filesystem::exists(assetpath) || !std::filesystem::is_regular_file(assetpath) || !assetpath.has_extension()) {
			LOG_ENGINE_ERROR("LoadAssetFile: invalid asset path {0}", assetpath.string());
			return false;
		}

		// choose loader based on the extension
		const std::string extension = assetpath.extension().string();
		for (const auto& SUPPORTED_FORMAT : SUPPORTED_MESH_FILE_FORMATS) {
			if (extension == SUPPORTED_FORMAT) {
				LOG_ENGINE_INFO("LoadAssetFile: loading mesh {0} for GUID {1}", assetpath.string(), (uint64_t)guid);
				return LoadMesh(assetpath, guid);
			}
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


	bool AssetManager::LoadMesh(const std::filesystem::path& assetpath, LR_GUID guid) {
		auto timerStart = std::chrono::high_resolution_clock::now();

		if (!m_AssetPool) {
			LOG_ENGINE_CRITICAL("LoadMesh: called without a valid AssetPool for asset {0}", assetpath.string());
			return false;
		}

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
			LOG_ENGINE_CRITICAL("LoadMesh: failed to load assimp scene from {0} (GUID {1})", assetpath.string(), (uint64_t)guid);
			return false;
		}

		std::vector<MeshInstance> instances;
		CollectMeshInstances(scene->mRootNode, glm::mat4(1.0f), instances);
		if (instances.empty()) {
			LOG_ENGINE_WARN("LoadMesh: {0} has no mesh instances under its node graph", assetpath.string());
			return false;
		}

		auto& triPositions = m_AssetPool->TriPositionBuffer;
		auto& triRefs      = m_AssetPool->TriRefBuffer;
		auto& vertices     = m_AssetPool->VertexBuffer;

		auto metadata = std::make_shared<MeshMetadata>();
		metadata->firstTriIdx    = static_cast<uint32_t>(triPositions.size());
		metadata->firstVertexIdx = static_cast<uint32_t>(vertices.size());

		auto metadataExtension = std::make_shared<MeshMetadataExtension>();
		metadataExtension->sourcePath = assetpath;
		metadataExtension->fileSizeInBytes = std::filesystem::file_size(assetpath);

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

			const uint32_t slot = static_cast<uint32_t>(metadata->importedMaterials.size());
			materialSlotOf[m->mMaterialIndex] = slot;
			metadata->importedMaterials.push_back(
				ImportMaterial(scene, scene->mMaterials[m->mMaterialIndex], modelDir));
		}
		metadata->materialSlotCount = static_cast<uint32_t>(metadata->importedMaterials.size());

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

				// GLOBAL vertex indices -- the add happens once here rather than
				// once per hit in the shader.
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

			metadata->submeshes.push_back(SubmeshInfo{
				instanceFirstTri - metadata->firstTriIdx,   // MESH-LOCAL
				instanceTriCount,
				slot,
				m->mName.C_Str() });
		}

		assert(triPositions.size() == triRefs.size() &&
		       "TriPositionBuffer and TriRefBuffer must be appended in lockstep");

		metadata->TriCount    = static_cast<uint32_t>(triPositions.size()) - metadata->firstTriIdx;
		metadata->vertexCount = static_cast<uint32_t>(vertices.size())     - metadata->firstVertexIdx;

		m_AssetPool->MarkUpdated(AssetPool::AssetType::TriPositionBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::TriRefBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::VertexBuffer);

		// Build BVH -- over positions only, exactly as before. The BVH never sees
		// an attribute and must not start to.
		BVHAccel bvh(triPositions, metadata->firstTriIdx, metadata->TriCount);
		bvh.Build(m_AssetPool->NodeBuffer, m_AssetPool->BvhPrimIndexBuffer, metadata->firstNodeIdx, metadata->nodeCount);

		m_AssetPool->MarkUpdated(AssetPool::AssetType::NodeBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::BvhPrimIndexBuffer);

		double loadTimeMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - timerStart).count();
		metadataExtension->loadTimeMs = loadTimeMs;

		m_AssetPool->Metadata[guid] = { metadata, metadataExtension };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("LoadMesh: loaded {0} triangles / {1} vertices / {2} submeshes / {3} materials "
		                "from {4} (GUID {5}) in {6:.2f} ms",
			metadata->TriCount, metadata->vertexCount, metadata->submeshes.size(),
			metadata->materialSlotCount, assetpath.string(), (uint64_t)guid, loadTimeMs);
		return true;
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
		stbi_set_flip_vertically_on_load(0);
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
	                                          bool isSRGB) {
		// A stable, content-addressed GUID so re-importing the same model twice
		// resolves to the same texture asset instead of duplicating it, and so a
		// texture shared by five materials (a glTF ORM map typically is) uploads
		// once. Derived, not random -- LR_GUID's default constructor is random and
		// would defeat both.
		const std::string key = (modelDir / texPath.C_Str()).string();
		const LR_GUID guid{ std::hash<std::string>{}(key) | 0x8000000000000000ULL };

		if (m_AssetPool->Textures.contains(guid))
			return guid;   // already imported, by this model or another material

		auto timerStart = std::chrono::high_resolution_clock::now();

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
				stbi_set_flip_vertically_on_load(0);
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
			stbi_set_flip_vertically_on_load(0);
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
		m_AssetPool->Textures[guid] = std::move(pixels);

		auto metadata = std::make_shared<TextureMetadata>();
		metadata->width    = width;
		metadata->height   = height;
		metadata->channels = 4;
		metadata->isSRGB   = isSRGB;

		auto metadataExt = std::make_shared<TextureMetadataExtension>();
		metadataExt->sourcePath = key;
		// OWNED BY THE MODEL. SaveAssetPoolToFolder must not write a .lrmeta for
		// this: the path is synthetic for embedded images, so the sidecar would
		// point at nothing and the next project open would warn on every one.
		metadataExt->ownedByModel = true;
		metadataExt->loadTimeMs = std::chrono::duration<double, std::milli>(
			std::chrono::high_resolution_clock::now() - timerStart).count();

		m_AssetPool->Metadata[guid] = { metadata, metadataExt };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Textures);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("ResolveModelTexture: imported {0} texture '{1}' {2}x{3} ({4}) as GUID {5}",
			embedded ? "embedded" : "external", texPath.C_Str(), width, height,
			isSRGB ? "sRGB" : "linear", (uint64_t)guid);
		return guid;
	}


	MaterialDesc AssetManager::ImportMaterial(const aiScene* scene, const aiMaterial* mat,
	                                          const std::filesystem::path& modelDir) {
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
			desc.baseColorTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ true);

		if (mat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS)
			desc.normalTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ false);

		// In glTF these two resolve to the same ORM image; whichever assimp
		// reports first is the one to take.
		if (mat->GetTexture(aiTextureType_METALNESS,         0, &texPath) == AI_SUCCESS ||
		    mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS)
			desc.metalRoughTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ false);

		if (mat->GetTexture(aiTextureType_EMISSIVE, 0, &texPath) == AI_SUCCESS)
			desc.emissiveTex = ResolveModelTexture(scene, texPath, modelDir, /*isSRGB*/ true);

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
			void finish() {
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
