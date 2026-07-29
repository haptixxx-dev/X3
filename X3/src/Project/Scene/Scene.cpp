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
		if (source.HasComponent<ConstraintComponent>()) {
			auto& constraint = duplicate.GetOrAddComponent<ConstraintComponent>();
			constraint = source.GetComponent<ConstraintComponent>();
			// Note: connectedEntity reference won't be valid after duplication
			// User needs to reassign the connected entity manually
			constraint.connectedEntity = entt::null;
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
			if (src->any_of<ConstraintComponent>(srcEntity)) {
				dst->emplace_or_replace<ConstraintComponent>(dstEntity, src->get<ConstraintComponent>(srcEntity));
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
		// Version 2: MaterialComponent became a slot vector (Phase 2). The
		// READER branches on the presence of the "Slots" key rather than on this
		// number -- that is more robust against a hand-edited file -- but the key
		// is written so a human can tell the two shapes apart at a glance.
		<< YAML::Key << "SceneVersion" << YAML::Value << 2
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
			
			// Material Component -- one entry per submesh material slot.
			if (entity.HasComponent<MaterialComponent>()) {
				auto& mc = entity.GetComponent<MaterialComponent>();
				out << YAML::Key << "MaterialComponent" << YAML::Value
				<< YAML::BeginMap
					<< YAML::Key << "Slots" << YAML::Value << YAML::BeginSeq;
				for (const MaterialDesc& slot : mc.slots) {
					out << YAML::BeginMap
						<< YAML::Key << "Emission" << YAML::Value << YAML::Flow
						<< YAML::BeginSeq << slot.emission.x << slot.emission.y << slot.emission.z << slot.emission.w << YAML::EndSeq

						<< YAML::Key << "Color" << YAML::Value << YAML::Flow
						<< YAML::BeginSeq << slot.color.x << slot.color.y << slot.color.z << slot.color.w << YAML::EndSeq

						<< YAML::Key << "Metallic"    << YAML::Value << slot.metallic
						<< YAML::Key << "Roughness"   << YAML::Value << slot.roughness
						<< YAML::Key << "AO"          << YAML::Value << slot.ao
						<< YAML::Key << "NormalScale" << YAML::Value << slot.normalScale

						// GUIDs as uint64; 0 is LR_GUID::INVALID, i.e. no texture.
						<< YAML::Key << "BaseColorTex"  << YAML::Value << static_cast<uint64_t>(slot.baseColorTex)
						<< YAML::Key << "NormalTex"     << YAML::Value << static_cast<uint64_t>(slot.normalTex)
						<< YAML::Key << "MetalRoughTex" << YAML::Value << static_cast<uint64_t>(slot.metalRoughTex)
						<< YAML::Key << "EmissiveTex"   << YAML::Value << static_cast<uint64_t>(slot.emissiveTex)

						// Extended lobes. Always written, even at their defaults:
						// they are cheap in the file and a missing key that reads
						// back as a default is indistinguishable from one that was
						// deliberately set to it.
						<< YAML::Key << "SpecularLevel"  << YAML::Value << slot.specularLevel
						<< YAML::Key << "Clearcoat"      << YAML::Value << slot.clearcoat
						<< YAML::Key << "ClearcoatRough" << YAML::Value << slot.clearcoatRough
						<< YAML::Key << "SheenColor" << YAML::Value << YAML::Flow
						<< YAML::BeginSeq << slot.sheenColor.x << slot.sheenColor.y << slot.sheenColor.z << YAML::EndSeq
						<< YAML::Key << "SheenRoughness" << YAML::Value << slot.sheenRoughness
						<< YAML::Key << "Anisotropy"     << YAML::Value << slot.anisotropy
					<< YAML::EndMap;
				}
				out << YAML::EndSeq
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
					<< YAML::Key << "UseCCD" << YAML::Value << rb.useCCD
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

			// Constraint Component
			if (entity.HasComponent<ConstraintComponent>()) {
				auto& con = entity.GetComponent<ConstraintComponent>();
				// Get the GUID of the connected entity if it's valid
				uint64_t connectedGuid = 0;
				if (con.connectedEntity != entt::null) {
					EntityHandle connectedHandle(con.connectedEntity, scene->GetRegistry());
					if (connectedHandle.HasComponent<IDComponent>()) {
						connectedGuid = static_cast<uint64_t>(connectedHandle.GetComponent<IDComponent>().guid);
					}
				}

				out << YAML::Key << "ConstraintComponent" << YAML::Value
				<< YAML::BeginMap
					<< YAML::Key << "Type" << YAML::Value << static_cast<int>(con.type)
					<< YAML::Key << "ConnectedEntityGuid" << YAML::Value << connectedGuid

					<< YAML::Key << "AnchorA" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << con.anchorA.x << con.anchorA.y << con.anchorA.z << YAML::EndSeq

					<< YAML::Key << "AnchorB" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << con.anchorB.x << con.anchorB.y << con.anchorB.z << YAML::EndSeq

					<< YAML::Key << "Axis" << YAML::Value << YAML::Flow
					<< YAML::BeginSeq << con.axis.x << con.axis.y << con.axis.z << YAML::EndSeq

					<< YAML::Key << "LimitsEnabled" << YAML::Value << con.limitsEnabled
					<< YAML::Key << "LimitMin" << YAML::Value << con.limitMin
					<< YAML::Key << "LimitMax" << YAML::Value << con.limitMax
					<< YAML::Key << "ConeHalfAngle" << YAML::Value << con.coneHalfAngle
					<< YAML::Key << "MinDistance" << YAML::Value << con.minDistance
					<< YAML::Key << "MaxDistance" << YAML::Value << con.maxDistance

					<< YAML::Key << "MotorEnabled" << YAML::Value << con.motorEnabled
					<< YAML::Key << "MotorTargetVelocity" << YAML::Value << con.motorTargetVelocity
					<< YAML::Key << "MotorMaxForce" << YAML::Value << con.motorMaxForce

					<< YAML::Key << "Breakable" << YAML::Value << con.breakable
					<< YAML::Key << "BreakForce" << YAML::Value << con.breakForce
					<< YAML::Key << "BreakTorque" << YAML::Value << con.breakTorque
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

					auto readSlot = [&](const YAML::Node& n) {
						MaterialDesc d;
						d.emission      = getVec4(n["Emission"], "Emission");
						d.color         = getVec4(n["Color"], "Color");
						d.metallic      = getScalar(n["Metallic"], 0.0f, "Metallic");
						d.roughness     = getScalar(n["Roughness"], 0.5f, "Roughness");
						d.ao            = getScalar(n["AO"], 1.0f, "AO");
						d.normalScale   = getScalar(n["NormalScale"], 1.0f, "NormalScale");
						d.baseColorTex  = static_cast<LR_GUID>(getScalar(n["BaseColorTex"], uint64_t(0), "BaseColorTex"));
						d.normalTex     = static_cast<LR_GUID>(getScalar(n["NormalTex"], uint64_t(0), "NormalTex"));
						d.metalRoughTex = static_cast<LR_GUID>(getScalar(n["MetalRoughTex"], uint64_t(0), "MetalRoughTex"));
						d.emissiveTex   = static_cast<LR_GUID>(getScalar(n["EmissiveTex"], uint64_t(0), "EmissiveTex"));

						// Extended lobes. Absent in a version-1 file and in every
						// scene written before Phase 6, so each falls back to the
						// value that keeps the material on the BASE tier -- an old
						// scene must not silently acquire a clearcoat.
						d.specularLevel  = getScalar(n["SpecularLevel"], 0.5f, "SpecularLevel");
						d.clearcoat      = getScalar(n["Clearcoat"], 0.0f, "Clearcoat");
						d.clearcoatRough = getScalar(n["ClearcoatRough"], 0.1f, "ClearcoatRough");
						d.sheenColor     = getVec3(n["SheenColor"], "SheenColor");
						d.sheenRoughness = getScalar(n["SheenRoughness"], 0.3f, "SheenRoughness");
						d.anisotropy     = getScalar(n["Anisotropy"], 0.0f, "Anisotropy");
						return d;
					};

					mc.slots.clear();
					if (mnode["Slots"] && mnode["Slots"].IsSequence()) {
						for (auto slotNode : mnode["Slots"])
							mc.slots.push_back(readSlot(slotNode));
					}
					else {
						// LEGACY (SceneVersion 1): one flat material at the top
						// level. Read it into slot 0. This branch is permanent --
						// it is ten lines and it is what makes every pre-Phase-2
						// scene file open unchanged. Saving rewrites the file in
						// the new shape, which is one-way and fine.
						mc.slots.push_back(readSlot(mnode));
					}
					// The component's invariant: never empty.
					if (mc.slots.empty())
						mc.slots.push_back(MaterialDesc{});
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
					rb.useCCD = getScalar(rnode["UseCCD"], false, "UseCCD");
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

				if (entityNode["ConstraintComponent"]) {
					auto& con = entity.GetOrAddComponent<ConstraintComponent>();
					auto cnode = entityNode["ConstraintComponent"];
					con.type = static_cast<ConstraintType>(getScalar(cnode["Type"], 0, "Type"));
					// ConnectedEntityGuid will be resolved in a second pass below
					con.anchorA = getVec3(cnode["AnchorA"], "AnchorA");
					con.anchorB = getVec3(cnode["AnchorB"], "AnchorB");
					con.axis = getVec3(cnode["Axis"], "Axis");
					if (con.axis == glm::vec3(0.0f)) con.axis = glm::vec3(0.0f, 1.0f, 0.0f);
					con.limitsEnabled = getScalar(cnode["LimitsEnabled"], false, "LimitsEnabled");
					con.limitMin = getScalar(cnode["LimitMin"], -180.0f, "LimitMin");
					con.limitMax = getScalar(cnode["LimitMax"], 180.0f, "LimitMax");
					con.coneHalfAngle = getScalar(cnode["ConeHalfAngle"], 45.0f, "ConeHalfAngle");
					con.minDistance = getScalar(cnode["MinDistance"], 0.0f, "MinDistance");
					con.maxDistance = getScalar(cnode["MaxDistance"], 1.0f, "MaxDistance");
					con.motorEnabled = getScalar(cnode["MotorEnabled"], false, "MotorEnabled");
					con.motorTargetVelocity = getScalar(cnode["MotorTargetVelocity"], 0.0f, "MotorTargetVelocity");
					con.motorMaxForce = getScalar(cnode["MotorMaxForce"], 1000.0f, "MotorMaxForce");
					con.breakable = getScalar(cnode["Breakable"], false, "Breakable");
					con.breakForce = getScalar(cnode["BreakForce"], 10000.0f, "BreakForce");
					con.breakTorque = getScalar(cnode["BreakTorque"], 10000.0f, "BreakTorque");
					con.connectedEntity = entt::null; // Will be resolved below
				}
			}

			// Second pass: resolve constraint connected entity GUIDs to entity handles
			for (auto entityNode : entitiesNode) {
				if (entityNode["ConstraintComponent"]) {
					auto cnode = entityNode["ConstraintComponent"];
					uint64_t connectedGuid = getScalar(cnode["ConnectedEntityGuid"], uint64_t(0), "ConnectedEntityGuid");

					if (connectedGuid != 0) {
						// Find the entity with this GUID
						auto entityGuid = static_cast<LR_GUID>(getScalar(entityNode["IDComponent"], (uint64_t)LR_GUID{}, "IDComponent"));

						for (auto [e, id] : scene->GetRegistry()->view<IDComponent>().each()) {
							if (static_cast<uint64_t>(id.guid) == static_cast<uint64_t>(entityGuid)) {
								// Found our constraint entity, now find the connected entity
								for (auto [connectedE, connectedId] : scene->GetRegistry()->view<IDComponent>().each()) {
									if (static_cast<uint64_t>(connectedId.guid) == connectedGuid) {
										EntityHandle handle(e, scene->GetRegistry());
										if (handle.HasComponent<ConstraintComponent>()) {
											handle.GetComponent<ConstraintComponent>().connectedEntity = connectedE;
										}
										break;
									}
								}
								break;
							}
						}
					}
				}
			}
		}

		LOG_ENGINE_INFO("LoadSceneFile: successfully loaded scene from {0}", scenepath.string());
		return scene;
	}

}