#include "Scene.h"
#include "Project/Scene/Components.h"

namespace Laura
{

	EntityHandle Scene::CreateEntity(const std::string& name) {
		entt::entity entityID = m_Registry->create();
		EntityHandle entity(entityID, m_Registry);
		entity.GetOrAddComponent<IDComponent>();
		entity.GetOrAddComponent<TagComponent>(name);
		return entity;
	}

	EntityHandle Scene::CreateEntityWithGuid(LR_GUID guid, const std::string& name) {
		entt::entity entityID = m_Registry->create();
		EntityHandle entity(entityID, m_Registry);
		entity.GetOrAddComponent<IDComponent>(guid);
		entity.GetOrAddComponent<TagComponent>(name);
		return entity;
	}

	void Scene::DestroyEntity(EntityHandle entity) {
		m_Registry->destroy(entity.GetEnttID());
	}


	void Scene::OnStart() {
	}


	void Scene::OnUpdate() {
	}


	void Scene::OnShutdown() {
	}


	std::shared_ptr<Scene> Scene::Copy(std::shared_ptr<Scene> other) {
		auto newScene = std::make_shared<Scene>();

		newScene->guid = other->guid;
		newScene->name = other->name;
		newScene->skyboxGuid = other->skyboxGuid;
		newScene->skyboxName = other->skyboxName;

		auto* src = other->m_Registry;
		auto* dst = newScene->m_Registry;

		auto view = src->view<IDComponent, TagComponent>();
		
		for (auto [srcEntity, id, tag] : view.each()) {
			entt::entity dstEntity = newScene->CreateEntityWithGuid(id.guid, tag.Tag).GetEnttID();
			// IDComponent and TagComponent already copied on CreateEntityWithGuid

			if (src->any_of<TransformComponent>(srcEntity)) {
				dst->emplace_or_replace<TransformComponent>(dstEntity, src->get<TransformComponent>(srcEntity));
			}
			if (src->any_of<CameraComponent>(srcEntity)) {
				dst->emplace_or_replace<CameraComponent>(dstEntity, src->get<CameraComponent>(srcEntity));
			}
			if (src->any_of<MeshComponent>(srcEntity)) {
				dst->emplace_or_replace<MeshComponent>(dstEntity, src->get<MeshComponent>(srcEntity));
			}
			if (src->any_of<MaterialComponent>(srcEntity)) {
				dst->emplace_or_replace<MaterialComponent>(dstEntity, src->get<MaterialComponent>(srcEntity));
			}
		}

		return newScene;
	}

