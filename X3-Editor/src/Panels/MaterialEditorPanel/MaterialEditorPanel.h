#pragma once

// =============================================================================
// MaterialEditorPanel -- ENGINE_PLAN.md Phase 13, "material editor reflecting
// the Phase 6 struct, with live preview".
//
// WHAT A "MATERIAL IN THE ASSET POOL" ACTUALLY IS, because the panel's whole
// shape follows from it: X3 has NO standalone material asset. AssetPool holds
// meshes and textures; the only MaterialDesc values that live in the pool are
// MeshMetadata::importedMaterials -- one per dense material slot, filled by
// AssetManager::ImportMaterial from the model file's aiMaterials. So a material
// is identified by (mesh GUID, slot index), which is exactly what MaterialRef
// below is, and the list is a flattened mesh x slot table rather than a folder
// of material files.
//
// The second source of MaterialDesc is MaterialComponent::slots -- a per-entity
// override, which is what InspectorPanel edits. This panel deliberately does
// NOT duplicate that: the Inspector owns overrides, this panel owns the shared
// imported material, and the difference between them is the single most
// confusing thing about the material system, so it is stated in the UI rather
// than left for the user to discover by editing the wrong one.
// =============================================================================

#include "X3.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

#include <string>
#include <vector>

namespace X3
{

	class MaterialEditorPanel : public IEditorPanel {
	public:
		MaterialEditorPanel(std::shared_ptr<EditorState> editorState,
		                    std::shared_ptr<ProjectManager> projectManager)
			: m_EditorState(editorState), m_ProjectManager(projectManager) {
		}

		~MaterialEditorPanel() = default;

		virtual void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		/// One row of the left-hand list: a (mesh, slot) pair plus the strings
		/// needed to draw it. `editable` is false for a slot the mesh declares in
		/// materialSlotCount but has no importedMaterials entry for -- the
		/// renderer default-constructs a MaterialDesc for those (Renderer.cpp's
		/// `else { MaterialDescs.emplace_back(); }`), so there is no stored value
		/// to point a widget at.
		struct MaterialRef {
			LR_GUID     meshGuid = LR_GUID::INVALID;
			uint32_t    slot     = 0;
			std::string meshName;
			std::string slotName;
			bool        editable = false;
		};

		std::vector<MaterialRef> CollectMaterials() const;

		/// Counts entities in the OPEN scene whose MaterialComponent overrides
		/// this exact (mesh, slot). Those entities do not see edits made here --
		/// see the precedence comment in Renderer::Parse.
		uint32_t CountSceneOverrides(LR_GUID meshGuid, uint32_t slot) const;

		/// Writes `desc` into every overriding-capable entity in the open scene
		/// that uses `meshGuid`, creating the MaterialComponent if absent.
		/// Returns how many entities were touched. This is the ONLY route by
		/// which an edit made in this panel survives a project reload -- see the
		/// persistence note in the .cpp.
		uint32_t PushToSceneOverrides(LR_GUID meshGuid, uint32_t slot, const MaterialDesc& desc);

		void DrawPreview(const MaterialDesc& desc);
		void DrawFields(MaterialDesc& desc);

		LR_GUID  m_SelectedMeshGuid = LR_GUID::INVALID;
		uint32_t m_SelectedSlot     = 0;
		char     m_Filter[64]       = {};

		/// Result of the last "Copy to scene overrides" press, shown next to the
		/// button so a press that matched nothing is visibly a no-op rather than
		/// silently one.
		int m_LastPushCount = -1;

		std::shared_ptr<EditorState>    m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;
	};
}
