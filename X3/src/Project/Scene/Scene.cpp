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
		if (!source.IsValid() || !m_Registry->valid(source.GetEnttID())) {
			LOG_ENGINE_WARN("Scene::DuplicateEntity: stale source handle; nothing duplicated.");
			return EntityHandle{};
		}

		// THE WHOLE SUBTREE, NOT JUST THE ENTITY. Duplicating a parent without its
		// children would silently drop most of what the user selected -- a rig, a
		// modular building, anything assembled out of parts. Every DCC tool and
		// every engine editor duplicates the subtree, so anything else reads as a
		// bug.
		EntityHandle duplicate = DuplicateEntityRecursive(source, true);

		// The copy becomes a SIBLING of the source, not a root. Re-parenting after
		// the subtree is fully built is deliberate: SetParent may emplace a
		// RelationshipComponent, and doing that mid-recursion would invalidate the
		// child vectors the recursion is walking.
		EntityHandle sourceParent = GetParent(source);
		if (sourceParent.IsValid()) {
			SetParent(duplicate, sourceParent);
		}

		return duplicate;
	}

	EntityHandle Scene::DuplicateEntityRecursive(EntityHandle source, bool isSubtreeRoot) {
		std::string newName = source.HasComponent<TagComponent>() ? source.GetComponent<TagComponent>().Tag : std::string("Entity");
		if (isSubtreeRoot) {
			newName += " (Copy)";
		}

		// New GUID: CreateEntity mints one. A duplicate is a NEW entity, and two
		// entities sharing a GUID would make every GUID-keyed lookup (constraint
		// targets, parent links on load) ambiguous.
		EntityHandle duplicate = CreateEntity(newName);
		CopyComponentsTo(source, duplicate);

		// COPY THE CHILD LIST BY VALUE FIRST. The loop body creates entities and
		// calls SetParent, both of which can emplace a RelationshipComponent and
		// therefore reallocate that component's storage -- a reference into
		// source's `children` would dangle mid-iteration. entt invalidates
		// references on emplace of the SAME component type; this is the trap that
		// makes hierarchy code crash intermittently rather than immediately.
		std::vector<EntityHandle> children = GetChildren(source);
		for (EntityHandle child : children) {
			EntityHandle childCopy = DuplicateEntityRecursive(child, false);
			SetParent(childCopy, duplicate);
		}

		return duplicate;
	}

	void Scene::CopyComponentsTo(EntityHandle source, EntityHandle duplicate) const {
		// RelationshipComponent is INTENTIONALLY ABSENT from this list -- it holds
		// handles into the source subtree and is rebuilt by the caller through
		// SetParent. See Scene.h.
		if (source.HasComponent<TransformComponent>()) {
			duplicate.GetOrAddComponent<TransformComponent>() = source.GetComponent<TransformComponent>();
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
		// FirstPersonCamera and FlowState were missing from BOTH this path and
		// Scene::Copy while being read every frame by RuntimeLayer -- duplicating a
		// player entity produced something the runtime would not recognise as a
		// player. They are still absent from Save/LoadSceneFile; that gap is
		// unrelated to the hierarchy work and is flagged rather than silently
		// widened, since adding them changes the file format.
		if (source.HasComponent<FirstPersonCameraComponent>()) {
			duplicate.GetOrAddComponent<FirstPersonCameraComponent>() = source.GetComponent<FirstPersonCameraComponent>();
		}
		if (source.HasComponent<FlowStateComponent>()) {
			duplicate.GetOrAddComponent<FlowStateComponent>() = source.GetComponent<FlowStateComponent>();
		}
		if (source.HasComponent<ConstraintComponent>()) {
			auto& constraint = duplicate.GetOrAddComponent<ConstraintComponent>();
			constraint = source.GetComponent<ConstraintComponent>();
			// Note: connectedEntity reference won't be valid after duplication
			// User needs to reassign the connected entity manually
			constraint.connectedEntity = entt::null;
		}
	}

	// ============================================================================
	// HIERARCHY
	// ============================================================================

	EntityHandle Scene::GetParent(EntityHandle entity) const {
		if (!entity.IsValid() || !m_Registry->valid(entity.GetEnttID())) {
			return EntityHandle{};
		}
		const auto* rel = m_Registry->try_get<RelationshipComponent>(entity.GetEnttID());
		if (!rel || rel->parent == entt::null || !m_Registry->valid(rel->parent)) {
			return EntityHandle{};
		}
		return EntityHandle(rel->parent, m_Registry);
	}

	std::vector<EntityHandle> Scene::GetChildren(EntityHandle entity) const {
		std::vector<EntityHandle> result;
		if (!entity.IsValid() || !m_Registry->valid(entity.GetEnttID())) {
			return result;
		}
		const auto* rel = m_Registry->try_get<RelationshipComponent>(entity.GetEnttID());
		if (!rel) {
			return result;
		}

		result.reserve(rel->children.size());
		for (entt::entity child : rel->children) {
			// Stale handles are skipped rather than trusted. DestroyEntity keeps
			// the list clean, but a scene file can be hand-edited and a future
			// caller may yet destroy through the registry directly; a dangling
			// handle here would be dereferenced by the panel on the very next
			// frame.
			if (child != entt::null && m_Registry->valid(child)) {
				result.emplace_back(child, m_Registry);
			}
		}
		return result;
	}

	bool Scene::IsDescendantOf(EntityHandle entity, EntityHandle possibleAncestor) const {
		if (!entity.IsValid() || !possibleAncestor.IsValid()) {
			return false;
		}

		const entt::entity target = possibleAncestor.GetEnttID();
		entt::entity cursor = entity.GetEnttID();

		// The step cap is not paranoia about our own writes -- SetParent makes a
		// cycle unreachable -- it is about a corrupt scene file or a future caller
		// that pokes RelationshipComponent directly. Without it, the cycle-check
		// that exists to PREVENT an infinite walk would itself be the infinite
		// walk. Bounded by the entity count, since a legal chain can never be
		// longer than that.
		const size_t maxSteps = m_Registry->storage<entt::entity>().size() + 1;
		for (size_t step = 0; step < maxSteps; ++step) {
			if (cursor == entt::null || !m_Registry->valid(cursor)) {
				return false;
			}
			if (cursor == target) {
				return true;
			}
			const auto* rel = m_Registry->try_get<RelationshipComponent>(cursor);
			if (!rel) {
				return false;
			}
			cursor = rel->parent;
		}

		LOG_ENGINE_ERROR("Scene::IsDescendantOf: parent chain exceeded the entity count -- the hierarchy already contains a cycle.");
		return true; // Refuse the move; a cycle is the only way to get here.
	}

	bool Scene::SetParent(EntityHandle child, EntityHandle parent) {
		if (!child.IsValid() || !m_Registry->valid(child.GetEnttID())) {
			LOG_ENGINE_WARN("Scene::SetParent: stale child handle; ignored.");
			return false;
		}

		const entt::entity childID = child.GetEnttID();
		// An invalid or stale parent handle means "detach to root". That is the
		// documented way to unparent, so it is not an error.
		const entt::entity parentID = (parent.IsValid() && m_Registry->valid(parent.GetEnttID()))
			? parent.GetEnttID()
			: entt::null;

		// ------------------------------------------------------------------
		// CYCLE REJECTION
		// ------------------------------------------------------------------
		// Dropping an entity onto itself, or onto one of its own descendants,
		// closes the parent chain into a ring. Nothing about that ring is
		// detectable at the point of damage -- the component fields stay
		// individually well-formed -- but SceneHierarchyPanel::DrawEntityNode
		// recurses into GetChildren unconditionally, so a ring makes it recurse
		// forever: stack overflow, hard crash, no ImGui assert to explain it,
		// and a saved scene file that crashes again the moment it is reopened.
		// GetWorldMatrix would hang the same way climbing upward. Cheaper to
		// refuse the drop here than to defend every walker separately.
		if (parentID == childID) {
			LOG_ENGINE_WARN("Scene::SetParent: refused -- an entity cannot be its own parent (would cycle).");
			return false;
		}
		if (parentID != entt::null && IsDescendantOf(EntityHandle(parentID, m_Registry), child)) {
			LOG_ENGINE_WARN("Scene::SetParent: refused -- cannot parent an entity onto its own descendant (would cycle).");
			return false;
		}

		// ------------------------------------------------------------------
		// TRANSFORMS: DELIBERATELY UNTOUCHED. READ BEFORE "FIXING".
		// ------------------------------------------------------------------
		// TransformComponent::GetMatrix() is documented as local-to-world, but
		// NOTHING composes a parent into it. Renderer::Parse pushes
		// `GetComponent<TransformComponent>().GetMatrix()` straight into
		// TransformBuffer, PhysicsWorld::CreateBody passes it straight to Jolt,
		// and the viewport gizmo edits it in place. In today's engine the local
		// matrix IS the world matrix.
		//
		// Under that rule, preserving the child's world transform across a
		// reparent means changing nothing: the entity's world placement is its
		// own transform and the new parent contributes nothing to it. Rebasing
		// into parent space here (newLocal = inverse(parentWorld) * childWorld)
		// would be the correct thing in a composing engine and is exactly WRONG
		// today -- every consumer would read the rebased local as a world matrix
		// and the object would visibly teleport on drop.
		//
		// The corollary is the loud limitation: parenting is ORGANISATIONAL
		// ONLY. Moving a parent does not move its children. When the renderer
		// starts composing (Scene::GetWorldMatrix is the intended one true
		// implementation), THIS FUNCTION MUST GAIN THE REBASE IN THE SAME
		// COMMIT -- ship one without the other and every existing scene's
		// parented geometry jumps.
		// ------------------------------------------------------------------

		auto* existing = m_Registry->try_get<RelationshipComponent>(childID);
		if (existing && existing->parent == parentID) {
			return true; // Already correct; do not churn sibling order.
		}

		DetachFromParent(childID);

		if (parentID == entt::null) {
			return true; // Detach-to-root is complete.
		}

		// ORDER MATTERS AND IT IS NOT COSMETIC. Each get_or_emplace of the same
		// component type may reallocate that type's storage and invalidate every
		// outstanding reference to it. So: materialise the parent's component,
		// then the child's (using its reference immediately), then re-fetch the
		// parent by value. Holding `parentRel` across the child's emplace is the
		// classic use-after-free in this file.
		static_cast<void>(m_Registry->get_or_emplace<RelationshipComponent>(parentID)); // nodiscard; the reference is deliberately dropped, see above
		m_Registry->get_or_emplace<RelationshipComponent>(childID).parent = parentID;
		m_Registry->get<RelationshipComponent>(parentID).children.push_back(childID);

		return true;
	}

	void Scene::DetachFromParent(entt::entity child) {
		auto* rel = m_Registry->try_get<RelationshipComponent>(child);
		if (!rel || rel->parent == entt::null) {
			return;
		}

		if (m_Registry->valid(rel->parent)) {
			if (auto* parentRel = m_Registry->try_get<RelationshipComponent>(rel->parent)) {
				auto& siblings = parentRel->children;
				siblings.erase(std::remove(siblings.begin(), siblings.end(), child), siblings.end());
			}
		}
		// Re-fetch: nothing above emplaces, but the parent lookup went through
		// the same storage and this keeps the pattern uniform if that changes.
		m_Registry->get<RelationshipComponent>(child).parent = entt::null;
	}

	glm::mat4 Scene::GetWorldMatrix(EntityHandle entity) const {
		if (!entity.IsValid() || !m_Registry->valid(entity.GetEnttID())) {
			return glm::mat4(1.0f);
		}

		// Ancestors collected first, then multiplied root-first, so the result is
		// parentWorld * childLocal at every level -- the same convention as
		// TransformComponent's own T * R * S. Same bounded walk as
		// IsDescendantOf, for the same reason.
		std::vector<entt::entity> chain;
		const size_t maxSteps = m_Registry->storage<entt::entity>().size() + 1;
		entt::entity cursor = entity.GetEnttID();
		for (size_t step = 0; step < maxSteps && cursor != entt::null && m_Registry->valid(cursor); ++step) {
			chain.push_back(cursor);
			const auto* rel = m_Registry->try_get<RelationshipComponent>(cursor);
			cursor = rel ? rel->parent : entt::null;
		}

		glm::mat4 world(1.0f);
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			if (const auto* transform = m_Registry->try_get<TransformComponent>(*it)) {
				world = world * transform->GetMatrix();
			}
		}
		return world;
	}

	void Scene::DestroyEntity(EntityHandle entity) {
		if (!entity.IsValid() || !m_Registry->valid(entity.GetEnttID())) {
			return;
		}

		// ------------------------------------------------------------------
		// ORPHAN POLICY: DESTROY THE SUBTREE.
		// ------------------------------------------------------------------
		// The alternatives were promote-to-root and promote-to-grandparent.
		// Both were rejected for the same reason: a child's transform is
		// authored RELATIVE to its parent, so the moment the renderer starts
		// composing (see SetParent) a promoted child silently relocates to
		// wherever its local transform lands in world space. Deleting a crate
		// would scatter its contents across the level instead of removing them.
		// A policy whose damage only appears after an unrelated future change
		// is the worst kind.
		//
		// Destroying the subtree is also what Unity and Unreal do, so it is what
		// a user's fingers already expect, and it is the only option where the
		// scene after the delete contains no entity the user did not see.
		//
		// The cost: no undo in this editor yet, so a delete on a collapsed
		// parent can remove more than the one visible row. The hierarchy panel
		// therefore states the descendant count in its confirmation prompt --
		// that is the mitigation, and it has to stay.
		// ------------------------------------------------------------------

		// Unlink from the parent FIRST, so the parent's child list never holds a
		// handle to a destroyed entity even for an instant.
		DetachFromParent(entity.GetEnttID());

		// Flatten before destroying. Destroying while walking would free the very
		// RelationshipComponent whose child vector the walk is iterating.
		std::vector<entt::entity> doomed;
		std::vector<entt::entity> stack{ entity.GetEnttID() };
		const size_t maxVisits = m_Registry->storage<entt::entity>().size() + 1;

		while (!stack.empty()) {
			const entt::entity current = stack.back();
			stack.pop_back();

			if (current == entt::null || !m_Registry->valid(current)) {
				continue;
			}
			// A pre-existing cycle (hand-edited file) would otherwise queue
			// entities forever and exhaust memory before it crashed.
			if (doomed.size() > maxVisits) {
				LOG_ENGINE_ERROR("Scene::DestroyEntity: subtree walk exceeded the entity count -- cycle in the hierarchy; aborting the walk.");
				break;
			}
			if (std::find(doomed.begin(), doomed.end(), current) != doomed.end()) {
				continue;
			}
			doomed.push_back(current);

			if (const auto* rel = m_Registry->try_get<RelationshipComponent>(current)) {
				stack.insert(stack.end(), rel->children.begin(), rel->children.end());
			}
		}

		for (entt::entity e : doomed) {
			if (m_Registry->valid(e)) {
				m_Registry->destroy(e);
			}
		}
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

		// SOURCE HANDLE -> DESTINATION HANDLE, built during the component pass and
		// consumed by the reference-fixup pass below. Every component that stores
		// an entt::entity (RelationshipComponent, ConstraintComponent) is
		// MEANINGLESS after a raw copy: dst mints its own handles, and the same
		// numeric value in the new registry is some unrelated entity. Copying such
		// a field verbatim does not crash -- it silently rewires the scene, which
		// is why this map exists rather than a "good enough" assumption that the
		// two registries allocate in lockstep. (They do not: src has holes from
		// destroyed entities, dst does not.)
		std::unordered_map<uint32_t, entt::entity> srcToDst;

		for (auto [srcEntity, id, tag] : view.each()) {
			entt::entity dstEntity = newScene->CreateEntityWithGuid(id.guid, tag.Tag).GetEnttID();
			// IDComponent and TagComponent already copied on CreateEntityWithGuid
			srcToDst.emplace(static_cast<uint32_t>(srcEntity), dstEntity);

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
			// Missing here until the hierarchy work; RuntimeLayer reads both every
			// frame, so a play-mode copy of the scene lost the player's camera
			// tuning and flow state. Same omission as in CopyComponentsTo.
			if (src->any_of<FirstPersonCameraComponent>(srcEntity)) {
				dst->emplace_or_replace<FirstPersonCameraComponent>(dstEntity, src->get<FirstPersonCameraComponent>(srcEntity));
			}
			if (src->any_of<FlowStateComponent>(srcEntity)) {
				dst->emplace_or_replace<FlowStateComponent>(dstEntity, src->get<FlowStateComponent>(srcEntity));
			}
			if (src->any_of<ConstraintComponent>(srcEntity)) {
				// The handle inside is remapped in the fixup pass below.
				dst->emplace_or_replace<ConstraintComponent>(dstEntity, src->get<ConstraintComponent>(srcEntity));
			}
		}

		// ------------------------------------------------------------------
		// PASS 2: TRANSLATE EVERY CROSS-ENTITY HANDLE INTO THE NEW REGISTRY.
		// ------------------------------------------------------------------
		// Must run after pass 1 -- a parent can appear later in the view than its
		// child, so half the targets do not exist yet while pass 1 is running.
		// ------------------------------------------------------------------
		auto remap = [&srcToDst](entt::entity srcHandle) -> entt::entity {
			if (srcHandle == entt::null) {
				return entt::null;
			}
			auto it = srcToDst.find(static_cast<uint32_t>(srcHandle));
			// A miss means the target was not copied (it lacked IDComponent or
			// TagComponent, so pass 1 skipped it). Dropping the link is the only
			// safe answer; keeping the stale handle would point into nothing.
			return it != srcToDst.end() ? it->second : entt::null;
		};

		for (const auto& [srcRaw, dstEntity] : srcToDst) {
			const auto srcEntity = static_cast<entt::entity>(srcRaw);

			if (const auto* srcRel = src->try_get<RelationshipComponent>(srcEntity)) {
				RelationshipComponent dstRel;
				dstRel.parent = remap(srcRel->parent);
				dstRel.children.reserve(srcRel->children.size());
				for (entt::entity child : srcRel->children) {
					const entt::entity mapped = remap(child);
					if (mapped != entt::null) {
						dstRel.children.push_back(mapped); // sibling order preserved
					}
				}
				dst->emplace_or_replace<RelationshipComponent>(dstEntity, std::move(dstRel));
			}

			if (auto* dstConstraint = dst->try_get<ConstraintComponent>(dstEntity)) {
				// Pre-existing bug, fixed here because the remap machinery now
				// exists: this handle used to be copied verbatim, so a constraint
				// in a copied scene pointed at whatever entity happened to own that
				// handle value in the new registry -- usually the wrong body, never
				// an error.
				dstConstraint->connectedEntity = remap(src->get<ConstraintComponent>(srcEntity).connectedEntity);
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

		// GUID of an entity handle, or 0 for "no entity". Cross-entity references
		// CANNOT be written as entt handles: they are registry-local and change on
		// every load. GUIDs are the only stable identity a scene file has, which is
		// what ConstraintComponent already does and what RelationshipComponent
		// follows.
		auto guidOf = [&scene](entt::entity e) -> uint64_t {
			if (e == entt::null || !scene->GetRegistry()->valid(e)) {
				return 0;
			}
			const auto* id = scene->GetRegistry()->try_get<IDComponent>(e);
			return id ? static_cast<uint64_t>(id->guid) : 0;
		};

		YAML::Emitter out;
		out << YAML::BeginMap
		// Version 2: MaterialComponent became a slot vector (Phase 2). The
		// READER branches on the presence of the "Slots" key rather than on this
		// number -- that is more robust against a hand-edited file -- but the key
		// is written so a human can tell the two shapes apart at a glance.
		//
		// Version 3: RelationshipComponent (parent/child hierarchy). Purely
		// additive -- a version-2 file simply has no such key and loads as a scene
		// of all roots, which is exactly what it was. The number is again
		// informational; the reader branches on key presence.
		<< YAML::Key << "SceneVersion" << YAML::Value << 3
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

			// Relationship component -- parent link and ordered child list.
			//
			// BOTH ARE WRITTEN EVEN THOUGH EITHER ALONE DETERMINES THE TREE.
			// "Children" carries the sibling ORDER, which parent links alone cannot
			// express; "ParentGuid" makes each entity's place readable on its own
			// line, which matters for a format people hand-edit and diff. The
			// reader treats "Children" as authoritative and uses "ParentGuid" only
			// to rescue entities no parent claimed -- see LoadSceneFile.
			if (entity.HasComponent<RelationshipComponent>()) {
				const auto& rel = entity.GetComponent<RelationshipComponent>();
				// A relationship that is empty in both directions is a leftover
				// from a detach; writing it would grow every scene file for
				// nothing and make "is a root" ambiguous on load.
				if (rel.parent != entt::null || !rel.children.empty()) {
					out << YAML::Key << "RelationshipComponent" << YAML::Value
					<< YAML::BeginMap
						<< YAML::Key << "ParentGuid" << YAML::Value << guidOf(rel.parent)
						<< YAML::Key << "Children" << YAML::Value << YAML::Flow << YAML::BeginSeq;
					for (entt::entity child : rel.children) {
						const uint64_t childGuid = guidOf(child);
						if (childGuid != 0) { // an entity with no IDComponent cannot be referenced
							out << childGuid;
						}
					}
					out << YAML::EndSeq
					<< YAML::EndMap;
				}
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
					<< YAML::Key << "SoftnessDegrees" << YAML::Value << lc.softnessDegrees
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
			// INDEX-ALIGNED WITH entitiesNode. The later passes need to get from a
			// YAML node back to the entity it produced, and doing that by GUID
			// lookup is subtly broken: an entity with no IDComponent key is given a
			// FRESH RANDOM GUID here, and re-reading that key later produces a
			// different random value, so the lookup misses. Position is exact.
			std::vector<EntityHandle> created;
			created.reserve(entitiesNode.size());
			// GUID -> entity, for resolving references BETWEEN entities. Built from
			// the guid actually assigned, not from the node, for the reason above.
			// Replaces a nested scan that was O(entities^2) per reference.
			std::unordered_map<uint64_t, entt::entity> byGuid;

			for (auto entityNode : entitiesNode) {
				auto name = getScalar(entityNode["TagComponent"], std::string("Unnamed Entity"), "TagComponent");
				auto guid = static_cast<LR_GUID>(getScalar(entityNode["IDComponent"], (uint64_t)LR_GUID{}, "IDComponent")); // give a random guid if missing
				EntityHandle entity = scene->CreateEntityWithGuid(guid, name);
				created.push_back(entity);
				byGuid.emplace(static_cast<uint64_t>(guid), entity.GetEnttID());

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
					// Defaults to 0 -- a hard shadow -- so every scene authored before
					// soft shadows existed keeps rendering exactly as it did.
					lc.softnessDegrees = getScalar(lnode["SoftnessDegrees"], 0.0f, "SoftnessDegrees");
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

			// ==============================================================
			// SECOND PASS: RESOLVE CROSS-ENTITY GUID REFERENCES.
			// --------------------------------------------------------------
			// Cannot be folded into the first pass: a reference may point at an
			// entity that appears LATER in the file and does not exist yet.
			// ==============================================================

			auto resolve = [&byGuid](uint64_t guid) -> entt::entity {
				if (guid == 0) {
					return entt::null;
				}
				auto it = byGuid.find(guid);
				return it != byGuid.end() ? it->second : entt::null;
			};

			// --- Hierarchy, from the child lists (they carry sibling order) ---
			// Every link goes through Scene::SetParent rather than writing the
			// component directly, so a hand-edited or corrupted file gets the same
			// cycle rejection a user drag would: a bad link is dropped with a
			// warning instead of producing a scene that stack-overflows the
			// hierarchy panel the instant it is drawn.
			for (size_t i = 0; i < created.size(); ++i) {
				auto relNode = entitiesNode[i]["RelationshipComponent"];
				if (!relNode) {
					continue;
				}
				auto childrenNode = relNode["Children"];
				if (!childrenNode || !childrenNode.IsSequence()) {
					continue;
				}
				for (auto childNode : childrenNode) {
					const uint64_t childGuid = getScalar(childNode, uint64_t(0), "RelationshipComponent.Children");
					const entt::entity child = resolve(childGuid);
					if (child == entt::null) {
						LOG_ENGINE_WARN("LoadSceneFile: child GUID {} not found in this scene; link dropped.", childGuid);
						continue;
					}
					scene->SetParent(EntityHandle(child, scene->GetRegistry()), created[i]);
				}
			}

			// --- Hierarchy rescue, from the parent links ---
			// Only for entities no parent's child list claimed: a hand-edit that
			// sets ParentGuid without updating the parent's Children would
			// otherwise silently lose the link. Runs as its own pass because a
			// parent's child list may be read after the child's own node.
			// Appending is the right position -- the file expressed no order.
			for (size_t i = 0; i < created.size(); ++i) {
				auto relNode = entitiesNode[i]["RelationshipComponent"];
				if (!relNode) {
					continue;
				}
				if (scene->GetParent(created[i]).IsValid()) {
					continue; // already linked by the authoritative child list
				}
				const uint64_t parentGuid = getScalar(relNode["ParentGuid"], uint64_t(0), "ParentGuid");
				const entt::entity parent = resolve(parentGuid);
				if (parentGuid != 0 && parent == entt::null) {
					LOG_ENGINE_WARN("LoadSceneFile: parent GUID {} not found in this scene; entity left at root.", parentGuid);
					continue;
				}
				if (parent != entt::null) {
					scene->SetParent(created[i], EntityHandle(parent, scene->GetRegistry()));
				}
			}

			// --- Constraint targets ---
			for (size_t i = 0; i < created.size(); ++i) {
				auto cnode = entitiesNode[i]["ConstraintComponent"];
				if (!cnode || !created[i].HasComponent<ConstraintComponent>()) {
					continue;
				}
				const uint64_t connectedGuid = getScalar(cnode["ConnectedEntityGuid"], uint64_t(0), "ConnectedEntityGuid");
				// entt::null on a miss means "attached to world", which is the
				// documented meaning of the field and the safe fallback.
				created[i].GetComponent<ConstraintComponent>().connectedEntity = resolve(connectedGuid);
			}
		}

		LOG_ENGINE_INFO("LoadSceneFile: successfully loaded scene from {0}", scenepath.string());
		return scene;
	}

}