	bool SaveSceneFile(const std::filesystem::path& scenepath, std::shared_ptr<const Scene> scene) {
		if (!(scenepath.has_extension() && scenepath.extension() == SCENE_FILE_EXTENSION)) {
			LOG_ENGINE_WARN("SaveSceneFile: invalid file extension '{}'.", scenepath.string());
			return false;
		}

		if (!std::filesystem::exists(scenepath.parent_path())) {
			LOG_ENGINE_WARN("SaveSceneFile: parent directory '{}' does not exist.", scenepath.parent_path().string());
			return false;
		}

		YAML::Emitter out;
		out << YAML::BeginMap
		<< YAML::Key << "SceneGuid"  << YAML::Value << static_cast<uint64_t>(scene->guid)
		<< YAML::Key << "SceneName"  << YAML::Value << scene->name
		<< YAML::Key << "SkyboxGuid" << YAML::Value << static_cast<uint64_t>(scene->skyboxGuid)
		<< YAML::Key << "SkyboxName" << YAML::Value << scene->skyboxName
		<< YAML::Key << "Entities" << YAML::Value << YAML::BeginSeq;
		// iterate over all entities
		for (auto& e : scene->GetRegistry()->view<entt::entity>()) {
			EntityHandle entity{ e, scene->GetRegistry() };
			out << YAML::BeginMap;

			// Tag component 
			if (entity.HasComponent<TagComponent>()) {
				out << YAML::Key << "TagComponent" << YAML::Value << entity.GetComponent<TagComponent>().Tag;
			}
			if (entity.HasComponent<IDComponent>()) {
				out << YAML::Key << "IDComponent" << YAML::Value << (uint64_t)entity.GetComponent<IDComponent>().guid;
			}

			// Transform component 
			if (entity.HasComponent<TransformComponent>()) {
				auto& tc = entity.GetComponent<TransformComponent>();
				glm::vec3 translation	= tc.GetTranslation();
				glm::vec3 rotation		= tc.GetRotation();
				glm::vec3 scale			= tc.GetScale();

				out << YAML::Key << "TransformComponent" << YAML::Value 
				<< YAML::BeginMap
					<< YAML::Key << "Translation" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << translation.x << translation.y << translation.z << YAML::EndSeq

					<< YAML::Key << "Rotation" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << rotation.x << rotation.y << rotation.z << YAML::EndSeq

					<< YAML::Key << "Scale" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << scale.x << scale.y << scale.z << YAML::EndSeq
				<< YAML::EndMap;
			}

			// Camera component 
			if (entity.HasComponent<CameraComponent>()) {
				auto& cc = entity.GetComponent<CameraComponent>();
				out << YAML::Key << "CameraComponent" << YAML::Value 
				<< YAML::BeginMap
					<< YAML::Key << "IsMain" << YAML::Value << cc.isMain
					<< YAML::Key << "Fov"    << YAML::Value << cc.fov 
				<< YAML::EndMap;
			}

			// Mesh component 
			if (entity.HasComponent<MeshComponent>()) {
				auto& mc = entity.GetComponent<MeshComponent>();
				out << YAML::Key << "MeshComponent" << YAML::Value 
				<< YAML::BeginMap
					<< YAML::Key << "SourceName" << YAML::Value << mc.sourceName
					<< YAML::Key << "MeshGuid"   << YAML::Value << static_cast<uint64_t>(mc.guid)
				<< YAML::EndMap;
			}
			
			// Material Component
			if (entity.HasComponent<MaterialComponent>()) {
				auto& mc = entity.GetComponent<MaterialComponent>();
				out << YAML::Key << "MaterialComponent" << YAML::Value
				<< YAML::BeginMap
					<< YAML::Key << "Emission" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << mc.emission.x << mc.emission.y << mc.emission.z << mc.emission.w << YAML::EndSeq

					<< YAML::Key << "Color" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << mc.color.x << mc.color.y << mc.color.z << mc.color.w << YAML::EndSeq
				<< YAML::EndMap;
			}

			out << YAML::EndMap;
		}
		out << YAML::EndSeq;
		out << YAML::EndMap;

		// write to scenefile
		std::ofstream fout(scenepath);
		if (!fout) {
			LOG_ENGINE_WARN("SaveSceneFile: failed to open file '{}'.", scenepath.string());
			return false;
		}
		fout << out.c_str();
		LOG_ENGINE_INFO("SaveSceneFile: successfully saved scene to {0}", scenepath.string());
		return true;
	}


