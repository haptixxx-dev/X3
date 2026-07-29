#pragma once 

#include "X3.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

namespace X3
{

	class SceneHierarchyPanel : public IEditorPanel {
	public:
		SceneHierarchyPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<ProjectManager> projectManager);
		~SceneHierarchyPanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		/// Draws one row and, if it is open, recurses into its children.
		/// 'panelWidth' is measured ONCE by the caller before any indentation:
		/// ImGui::SameLine takes an offset from the window content origin, not
		/// from the indented cursor, so the trash button lines up at every depth
		/// only if this value does not change as we descend.
		void DrawEntityNode(const std::shared_ptr<Scene>& scene, EntityHandle entity, float panelWidth, float lineHeight);

		/// Number of entities below 'entity'. Shown in the delete prompt because
		/// Scene::DestroyEntity takes the whole subtree and a collapsed parent can
		/// hide any amount of it -- there is no undo.
		size_t CountDescendants(const std::shared_ptr<Scene>& scene, EntityHandle entity) const;

		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;

		// STRUCTURAL EDITS ARE DEFERRED TO THE END OF THE FRAME, NEVER APPLIED
		// MID-WALK. DrawEntityNode is iterating a parent's child vector while it
		// recurses; destroying or reparenting from inside that walk mutates the
		// very vector being iterated (and can reallocate the component storage it
		// lives in), which is a use-after-free that only shows up on the drops
		// that happen to trigger a reallocation.
		entt::entity m_PendingDestroy = entt::null;
		entt::entity m_PendingReparentChild = entt::null;
		entt::entity m_PendingReparentParent = entt::null;  // entt::null == detach to root
		bool m_HasPendingReparent = false;

		// ConfirmAndExecute needs a bool that survives across frames while its
		// modal is open, and a message string that stays alive for the same span.
		bool m_DestroyRequested = false;
		std::string m_DestroyPrompt;
	};
}