#pragma once

// =============================================================================
// LightmapBakePanel -- ENGINE_PLAN.md Phase 13, "lightmap bake UI: trigger,
// progress, preview. Needs to be pleasant or nobody will bake."
//
// WHAT THIS PANEL CAN HONESTLY DO, stated here because the gap between the
// phase's name and the engine's state is the whole design constraint:
//
//   * UV GENERATION -- REAL. GenerateLightmapUVs (Project/Assets/LightmapUV.h)
//     exists, is pure, deterministic and CPU-only, and this panel drives it with
//     every parameter LightmapUVSettings exposes.
//
//   * ATLAS PREVIEW -- REAL. The packer's output is a list of rectangles and a
//     per-triangle-corner UV array. Both draw with ImGui draw-list primitives
//     and need no GPU texture, so the preview shows genuine packing quality
//     rather than a mock-up.
//
//   * PROGRESS -- NOT REAL, AND NOT FAKED. See the long note in the .cpp.
//
//   * THE BAKE ITSELF -- DOES NOT EXIST. There is no pass anywhere in the
//     engine that fills a lightmap: grep for "lightmap" over X3/src returns
//     LightmapUV.{h,cpp}, one AssetCook comment about a future section, and two
//     JobSystem comments about a future Phase 10. The Bake button in this panel
//     is therefore disabled and labelled as such, and the panel says so in a
//     banner rather than only in a comment -- a button that looks like it bakes
//     and does not is worse than no button at all.
//
//   * STORING THE RESULT -- NOWHERE TO PUT IT. MeshMetadata has no lightmap UV
//     field, CookedMesh has no lightmap section, and .lrmeta holds a GUID and a
//     path. The generated UV set therefore lives in this panel object and dies
//     with it. Said in the UI, next to the disabled Apply button.
// =============================================================================

#include "X3.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"
#include "Project/Assets/LightmapUV.h"

#include <string>

namespace X3
{

	class LightmapBakePanel : public IEditorPanel {
	public:
		LightmapBakePanel(std::shared_ptr<EditorState> editorState,
		                  std::shared_ptr<ProjectManager> projectManager)
			: m_EditorState(editorState), m_ProjectManager(projectManager) {
		}

		~LightmapBakePanel() = default;

		virtual void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		/// Above this the per-triangle overlay is disabled. Every triangle adds
		/// three anti-aliased lines -- roughly 18 vertices and 36 indices -- to
		/// the editor's draw data EVERY FRAME, not just on the frame it was
		/// generated. A 100k-triangle mesh is therefore ~3.6M indices per frame
		/// rebuilt and re-uploaded by the ImGui backend, which is a bigger
		/// per-frame cost than the scene render it sits next to. It also pushes
		/// hard on ImDrawIdx, which is 16-bit in this imconfig (line 109 is
		/// commented out) and is only survivable at all because the Vulkan
		/// backend advertises VtxOffset. The chart rectangles are always drawn;
		/// there are orders of magnitude fewer of them.
		static constexpr uint32_t kTriangleOverlayCap = 12000;

		/// Meshes above this get a "this will freeze the editor" warning before
		/// the user commits. Not a hard limit -- the unwrapper is roughly linear
		/// and this is the point where a blocked frame becomes noticeable rather
		/// than a point where anything breaks.
		static constexpr uint32_t kSlowUnwrapTriangles = 50000;

		/// The mesh the panel acts on: the selected entity's MeshComponent.
		/// Returns nullptr (and fills `reason`) when there is nothing to act on,
		/// so every caller disables its control with the same explanation.
		std::shared_ptr<MeshMetadata> ResolveTargetMesh(LR_GUID& guidOut,
		                                                std::string& nameOut,
		                                                std::string& reasonOut) const;

		void DrawSettings();
		void DrawProgress();
		void DrawResultStats();
		void DrawAtlasPreview();

		LightmapUVSettings m_Settings{};
		/// Mirrors "worldUnitsPerTexel == 0" as a checkbox, because 0 meaning
		/// "auto" rather than "infinitely fine" is not discoverable from a
		/// slider.
		bool m_AutoDensity = true;
		float m_ManualDensity = 0.05f;

		LightmapUV  m_Result{};
		bool        m_HasResult      = false;
		LR_GUID     m_ResultMeshGuid = LR_GUID::INVALID;
		std::string m_ResultMeshName;
		uint32_t    m_ResultTriCount = 0;
		double      m_LastRunMs      = 0.0;
		/// The requested chart angle at the time of the run, kept so the panel
		/// can point out when LightmapUV clamped it to [1, 44].
		float       m_RequestedChartAngle = 0.0f;

		bool m_DrawTriangleOverlay = false;
		bool m_DrawGutters         = true;

		std::shared_ptr<EditorState>    m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;
	};
}