	std::shared_ptr<Scene> LoadSceneFile(const std::filesystem::path& scenepath) {
		auto getScalar = [](const YAML::Node& node, auto defaultValue, const char* name) {
			using T = decltype(defaultValue);
			if (!node) {
				LOG_ENGINE_WARN("Missing node for '{}', using default", name);
				return defaultValue;
			}
			try {
				return node.as<T>();
			}
			catch (const YAML::Exception& e) {
				LOG_ENGINE_WARN("Bad value for '{}': {}, using default", name, e.what());
				return defaultValue;
			}
		};

		auto getVec3 = [&](const YAML::Node& node, const char* name) {
			if (!node || !node.IsSequence() || node.size() < 3) {
				LOG_ENGINE_WARN("Bad or missing vec3 '{}', using default", name);
				return glm::vec3(0.0f);
			}
			return glm::vec3(
				getScalar(node[0], 0.0f, (std::string(name) + "[0]").c_str()),
				getScalar(node[1], 0.0f, (std::string(name) + "[1]").c_str()),
				getScalar(node[2], 0.0f, (std::string(name) + "[2]").c_str())
			);
		};

		auto getVec4 = [&](const YAML::Node& node, const char* name) {
			if (!node || !node.IsSequence() || node.size() < 4) {
				LOG_ENGINE_WARN("Bad or missing vec4 '{}', using default", name);
				return glm::vec4(0.0f);
			}
			return glm::vec4(
				getScalar(node[0], 0.0f, (std::string(name) + "[0]").c_str()),
				getScalar(node[1], 0.0f, (std::string(name) + "[1]").c_str()),
				getScalar(node[2], 0.0f, (std::string(name) + "[2]").c_str()),
				getScalar(node[3], 0.0f, (std::string(name) + "[3]").c_str())
			);
		};

		LOG_ENGINE_INFO("Deserializing: {0}", scenepath.string());

		if (!(std::filesystem::exists(scenepath) &&
			  std::filesystem::is_regular_file(scenepath) &&
			  scenepath.has_extension() &&
			  scenepath.extension() == SCENE_FILE_EXTENSION))
		{
			LOG_ENGINE_WARN("LoadSceneFile: invalid or missing scene file: {0}", scenepath.string());
			return nullptr;
		}

		YAML::Node root;
		try {
			root = YAML::LoadFile(scenepath.string());
		}
		catch (const YAML::Exception& e) {
			LOG_ENGINE_ERROR("LoadSceneFile: YAML parse error while reading {0}: {1}", scenepath.string(), e.what());
			return nullptr;
		}

		auto scene = std::make_shared<Scene>();

		scene->guid        = static_cast<LR_GUID>(getScalar(root["SceneGuid"], uint64_t(0), "SceneGuid"));
		scene->name        = getScalar(root["SceneName"], std::string("Untitled Scene"), "SceneName");
		scene->skyboxGuid  = static_cast<LR_GUID>(getScalar(root["SkyboxGuid"], uint64_t(0), "SkyboxGuid"));
		scene->skyboxName  = getScalar(root["SkyboxName"], std::string(""), "SkyboxName");

		auto entitiesNode = root["Entities"];
		if (!entitiesNode || !entitiesNode.IsSequence()) {
			LOG_ENGINE_WARN("No 'Entities' array in scene file");
		} else {
			for (auto entityNode : entitiesNode) {
				auto name = getScalar(entityNode["TagComponent"], std::string("Unnamed Entity"), "TagComponent");
				auto guid = static_cast<LR_GUID>(getScalar(entityNode["IDComponent"], (uint64_t)LR_GUID{}, "IDComponent")); // give a random guid if missing
				EntityHandle entity = scene->CreateEntityWithGuid(guid, name);

				if (entityNode["TransformComponent"]) {
					auto& tc = entity.GetOrAddComponent<TransformComponent>();
					auto tnode = entityNode["TransformComponent"];
					tc.SetTranslation(getVec3(tnode["Translation"], "Translation"));
					tc.SetRotation   (getVec3(tnode["Rotation"], "Rotation"));
					tc.SetScale      (getVec3(tnode["Scale"], "Scale"));
				}

				if (entityNode["CameraComponent"]) {
					auto& cc = entity.GetOrAddComponent<CameraComponent>();
					auto cnode = entityNode["CameraComponent"];
					cc.isMain = getScalar(cnode["IsMain"], false, "IsMain");
					cc.fov    = getScalar(cnode["Fov"], 60.0f, "Fov");
				}

				if (entityNode["MeshComponent"]) {
					auto& mc = entity.GetOrAddComponent<MeshComponent>();
					auto mnode = entityNode["MeshComponent"];
					mc.sourceName = getScalar(mnode["SourceName"], std::string(""), "SourceName");
					mc.guid       = static_cast<LR_GUID>(getScalar(mnode["MeshGuid"], uint64_t(0), "MeshGuid"));
				}

				if (entityNode["MaterialComponent"]) {
					auto& mc = entity.GetOrAddComponent<MaterialComponent>();
					auto mnode = entityNode["MaterialComponent"];
					mc.emission = getVec4(mnode["Emission"], "Emission");
					mc.color    = getVec4(mnode["Color"], "Color");
				}
			}
		}

		LOG_ENGINE_INFO("LoadSceneFile: successfully loaded scene from {0}", scenepath.string());
		return scene;
	}

}
