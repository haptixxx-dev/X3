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


	bool AssetManager::LoadMesh(const std::filesystem::path& assetpath, LR_GUID guid) {
		auto timerStart = std::chrono::high_resolution_clock::now();

		if (!m_AssetPool) {
			LOG_ENGINE_CRITICAL("LoadMesh: called without a valid AssetPool for asset {0}", assetpath.string());
			return false;
		}

		Assimp::Importer importer;
		const aiScene* scene = importer.ReadFile(assetpath.string(), aiProcessPreset_TargetRealtime_MaxQuality);
		if (!scene) {
			LOG_ENGINE_CRITICAL("LoadMesh: failed to load assimp scene from {0} (GUID {1})", assetpath.string(), (uint64_t)guid);
			return false;
		}

		size_t triCount = 0;
		for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
			triCount += scene->mMeshes[i]->mNumFaces;

		std::vector<Triangle>& meshBuffer = m_AssetPool->MeshBuffer;

		auto metadata = std::make_shared<MeshMetadata>();
		metadata->firstTriIdx = meshBuffer.size();
		metadata->TriCount = triCount;

		auto metadataExtension = std::make_shared<MeshMetadataExtension>();
		metadataExtension->sourcePath = assetpath;
		metadataExtension->fileSizeInBytes = std::filesystem::file_size(assetpath);

		meshBuffer.reserve(meshBuffer.size() + triCount);

		for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
			const aiMesh* subMesh = scene->mMeshes[i];
			const aiVector3D* verts = subMesh->mVertices;

			for (unsigned int j = 0; j < subMesh->mNumFaces; ++j) {
				const aiFace& face = subMesh->mFaces[j];
				if (face.mNumIndices != 3) continue;

				const auto& idx = face.mIndices;
				meshBuffer.emplace_back(Triangle({
					glm::vec4(verts[idx[0]].x, verts[idx[0]].y, verts[idx[0]].z, 0.0f),
					glm::vec4(verts[idx[1]].x, verts[idx[1]].y, verts[idx[1]].z, 0.0f),
					glm::vec4(verts[idx[2]].x, verts[idx[2]].y, verts[idx[2]].z, 0.0f)
				}));
			}
		}

		m_AssetPool->MarkUpdated(AssetPool::AssetType::MeshBuffer);

		// Build BVH
		BVHAccel bvh(meshBuffer, metadata->firstTriIdx, metadata->TriCount);
		bvh.Build(m_AssetPool->NodeBuffer, m_AssetPool->IndexBuffer, metadata->firstNodeIdx, metadata->nodeCount);

		m_AssetPool->MarkUpdated(AssetPool::AssetType::NodeBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::IndexBuffer);

		double loadTimeMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - timerStart).count();
		metadataExtension->loadTimeMs = loadTimeMs;

		m_AssetPool->Metadata[guid] = { metadata, metadataExtension };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("LoadMesh: loaded {0} triangles from {1} (GUID {2}) in {3:.2f} ms", 
			triCount, assetpath.string(), (uint64_t)guid, loadTimeMs);
		return true;
	}


	bool AssetManager::LoadTexture(const std::filesystem::path& assetpath, LR_GUID guid, int channels) {
		auto timerStart = std::chrono::high_resolution_clock::now();

		if (!m_AssetPool) {
			LOG_ENGINE_CRITICAL("LoadTexture: called without a valid AssetPool for asset {0}", assetpath.string());
			return false;
		}

		int width, height, channelsInFile;
		stbi_set_flip_vertically_on_load(1); // OpenGL-style orientation
		unsigned char* data = stbi_load(assetpath.string().c_str(), &width, &height, &channelsInFile, channels);
		if (!data) {
			LOG_ENGINE_CRITICAL("LoadTexture: failed to load texture from path={0} (requested channels={1}) for GUID={2}.",
				assetpath.string(), channels, (uint64_t)guid);
			return false;
		}

		const int actualChannels = (channels == 0) ? channelsInFile : channels;
		const size_t totalBytes = width * height * actualChannels;

		std::vector<unsigned char>& textureBuffer = m_AssetPool->TextureBuffer;
		textureBuffer.reserve(textureBuffer.size() + totalBytes);
		textureBuffer.insert(textureBuffer.end(), data, data + totalBytes);
		stbi_image_free(data);

		auto metadata = std::make_shared<TextureMetadata>();
		metadata->texStartIdx = textureBuffer.size() - totalBytes;
		metadata->width = width;
		metadata->height = height;
		metadata->channels = actualChannels;

		auto metadataExt = std::make_shared<TextureMetadataExtension>();
		metadataExt->sourcePath = assetpath;
		metadataExt->fileSizeInBytes = std::filesystem::file_size(assetpath);

		double loadTimeMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - timerStart).count();
		metadataExt->loadTimeMs = loadTimeMs;

		m_AssetPool->Metadata[guid] = { metadata, metadataExt };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::TextureBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("LoadTexture: loaded texture {0} (GUID {1}) {2}x{3} with {4} channels in {5:.2f} ms",
			assetpath.string(), (uint64_t)guid, width, height, actualChannels, loadTimeMs);
		return true;
	}


	// ============================================================================
	// PRIMITIVE MESH GENERATION
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

	void AssetManager::CreatePrimitiveMesh(LR_GUID guid, const std::vector<Triangle>& triangles, const char* name) {
		std::vector<Triangle>& meshBuffer = m_AssetPool->MeshBuffer;

		auto metadata = std::make_shared<MeshMetadata>();
		metadata->firstTriIdx = meshBuffer.size();
		metadata->TriCount = triangles.size();

		auto metadataExtension = std::make_shared<MeshMetadataExtension>();
		metadataExtension->sourcePath = std::string("primitive://") + name;
		metadataExtension->fileSizeInBytes = triangles.size() * sizeof(Triangle);
		metadataExtension->loadTimeMs = 0.0f;

		meshBuffer.insert(meshBuffer.end(), triangles.begin(), triangles.end());
		m_AssetPool->MarkUpdated(AssetPool::AssetType::MeshBuffer);

		// Build BVH
		BVHAccel bvh(meshBuffer, metadata->firstTriIdx, metadata->TriCount);
		bvh.Build(m_AssetPool->NodeBuffer, m_AssetPool->IndexBuffer, metadata->firstNodeIdx, metadata->nodeCount);

		m_AssetPool->MarkUpdated(AssetPool::AssetType::NodeBuffer);
		m_AssetPool->MarkUpdated(AssetPool::AssetType::IndexBuffer);

		m_AssetPool->Metadata[guid] = { metadata, metadataExtension };
		m_AssetPool->MarkUpdated(AssetPool::AssetType::Metadata);

		LOG_ENGINE_INFO("CreatePrimitiveMesh: created '{}' with {} triangles (GUID {})",
			name, triangles.size(), static_cast<uint64_t>(guid));
	}

	void AssetManager::CreatePrimitiveMeshes() {
		// Helper lambda to create a triangle
		auto makeTri = [](glm::vec3 a, glm::vec3 b, glm::vec3 c) -> Triangle {
			return Triangle{
				glm::vec4(a, 0.0f),
				glm::vec4(b, 0.0f),
				glm::vec4(c, 0.0f)
			};
		};

		// ========================================
		// CUBE (unit cube centered at origin)
		// ========================================
		{
			std::vector<Triangle> tris;
			const float s = 0.5f; // half-size

			// Front face (z = +s)
			tris.push_back(makeTri({-s, -s, s}, {s, -s, s}, {s, s, s}));
			tris.push_back(makeTri({-s, -s, s}, {s, s, s}, {-s, s, s}));
			// Back face (z = -s)
			tris.push_back(makeTri({s, -s, -s}, {-s, -s, -s}, {-s, s, -s}));
			tris.push_back(makeTri({s, -s, -s}, {-s, s, -s}, {s, s, -s}));
			// Right face (x = +s)
			tris.push_back(makeTri({s, -s, s}, {s, -s, -s}, {s, s, -s}));
			tris.push_back(makeTri({s, -s, s}, {s, s, -s}, {s, s, s}));
			// Left face (x = -s)
			tris.push_back(makeTri({-s, -s, -s}, {-s, -s, s}, {-s, s, s}));
			tris.push_back(makeTri({-s, -s, -s}, {-s, s, s}, {-s, s, -s}));
			// Top face (y = +s)
			tris.push_back(makeTri({-s, s, s}, {s, s, s}, {s, s, -s}));
			tris.push_back(makeTri({-s, s, s}, {s, s, -s}, {-s, s, -s}));
			// Bottom face (y = -s)
			tris.push_back(makeTri({-s, -s, -s}, {s, -s, -s}, {s, -s, s}));
			tris.push_back(makeTri({-s, -s, -s}, {s, -s, s}, {-s, -s, s}));

			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CUBE), tris, "Cube");
		}

		// ========================================
		// SPHERE (UV sphere, radius 0.5)
		// ========================================
		{
			std::vector<Triangle> tris;
			const float radius = 0.5f;
			const int stacks = 16;
			const int slices = 24;

			for (int i = 0; i < stacks; ++i) {
				float theta1 = glm::pi<float>() * float(i) / float(stacks);
				float theta2 = glm::pi<float>() * float(i + 1) / float(stacks);

				for (int j = 0; j < slices; ++j) {
					float phi1 = 2.0f * glm::pi<float>() * float(j) / float(slices);
					float phi2 = 2.0f * glm::pi<float>() * float(j + 1) / float(slices);

					glm::vec3 p1 = radius * glm::vec3(sin(theta1) * cos(phi1), cos(theta1), sin(theta1) * sin(phi1));
					glm::vec3 p2 = radius * glm::vec3(sin(theta1) * cos(phi2), cos(theta1), sin(theta1) * sin(phi2));
					glm::vec3 p3 = radius * glm::vec3(sin(theta2) * cos(phi2), cos(theta2), sin(theta2) * sin(phi2));
					glm::vec3 p4 = radius * glm::vec3(sin(theta2) * cos(phi1), cos(theta2), sin(theta2) * sin(phi1));

					if (i != 0) tris.push_back(makeTri(p1, p2, p3));
					if (i != stacks - 1) tris.push_back(makeTri(p1, p3, p4));
				}
			}

			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::SPHERE), tris, "Sphere");
		}

		// ========================================
		// PLANE (1x1 plane on XZ, y = 0)
		// ========================================
		{
			std::vector<Triangle> tris;
			const float s = 0.5f;

			tris.push_back(makeTri({-s, 0, s}, {s, 0, s}, {s, 0, -s}));
			tris.push_back(makeTri({-s, 0, s}, {s, 0, -s}, {-s, 0, -s}));

			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE), tris, "Plane");
		}

		// ========================================
		// CYLINDER (radius 0.5, height 1)
		// ========================================
		{
			std::vector<Triangle> tris;
			const float radius = 0.5f;
			const float halfHeight = 0.5f;
			const int segments = 24;

			for (int i = 0; i < segments; ++i) {
				float angle1 = 2.0f * glm::pi<float>() * float(i) / float(segments);
				float angle2 = 2.0f * glm::pi<float>() * float(i + 1) / float(segments);

				glm::vec3 b1 = {radius * cos(angle1), -halfHeight, radius * sin(angle1)};
				glm::vec3 b2 = {radius * cos(angle2), -halfHeight, radius * sin(angle2)};
				glm::vec3 t1 = {radius * cos(angle1), halfHeight, radius * sin(angle1)};
				glm::vec3 t2 = {radius * cos(angle2), halfHeight, radius * sin(angle2)};

				// Side faces
				tris.push_back(makeTri(b1, b2, t2));
				tris.push_back(makeTri(b1, t2, t1));

				// Bottom cap
				tris.push_back(makeTri({0, -halfHeight, 0}, b2, b1));

				// Top cap
				tris.push_back(makeTri({0, halfHeight, 0}, t1, t2));
			}

			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CYLINDER), tris, "Cylinder");
		}

		// ========================================
		// CAPSULE (radius 0.25, total height 1)
		// ========================================
		{
			std::vector<Triangle> tris;
			const float radius = 0.25f;
			const float cylinderHalfHeight = 0.25f; // cylinder part
			const int stacks = 8;
			const int slices = 16;

			// Cylinder body
			for (int i = 0; i < slices; ++i) {
				float angle1 = 2.0f * glm::pi<float>() * float(i) / float(slices);
				float angle2 = 2.0f * glm::pi<float>() * float(i + 1) / float(slices);

				glm::vec3 b1 = {radius * cos(angle1), -cylinderHalfHeight, radius * sin(angle1)};
				glm::vec3 b2 = {radius * cos(angle2), -cylinderHalfHeight, radius * sin(angle2)};
				glm::vec3 t1 = {radius * cos(angle1), cylinderHalfHeight, radius * sin(angle1)};
				glm::vec3 t2 = {radius * cos(angle2), cylinderHalfHeight, radius * sin(angle2)};

				tris.push_back(makeTri(b1, b2, t2));
				tris.push_back(makeTri(b1, t2, t1));
			}

			// Top hemisphere
			for (int i = 0; i < stacks / 2; ++i) {
				float theta1 = glm::pi<float>() * float(i) / float(stacks);
				float theta2 = glm::pi<float>() * float(i + 1) / float(stacks);

				for (int j = 0; j < slices; ++j) {
					float phi1 = 2.0f * glm::pi<float>() * float(j) / float(slices);
					float phi2 = 2.0f * glm::pi<float>() * float(j + 1) / float(slices);

					glm::vec3 p1 = glm::vec3(radius * sin(theta1) * cos(phi1), cylinderHalfHeight + radius * cos(theta1), radius * sin(theta1) * sin(phi1));
					glm::vec3 p2 = glm::vec3(radius * sin(theta1) * cos(phi2), cylinderHalfHeight + radius * cos(theta1), radius * sin(theta1) * sin(phi2));
					glm::vec3 p3 = glm::vec3(radius * sin(theta2) * cos(phi2), cylinderHalfHeight + radius * cos(theta2), radius * sin(theta2) * sin(phi2));
					glm::vec3 p4 = glm::vec3(radius * sin(theta2) * cos(phi1), cylinderHalfHeight + radius * cos(theta2), radius * sin(theta2) * sin(phi1));

					if (i != 0) tris.push_back(makeTri(p1, p2, p3));
					tris.push_back(makeTri(p1, p3, p4));
				}
			}

			// Bottom hemisphere
			for (int i = stacks / 2; i < stacks; ++i) {
				float theta1 = glm::pi<float>() * float(i) / float(stacks);
				float theta2 = glm::pi<float>() * float(i + 1) / float(stacks);

				for (int j = 0; j < slices; ++j) {
					float phi1 = 2.0f * glm::pi<float>() * float(j) / float(slices);
					float phi2 = 2.0f * glm::pi<float>() * float(j + 1) / float(slices);

					glm::vec3 p1 = glm::vec3(radius * sin(theta1) * cos(phi1), -cylinderHalfHeight + radius * cos(theta1), radius * sin(theta1) * sin(phi1));
					glm::vec3 p2 = glm::vec3(radius * sin(theta1) * cos(phi2), -cylinderHalfHeight + radius * cos(theta1), radius * sin(theta1) * sin(phi2));
					glm::vec3 p3 = glm::vec3(radius * sin(theta2) * cos(phi2), -cylinderHalfHeight + radius * cos(theta2), radius * sin(theta2) * sin(phi2));
					glm::vec3 p4 = glm::vec3(radius * sin(theta2) * cos(phi1), -cylinderHalfHeight + radius * cos(theta2), radius * sin(theta2) * sin(phi1));

					tris.push_back(makeTri(p1, p2, p3));
					if (i != stacks - 1) tris.push_back(makeTri(p1, p3, p4));
				}
			}

			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CAPSULE), tris, "Capsule");
		}

		// ========================================
		// CONE (radius 0.5, height 1)
		// ========================================
		{
			std::vector<Triangle> tris;
			const float radius = 0.5f;
			const float halfHeight = 0.5f;
			const int segments = 24;

			glm::vec3 apex = {0, halfHeight, 0};

			for (int i = 0; i < segments; ++i) {
				float angle1 = 2.0f * glm::pi<float>() * float(i) / float(segments);
				float angle2 = 2.0f * glm::pi<float>() * float(i + 1) / float(segments);

				glm::vec3 b1 = {radius * cos(angle1), -halfHeight, radius * sin(angle1)};
				glm::vec3 b2 = {radius * cos(angle2), -halfHeight, radius * sin(angle2)};

				// Side face
				tris.push_back(makeTri(b1, b2, apex));

				// Bottom cap
				tris.push_back(makeTri({0, -halfHeight, 0}, b2, b1));
			}

			CreatePrimitiveMesh(static_cast<LR_GUID>(PrimitiveMeshGUIDs::CONE), tris, "Cone");
		}

		LOG_ENGINE_INFO("CreatePrimitiveMeshes: created all primitive meshes");
	}
}