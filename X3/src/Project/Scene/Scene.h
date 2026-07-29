#pragma once

#include <yaml-cpp/yaml.h>
#include "entt/entt.hpp"
#include "Project/Scene/Entity.h"
#include "Project/Scene/Components.h"

namespace X3
{

	// ============================================================================
	// SCENE FILE (.lrscene)
	// ----------------------------------------------------------------------------
	// Represents a scene containing entities, components, and metadata.
	// Serialization and deserialization handled by free functions.
	// ============================================================================
	
	#define SCENE_FILE_EXTENSION ".lrscn"

	class Scene {
	public:
		explicit Scene() {
			m_Registry = new entt::registry();
		}

		~Scene() {
			delete m_Registry;
		}

		// non movable, non copyable
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		EntityHandle CreateEntity(const std::string& name = "Empty Entity");
		EntityHandle CreateEntityWithGuid(LR_GUID guid, const std::string& name);
		EntityHandle DuplicateEntity(EntityHandle entity);

		// Destroys the entity AND ITS ENTIRE SUBTREE. See the orphan-policy comment
		// in Scene.cpp for why children are destroyed rather than promoted.
		void DestroyEntity(EntityHandle entity);

		// ====================================================================
		// HIERARCHY
		// --------------------------------------------------------------------
		// The only sanctioned way to read or write RelationshipComponent. The
		// component stores parent and children in both directions, so a caller
		// writing one field directly would desynchronise the other half.
		// ====================================================================

		/// Reparents 'child' under 'parent'. Pass a default-constructed (invalid)
		/// EntityHandle as 'parent' to detach 'child' to the scene root.
		/// Returns false and changes nothing if the move would create a cycle,
		/// or if either handle is stale. DOES NOT TOUCH THE TRANSFORM -- read the
		/// implementation comment before assuming it should.
		bool SetParent(EntityHandle child, EntityHandle parent);

		/// Direct children, in user-visible sibling order. Returns empty for a
		/// leaf or for an entity with no RelationshipComponent.
		std::vector<EntityHandle> GetChildren(EntityHandle entity) const;

		/// Parent, or an invalid EntityHandle if 'entity' is a root.
		EntityHandle GetParent(EntityHandle entity) const;

		/// True if 'entity' sits anywhere below 'possibleAncestor'. This is the
		/// cycle test SetParent uses; exposed because the editor needs it to grey
		/// out illegal drop targets before the user commits to them.
		bool IsDescendantOf(EntityHandle entity, EntityHandle possibleAncestor) const;

		/// Local matrix of 'entity' composed with every ancestor's, root-first.
		///
		/// NOT CURRENTLY CONSUMED BY THE RENDERER -- Renderer::Parse still pushes
		/// the bare local matrix. This exists so that when the renderer, the
		/// physics body factory and the viewport gizmo do start composing, they
		/// all compose the SAME way rather than growing three subtly different
		/// loops. See the transform note in Components.h.
		glm::mat4 GetWorldMatrix(EntityHandle entity) const;

		void OnStart();
		void OnUpdate();
		void OnShutdown();

		static std::shared_ptr<Scene> Copy(std::shared_ptr<Scene> other);

		inline entt::registry* GetRegistry() const { return m_Registry; }

		LR_GUID		guid;
		std::string name;

		LR_GUID		skyboxGuid;
		std::string skyboxName;

	private:
		/// Unlinks 'child' from its current parent's child list and clears its
		/// parent field. Leaves the (now empty) RelationshipComponent in place --
		/// removing it here would invalidate references held by the caller, and
		/// an empty relationship is harmless.
		void DetachFromParent(entt::entity child);

		/// Copies every component EXCEPT RelationshipComponent from src to dst.
		/// The relationship is deliberately excluded: it holds handles into the
		/// source subtree, and DuplicateEntity rebuilds it through SetParent so
		/// the duplicate's links point at the duplicate's own children.
		void CopyComponentsTo(EntityHandle src, EntityHandle dst) const;

		/// Recursive half of DuplicateEntity. 'isSubtreeRoot' only controls the
		/// " (Copy)" name suffix -- descendants keep their original names, which
		/// is what every DCC tool does and what makes a duplicated rig readable.
		EntityHandle DuplicateEntityRecursive(EntityHandle source, bool isSubtreeRoot);

		entt::registry* m_Registry;

		friend bool SaveSceneFile(const std::filesystem::path& scenepath, std::shared_ptr<const Scene> scene);
		friend std::shared_ptr<Scene> LoadSceneFile(const std::filesystem::path& scenepath);
	};


	// ============================================================================
	// SERIALIZATION / DESERIALIZATION
	// ============================================================================

	/// Saves scene data to 'scenepath' (full path with filename and extension, e.g. "c:/dev/scene.lrscn").
	/// The file will be created or overwritten; it does not need to exist beforehand.
	/// The parent directory must exist; this function does not create directories.
	/// Returns true on success, false otherwise.
	bool SaveSceneFile(const std::filesystem::path& scenepath, std::shared_ptr<const Scene> scene);

	/// Loads a scene from the scene file at 'scenepath'.
	/// 'sceneFilePath' must be the full path including filename and extension.
	/// Returns a shared_ptr to the Scene or nullptr on failure.
	std::shared_ptr<Scene> LoadSceneFile(const std::filesystem::path& scenepath);
}
