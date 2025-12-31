#include "Scene.h"
#include "Project/Scene/Components.h"

namespace X3
{

	EntityHandle Scene::CreateEntity(const std::string& name) {
		entt::entity entityID = m_Registry->create();
		EntityHandle entity(entityID, m_Registry);
		entity.GetOrAddComponent<IDComponent>();
		entity.GetOrAddComponent<TagComponent>(name);
		entity.GetOrAddComponent<TransformComponent>(); // Always add transform by default
		return entity;
	}

	EntityHandle Scene::CreateEntityWithGuid(LR_GUID guid, const std::string& name) {
		entt::entity entityID = m_Registry->create();
		EntityHandle entity(entityID, m_Registry);
		entity.GetOrAddComponent<IDComponent>(guid);
		entity.GetOrAddComponent<TagComponent>(name);
		entity.GetOrAddComponent<TransformComponent>(); // Always add transform by default
		return entity;
	}

	EntityHandle Scene::DuplicateEntity(EntityHandle source) {
		// Create new entity with a new GUID
		std::string newName = source.GetComponent<TagComponent>().Tag + " (Copy)";
		EntityHandle duplicate = CreateEntity(newName);

		// Copy all components from source to duplicate
		if (source.HasComponent<TransformComponent>()) {
			duplicate.GetComponent<TransformComponent>() = source.GetComponent<TransformComponent>();
		}
		if (source.HasComponent<CameraComponent>()) {
			auto& cam = duplicate.GetOrAddComponent<CameraComponent>();
			cam = source.GetComponent<CameraComponent>();
			cam.isMain = false; // Don't duplicate main camera status
		}
		if (source.HasComponent<MeshComponent>()) {
			duplicate.GetOrAddComponent<MeshComponent>() = source.GetComponent<MeshComponent>();
		}
		if (source.HasComponent<MaterialComponent>()) {
			duplicate.GetOrAddComponent<MaterialComponent>() = source.GetComponent<MaterialComponent>();
		}
		if (source.HasComponent<LightComponent>()) {
			duplicate.GetOrAddComponent<LightComponent>() = source.GetComponent<LightComponent>();
		}
		if (source.HasComponent<RigidBodyComponent>()) {
			duplicate.GetOrAddComponent<RigidBodyComponent>() = source.GetComponent<RigidBodyComponent>();
		}
		if (source.HasComponent<ColliderComponent>()) {
			duplicate.GetOrAddComponent<ColliderComponent>() = source.GetComponent<ColliderComponent>();
		}
		if (source.HasComponent<CharacterControllerComponent>()) {
			duplicate.GetOrAddComponent<CharacterControllerComponent>() = source.GetComponent<CharacterControllerComponent>();
		}

		return duplicate;
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
			if (src->any_of<LightComponent>(srcEntity)) {
				dst->emplace_or_replace<LightComponent>(dstEntity, src->get<LightComponent>(srcEntity));
			}
			if (src->any_of<RigidBodyComponent>(srcEntity)) {
				dst->emplace_or_replace<RigidBodyComponent>(dstEntity, src->get<RigidBodyComponent>(srcEntity));
			}
			if (src->any_of<ColliderComponent>(srcEntity)) {
				dst->emplace_or_replace<ColliderComponent>(dstEntity, src->get<ColliderComponent>(srcEntity));
			}
			if (src->any_of<CharacterControllerComponent>(srcEntity)) {
				dst->emplace_or_replace<CharacterControllerComponent>(dstEntity, src->get<CharacterControllerComponent>(srcEntity));
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

					<< YAML::Key << "Metallic" << YAML::Value << mc.metallic
					<< YAML::Key << "Roughness" << YAML::Value << mc.roughness
					<< YAML::Key << "AO" << YAML::Value << mc.ao
				<< YAML::EndMap;
			}

			// Light Component
			if (entity.HasComponent<LightComponent>()) {
				auto& lc = entity.GetComponent<LightComponent>();
				out << YAML::Key << "LightComponent" << YAML::Value
				<< YAML::BeginMap
					<< YAML::Key << "Type" << YAML::Value << static_cast<int>(lc.type)

					<< YAML::Key << "Color" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << lc.color.x << lc.color.y << lc.color.z << YAML::EndSeq

					<< YAML::Key << "Intensity" << YAML::Value << lc.intensity
					<< YAML::Key << "Range" << YAML::Value << lc.range
					<< YAML::Key << "Attenuation" << YAML::Value << lc.attenuation
					<< YAML::Key << "InnerConeAngle" << YAML::Value << lc.innerConeAngle
					<< YAML::Key << "OuterConeAngle" << YAML::Value << lc.outerConeAngle
				<< YAML::EndMap;
			}

			// RigidBody Component
			if (entity.HasComponent<RigidBodyComponent>()) {
				auto& rb = entity.GetComponent<RigidBodyComponent>();
				out << YAML::Key << "RigidBodyComponent" << YAML::Value
				<< YAML::BeginMap
					<< YAML::Key << "BodyType" << YAML::Value << static_cast<int>(rb.bodyType)
					<< YAML::Key << "Mass" << YAML::Value << rb.mass
					<< YAML::Key << "LinearDamping" << YAML::Value << rb.linearDamping
					<< YAML::Key << "AngularDamping" << YAML::Value << rb.angularDamping
					<< YAML::Key << "Friction" << YAML::Value << rb.friction
					<< YAML::Key << "Restitution" << YAML::Value << rb.restitution
					<< YAML::Key << "LockRotationX" << YAML::Value << rb.lockRotationX
					<< YAML::Key << "LockRotationY" << YAML::Value << rb.lockRotationY
					<< YAML::Key << "LockRotationZ" << YAML::Value << rb.lockRotationZ
					<< YAML::Key << "LockPositionX" << YAML::Value << rb.lockPositionX
					<< YAML::Key << "LockPositionY" << YAML::Value << rb.lockPositionY
					<< YAML::Key << "LockPositionZ" << YAML::Value << rb.lockPositionZ
					<< YAML::Key << "CollisionLayer" << YAML::Value << rb.collisionLayer
					<< YAML::Key << "CollisionMask" << YAML::Value << rb.collisionMask
					<< YAML::Key << "GravityScale" << YAML::Value << rb.gravityScale
				<< YAML::EndMap;
			}

			// Collider Component
			if (entity.HasComponent<ColliderComponent>()) {
				auto& col = entity.GetComponent<ColliderComponent>();
				out << YAML::Key << "ColliderComponent" << YAML::Value
				<< YAML::BeginMap
					<< YAML::Key << "Shape" << YAML::Value << static_cast<int>(col.shape)

					<< YAML::Key << "BoxHalfExtents" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << col.boxHalfExtents.x << col.boxHalfExtents.y << col.boxHalfExtents.z << YAML::EndSeq

					<< YAML::Key << "SphereRadius" << YAML::Value << col.sphereRadius
					<< YAML::Key << "CapsuleRadius" << YAML::Value << col.capsuleRadius
					<< YAML::Key << "CapsuleHalfHeight" << YAML::Value << col.capsuleHalfHeight
					<< YAML::Key << "MeshGuid" << YAML::Value << static_cast<uint64_t>(col.meshGuid)

					<< YAML::Key << "Offset" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << col.offset.x << col.offset.y << col.offset.z << YAML::EndSeq

					<< YAML::Key << "RotationOffset" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << col.rotationOffset.x << col.rotationOffset.y << col.rotationOffset.z << YAML::EndSeq

					<< YAML::Key << "IsTrigger" << YAML::Value << col.isTrigger
				<< YAML::EndMap;
			}

			// CharacterController Component
			if (entity.HasComponent<CharacterControllerComponent>()) {
				auto& cc = entity.GetComponent<CharacterControllerComponent>();
				out << YAML::Key << "CharacterControllerComponent" << YAML::Value
				<< YAML::BeginMap
					<< YAML::Key << "CapsuleRadius" << YAML::Value << cc.capsuleRadius
					<< YAML::Key << "CapsuleHeight" << YAML::Value << cc.capsuleHeight
					<< YAML::Key << "MaxSlopeAngle" << YAML::Value << cc.maxSlopeAngle
					<< YAML::Key << "MaxStepHeight" << YAML::Value << cc.maxStepHeight
					<< YAML::Key << "WalkSpeed" << YAML::Value << cc.walkSpeed
					<< YAML::Key << "SprintSpeed" << YAML::Value << cc.sprintSpeed
					<< YAML::Key << "JumpForce" << YAML::Value << cc.jumpForce
					<< YAML::Key << "Mass" << YAML::Value << cc.mass
					<< YAML::Key << "SkinWidth" << YAML::Value << cc.skinWidth
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
					mc.metallic = getScalar(mnode["Metallic"], 0.0f, "Metallic");
					mc.roughness = getScalar(mnode["Roughness"], 0.5f, "Roughness");
					mc.ao = getScalar(mnode["AO"], 1.0f, "AO");
				}

				if (entityNode["LightComponent"]) {
					auto& lc = entity.GetOrAddComponent<LightComponent>();
					auto lnode = entityNode["LightComponent"];
					lc.type = static_cast<LightType>(getScalar(lnode["Type"], 0, "Type"));
					lc.color = getVec3(lnode["Color"], "Color");
					lc.intensity = getScalar(lnode["Intensity"], 1.0f, "Intensity");
					lc.range = getScalar(lnode["Range"], 10.0f, "Range");
					lc.attenuation = getScalar(lnode["Attenuation"], 1.0f, "Attenuation");
					lc.innerConeAngle = getScalar(lnode["InnerConeAngle"], 30.0f, "InnerConeAngle");
					lc.outerConeAngle = getScalar(lnode["OuterConeAngle"], 45.0f, "OuterConeAngle");
				}

				if (entityNode["RigidBodyComponent"]) {
					auto& rb = entity.GetOrAddComponent<RigidBodyComponent>();
					auto rnode = entityNode["RigidBodyComponent"];
					rb.bodyType = static_cast<BodyType>(getScalar(rnode["BodyType"], 2, "BodyType"));
					rb.mass = getScalar(rnode["Mass"], 1.0f, "Mass");
					rb.linearDamping = getScalar(rnode["LinearDamping"], 0.0f, "LinearDamping");
					rb.angularDamping = getScalar(rnode["AngularDamping"], 0.05f, "AngularDamping");
					rb.friction = getScalar(rnode["Friction"], 0.5f, "Friction");
					rb.restitution = getScalar(rnode["Restitution"], 0.0f, "Restitution");
					rb.lockRotationX = getScalar(rnode["LockRotationX"], false, "LockRotationX");
					rb.lockRotationY = getScalar(rnode["LockRotationY"], false, "LockRotationY");
					rb.lockRotationZ = getScalar(rnode["LockRotationZ"], false, "LockRotationZ");
					rb.lockPositionX = getScalar(rnode["LockPositionX"], false, "LockPositionX");
					rb.lockPositionY = getScalar(rnode["LockPositionY"], false, "LockPositionY");
					rb.lockPositionZ = getScalar(rnode["LockPositionZ"], false, "LockPositionZ");
					rb.collisionLayer = getScalar(rnode["CollisionLayer"], (uint16_t)1, "CollisionLayer");
					rb.collisionMask = getScalar(rnode["CollisionMask"], (uint16_t)0xFFFF, "CollisionMask");
					rb.gravityScale = getScalar(rnode["GravityScale"], 1.0f, "GravityScale");
				}

				if (entityNode["ColliderComponent"]) {
					auto& col = entity.GetOrAddComponent<ColliderComponent>();
					auto cnode = entityNode["ColliderComponent"];
					col.shape = static_cast<ColliderShape>(getScalar(cnode["Shape"], 0, "Shape"));
					col.boxHalfExtents = getVec3(cnode["BoxHalfExtents"], "BoxHalfExtents");
					if (col.boxHalfExtents == glm::vec3(0.0f)) col.boxHalfExtents = glm::vec3(0.5f);
					col.sphereRadius = getScalar(cnode["SphereRadius"], 0.5f, "SphereRadius");
					col.capsuleRadius = getScalar(cnode["CapsuleRadius"], 0.25f, "CapsuleRadius");
					col.capsuleHalfHeight = getScalar(cnode["CapsuleHalfHeight"], 0.5f, "CapsuleHalfHeight");
					col.meshGuid = static_cast<LR_GUID>(getScalar(cnode["MeshGuid"], uint64_t(0), "MeshGuid"));
					col.offset = getVec3(cnode["Offset"], "Offset");
					col.rotationOffset = getVec3(cnode["RotationOffset"], "RotationOffset");
					col.isTrigger = getScalar(cnode["IsTrigger"], false, "IsTrigger");
				}

				if (entityNode["CharacterControllerComponent"]) {
					auto& cc = entity.GetOrAddComponent<CharacterControllerComponent>();
					auto ccnode = entityNode["CharacterControllerComponent"];
					cc.capsuleRadius = getScalar(ccnode["CapsuleRadius"], 0.3f, "CapsuleRadius");
					cc.capsuleHeight = getScalar(ccnode["CapsuleHeight"], 1.8f, "CapsuleHeight");
					cc.maxSlopeAngle = getScalar(ccnode["MaxSlopeAngle"], 45.0f, "MaxSlopeAngle");
					cc.maxStepHeight = getScalar(ccnode["MaxStepHeight"], 0.3f, "MaxStepHeight");
					cc.walkSpeed = getScalar(ccnode["WalkSpeed"], 5.0f, "WalkSpeed");
					cc.sprintSpeed = getScalar(ccnode["SprintSpeed"], 8.0f, "SprintSpeed");
					cc.jumpForce = getScalar(ccnode["JumpForce"], 5.0f, "JumpForce");
					cc.mass = getScalar(ccnode["Mass"], 70.0f, "Mass");
					cc.skinWidth = getScalar(ccnode["SkinWidth"], 0.02f, "SkinWidth");
				}
			}
		}

		LOG_ENGINE_INFO("LoadSceneFile: successfully loaded scene from {0}", scenepath.string());
		return scene;
	}

}