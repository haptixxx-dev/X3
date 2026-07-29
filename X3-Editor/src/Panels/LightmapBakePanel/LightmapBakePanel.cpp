#include <IconsFontAwesome6.h>
#include "Panels/LightmapBakePanel/LightmapBakePanel.h"
#include "Project/Assets/AssetManager.h"
#include "Project/Scene/SceneManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

// =============================================================================
// WHY THERE IS NO PROGRESS BAR THAT MOVES
// -----------------------------------------------------------------------------
// The obvious thing to build here is a bar that fills while the unwrap runs.
// That bar cannot exist yet, and animating one anyway would be a lie with a
// specific cost: the user would read the smooth fill as "it is working, this is
// how long it takes", when in fact the editor is not painting at all and the
// number under the bar came from a timer rather than from the unwrapper.
//
// Three separate things are missing, and all three are needed:
//
//   1. NO CALLBACK. GenerateLightmapUVs's signature is
//      (vertices, tris, firstTri, triCount, settings) -> LightmapUV. There is no
//      progress functor, no cancellation token, and the header states the
//      function is pure and does no logging -- deliberately, so X3LightmapTest
//      can assert on it with no display and no device. Adding one would mean
//      editing X3/src/Project/Assets/LightmapUV.h, which this panel does not
//      own.
//
//   2. NO SECOND THREAD. The call runs inline on the UI thread. While it runs,
//      OnImGuiRender has not returned, no frame is submitted, and the window is
//      not repainted -- so even a bar driven by a real callback would be drawn
//      once, at the end. Phase 4's job system (Core/JobSystem.h, enkiTS) is what
//      moves the unwrap off this thread; its own comments already name the
//      lightmap bake as the reason it exists. Nothing currently plumbs a running
//      job's state back to a panel.
//
//   3. NO CANCEL. Following from (2): with the UI thread inside the call there
//      is no frame in which a Cancel button could be clicked.
//
// So the bar below reports COMPLETION STATE ONLY -- empty before a run,
// full after one -- is drawn disabled, and says why in its tooltip. What the
// panel offers instead is the honest substitute: the measured wall time of the
// last run, and a warning shown BEFORE a run on a mesh large enough for the
// freeze to be noticeable.
//
// -----------------------------------------------------------------------------
// WHY THE ATLAS PREVIEW IS DRAW-LIST GEOMETRY AND NOT A TEXTURE
// -----------------------------------------------------------------------------
// A lightmap preview would normally be an image. There is no image: nothing
// bakes irradiance, so there are no texels to show. What DOES exist after a run
// is exactly the data that answers the question a user actually has at this
// stage -- "did it pack well, and are my charts sane?" -- as rectangles and UV
// triangles. Both are ImGui draw-list primitives: no VkImage, no
// ImGui_ImplVulkan_AddTexture, no descriptor lifetime to manage, and the result
// is sharper than a 512x512 texture scaled into a panel would be.
// =============================================================================

namespace {

	/// Distinct-but-stable chart colour. Golden-ratio hue stepping gives
	/// neighbouring indices very different hues, which matters because charts
	/// packed next to each other in the atlas usually have adjacent indices.
	ImU32 ChartColor(uint32_t index, float alpha) {
		const float hue = std::fmod(static_cast<float>(index) * 0.6180339887f, 1.0f);
		return ImColor::HSV(hue, 0.65f, 0.95f, alpha);   // ImColor::operator ImU32
	}
}

namespace X3
{

	std::shared_ptr<MeshMetadata> LightmapBakePanel::ResolveTargetMesh(LR_GUID& guidOut,
	                                                                  std::string& nameOut,
	                                                                  std::string& reasonOut) const {
		guidOut = LR_GUID::INVALID;
		nameOut.clear();
		reasonOut.clear();

		auto sceneManager = m_ProjectManager->GetSceneManager();
		std::shared_ptr<Scene> scene = sceneManager ? sceneManager->GetOpenScene() : nullptr;
		if (!scene) {
			reasonOut = "No scene is open.";
			return nullptr;
		}

		const entt::entity selected = m_EditorState->temp.selectedEntity;
		// VALIDITY, not just non-null: selectedEntity survives the entity being
		// deleted from the hierarchy panel, and entt::registry::all_of on a dead
		// handle is undefined rather than false.
		if (selected == entt::null || !scene->GetRegistry()->valid(selected)) {
			reasonOut = "No entity selected. Select one in the Scene Hierarchy.";
			return nullptr;
		}

		EntityHandle entity(selected, scene->GetRegistry());
		if (!entity.HasComponent<MeshComponent>()) {
			reasonOut = "The selected entity has no Mesh component, so there is nothing to unwrap.";
			return nullptr;
		}

		const LR_GUID guid = entity.GetComponent<MeshComponent>().guid;
		auto assetPool = m_ProjectManager->GetAssetManager()->GetAssetPool();
		std::shared_ptr<MeshMetadata> mesh = assetPool ? assetPool->find<MeshMetadata>(guid) : nullptr;
		if (!mesh) {
			reasonOut = "The selected entity's mesh GUID is not in the asset pool.";
			return nullptr;
		}
		if (mesh->TriCount == 0) {
			reasonOut = "The selected mesh has no triangles.";
			return nullptr;
		}

		guidOut = guid;
		nameOut = AssetManager::IsPrimitiveMesh(guid)
			? AssetManager::GetPrimitiveMeshName(guid)
			: entity.GetComponent<MeshComponent>().sourceName;
		if (nameOut.empty()) nameOut = guid.string();
		return mesh;
	}

	void LightmapBakePanel::DrawSettings() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;
		constexpr float kLabelWidth = 170.0f;

		auto Label = [&theme](const char* text) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(text);
			theme.PopColor();
			ImGui::SameLine(kLabelWidth);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		};

		// --- Atlas resolution ------------------------------------------------
		// A combo of powers of two rather than a free integer: the packer is a
		// skyline packer over a fixed square bin, and a non-power-of-two bin buys
		// nothing while making every "half the resolution" comparison awkward.
		{
			static constexpr uint32_t kResolutions[] = { 64, 128, 256, 512, 1024, 2048, 4096 };
			static const char* const kResolutionNames[] = {
				"64", "128", "256", "512", "1024", "2048", "4096" };
			int current = 3;
			for (int i = 0; i < IM_ARRAYSIZE(kResolutions); ++i)
				if (kResolutions[i] == m_Settings.atlasResolution) current = i;
			Label("Atlas Resolution:");
			if (ImGui::Combo("##AtlasResolution", &current, kResolutionNames, IM_ARRAYSIZE(kResolutionNames)))
				m_Settings.atlasResolution = kResolutions[current];
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Side length of the square atlas, in texels.\n"
				                  "With auto density this is the budget the texel size is fitted to.");
		}

		// --- Gutter -----------------------------------------------------------
		{
			Label("Gutter (texels):");
			int gutter = static_cast<int>(m_Settings.gutterTexels);
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			if (ImGui::SliderInt("##Gutter", &gutter, 0, 16, "%d", ImGuiSliderFlags_AlwaysClamp))
				m_Settings.gutterTexels = static_cast<uint32_t>(std::max(0, gutter));
			theme.PopColor();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Empty texels reserved on all four sides of every chart.\n"
				                  "2 is the minimum that survives bilinear filtering at mip 0.");

			// The header calls a zero gutter "the single most visible lightmap
			// artefact"; a warning is cheaper than a user rediscovering it.
			if (m_Settings.gutterTexels < 2) {
				theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
				ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Below 2, bilinear filtering blends each chart's "
				                   "edge texels with a neighbouring chart's -- a bright or dark line along every "
				                   "chart boundary that more bake samples will not remove.");
				theme.PopColor();
			}
		}

		// --- Chart angle ------------------------------------------------------
		{
			Label("Max Chart Angle:");
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			// Slider bounds ARE the library's clamp range, so the UI can never
			// request a value that is silently changed underneath it.
			ImGui::SliderFloat("##ChartAngle", &m_Settings.maxChartAngleDegrees, 1.0f, 44.0f, "%.1f deg",
				ImGuiSliderFlags_AlwaysClamp);
			theme.PopColor();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("How far a face's normal may stray from its chart's seed normal.\n"
				                  "Clamped to [1, 44] by the unwrapper: at twice 45 degrees a face\n"
				                  "projects to zero or negative area and its texels stop existing.\n"
				                  "Tighter means less distortion and many more charts.");
		}

		// --- Texel density ----------------------------------------------------
		{
			Label("Texel Density:");
			ImGui::Checkbox("##AutoDensity", &m_AutoDensity);
			ImGui::SameLine();
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("auto-fit to atlas");
			theme.PopColor();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Auto picks the finest world-units-per-texel whose charts still fit,\n"
				                  "coarsening and re-packing until they do.");

			Label("World Units / Texel:");
			ImGui::BeginDisabled(m_AutoDensity);
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			ImGui::DragFloat("##Density", &m_ManualDensity, 0.001f, 0.0001f, 10.0f, "%.4f",
				ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			theme.PopColor();
			ImGui::EndDisabled();
			if (m_AutoDensity && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Auto-fit is on, so the unwrapper chooses this value.\n"
				                  "The value it chose is reported under Result after a run.");

			// The settings struct encodes auto as 0. Keeping the manual value in
			// its own member means unticking Auto restores what the user last
			// typed instead of 0.
			m_Settings.worldUnitsPerTexel = m_AutoDensity ? 0.0f : m_ManualDensity;
		}

		// --- Auto-fit tuning (only meaningful while auto is on) ---------------
		ImGui::BeginDisabled(!m_AutoDensity);
		{
			Label("Target Atlas Usage:");
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			ImGui::SliderFloat("##TargetUsage", &m_Settings.targetAtlasUsage, 0.05f, 0.95f, "%.2f",
				ImGuiSliderFlags_AlwaysClamp);
			theme.PopColor();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(m_AutoDensity
					? "The fraction of the atlas the FIRST density estimate aims to fill with\n"
					  "chart content, before gutters and packing waste. It is a starting point\n"
					  "for the retry loop, not a prediction."
					: "Only used by the auto-fit, which is off.");

			Label("Max Fit Attempts:");
			int attempts = static_cast<int>(m_Settings.maxFitAttempts);
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			if (ImGui::SliderInt("##FitAttempts", &attempts, 1, 128, "%d", ImGuiSliderFlags_AlwaysClamp))
				m_Settings.maxFitAttempts = static_cast<uint32_t>(std::max(1, attempts));
			theme.PopColor();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(m_AutoDensity
					? "Each attempt coarsens the density by 1.15x and re-packs. Every attempt is\n"
					  "a full re-pack, so raising this raises the worst-case blocking time."
					: "Only used by the auto-fit, which is off.");
		}
		ImGui::EndDisabled();
	}

	void LightmapBakePanel::DrawProgress() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;

		// COMPLETION STATE, NOT PROGRESS. Disabled so it never reads as a live
		// control, and 0-or-1 so it never claims knowledge it does not have. The
		// long note at the top of this file is what the tooltip summarises.
		ImGui::BeginDisabled();
		ImGui::ProgressBar(m_HasResult ? 1.0f : 0.0f,
			ImVec2(-FLT_MIN, 0.0f),
			m_HasResult ? "complete" : "not run");
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("This bar can only be empty or full, and that is deliberate.\n"
			                  "GenerateLightmapUVs is synchronous, has no progress callback, and\n"
			                  "runs on the UI thread -- no frame is drawn while it works, so there\n"
			                  "is no moment at which an intermediate value could be displayed.\n"
			                  "Live progress needs the Phase 4 job system running the unwrap off\n"
			                  "this thread and reporting chart counts back to the panel.");

		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		if (m_HasResult) {
			ImGui::Text("Last run: %.1f ms (measured, wall clock) on %s",
				m_LastRunMs, m_ResultMeshName.c_str());
		}
		else {
			ImGui::TextUnformatted("The editor will not repaint while the unwrap runs.");
		}
		theme.PopColor();
	}

	void LightmapBakePanel::DrawResultStats() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;

		if (!m_HasResult) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextUnformatted("No UV set generated yet.");
			theme.PopColor();
			return;
		}

		if (!m_Result.ok) {
			// The header names exactly two failure modes, so the message names
			// exactly two fixes rather than shrugging.
			theme.PushColor(ImGuiCol_Text, EditorCol_Error);
			ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Generation FAILED. Packing did not converge.");
			theme.PopColor();
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextWrapped("Either the gutter is wider than the atlas (2 x gutter + 1 must be less than the "
			                   "resolution), or the retry budget ran out before the charts fit. Raise Atlas "
			                   "Resolution, lower the Gutter, loosen Max Chart Angle so there are fewer charts, "
			                   "or raise Max Fit Attempts.");
			theme.PopColor();
			return;
		}

		// Two utilisation numbers, because they answer different questions. The
		// padded figure is what the packer filled; the content figure is what
		// texels a bake would actually write. The gap between them is the price
		// of the gutter, and it is large at small chart sizes.
		double paddedArea = 0.0, contentArea = 0.0;
		for (const LightmapChart& chart : m_Result.charts) {
			paddedArea  += double(chart.width) * double(chart.height);
			contentArea += double(chart.innerWidth(m_Result.gutterTexels))
			             * double(chart.innerHeight(m_Result.gutterTexels));
		}
		const double atlasArea = double(m_Result.atlasResolution) * double(m_Result.atlasResolution);

		if (ImGui::BeginTable("##LightmapStats", 2, ImGuiTableFlags_SizingStretchProp)) {
			auto Row = [&theme](const char* label, const std::string& value, const char* tooltip = nullptr) {
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::TextUnformatted(label);
				theme.PopColor();
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(value.c_str());
				if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
			};

			Row("Mesh",      std::format("{}  ({} triangles)", m_ResultMeshName, m_ResultTriCount));
			Row("Charts",    std::format("{}", m_Result.charts.size()),
				"One connected, near-planar group of triangles. Every chart boundary\n"
				"is a seam, so fewer charts is better -- but each chart is projected\n"
				"flat, so bigger charts distort more.");
			Row("Atlas",     std::format("{0} x {0} texels, gutter {1}",
				m_Result.atlasResolution, m_Result.gutterTexels));
			Row("Density",   std::format("{:.5f} world units / texel", m_Result.worldUnitsPerTexel),
				"The value the auto-fit settled on, or the one you typed. This is what\n"
				"a texel is worth in world space, and therefore the bake's resolution.");
			Row("Chart angle used", std::format("{:.1f} deg", m_Result.maxChartAngleDegrees));
			Row("Packed area",  std::format("{:.1f}% of atlas (charts including gutters)",
				100.0 * paddedArea / atlasArea));
			Row("Usable area",  std::format("{:.1f}% of atlas (texels a bake would write)",
				100.0 * contentArea / atlasArea),
				"The difference from the packed figure is what the gutters cost.\n"
				"Bounding boxes are packed, not chart outlines, so a further\n"
				"input-dependent slice of this is inside a rectangle but outside\n"
				"any triangle.");
			ImGui::EndTable();
		}

		// LightmapUV clamps the requested angle; say so rather than showing a
		// number the user did not ask for and letting them wonder.
		if (std::abs(m_RequestedChartAngle - m_Result.maxChartAngleDegrees) > 0.01f) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
			ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Requested %.1f deg was clamped to %.1f.",
				m_RequestedChartAngle, m_Result.maxChartAngleDegrees);
			theme.PopColor();
		}
	}

	void LightmapBakePanel::DrawAtlasPreview() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;

		if (!m_HasResult || !m_Result.ok || m_Result.atlasResolution == 0) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextUnformatted("Nothing to preview.");
			theme.PopColor();
			return;
		}

		// --- Overlay toggles --------------------------------------------------
		const bool overlayAffordable = m_ResultTriCount <= kTriangleOverlayCap;
		ImGui::BeginDisabled(!overlayAffordable);
		bool overlay = m_DrawTriangleOverlay && overlayAffordable;
		if (ImGui::Checkbox("Triangles", &overlay)) m_DrawTriangleOverlay = overlay;
		ImGui::EndDisabled();
		if (!overlayAffordable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Disabled above %u triangles (this mesh has %u).\n"
			                  "The overlay is re-submitted to the draw list every frame, so it\n"
			                  "costs its full vertex count per frame, not once. Chart rectangles\n"
			                  "below still show the packing.",
			                  kTriangleOverlayCap, m_ResultTriCount);

		ImGui::SameLine();
		ImGui::Checkbox("Gutters", &m_DrawGutters);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Draws each chart's PADDED rectangle around its content rectangle.\n"
			                  "The ring between them is the reserved gutter.");

		// --- Canvas -----------------------------------------------------------
		// Square, because the atlas is. Capped so the preview does not eat a
		// maximised window, and floored so a narrow dock still gets something.
		const float side = std::clamp(ImGui::GetContentRegionAvail().x, 96.0f, 480.0f);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		// InvisibleButton rather than Dummy: it makes the canvas a real item, so
		// IsItemHovered works and the chart tooltip below can exist.
		ImGui::InvisibleButton("##AtlasCanvas", ImVec2(side, side));
		const bool canvasHovered = ImGui::IsItemHovered();

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 canvasMax = { origin.x + side, origin.y + side };
		drawList->AddRectFilled(origin, canvasMax, IM_COL32(24, 24, 28, 255));

		const float texelToPx = side / static_cast<float>(m_Result.atlasResolution);
		auto TexelToScreen = [&](float tx, float ty) {
			return ImVec2(origin.x + tx * texelToPx, origin.y + ty * texelToPx);
		};

		drawList->PushClipRect(origin, canvasMax, true);

		// Charts: content rectangle filled, padded rectangle outlined.
		int hoveredChart = -1;
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		for (size_t i = 0; i < m_Result.charts.size(); ++i) {
			const LightmapChart& chart = m_Result.charts[i];

			const ImVec2 paddedMin = TexelToScreen(float(chart.x), float(chart.y));
			const ImVec2 paddedMax = TexelToScreen(float(chart.x + chart.width),
			                                       float(chart.y + chart.height));
			const ImVec2 innerMin = TexelToScreen(float(chart.innerX(m_Result.gutterTexels)),
			                                      float(chart.innerY(m_Result.gutterTexels)));
			const ImVec2 innerMax = TexelToScreen(
				float(chart.innerX(m_Result.gutterTexels) + chart.innerWidth(m_Result.gutterTexels)),
				float(chart.innerY(m_Result.gutterTexels) + chart.innerHeight(m_Result.gutterTexels)));

			if (m_DrawGutters)
				drawList->AddRect(paddedMin, paddedMax, IM_COL32(90, 90, 100, 180));

			drawList->AddRectFilled(innerMin, innerMax, ChartColor(static_cast<uint32_t>(i), 0.45f));
			drawList->AddRect(innerMin, innerMax, ChartColor(static_cast<uint32_t>(i), 0.95f));

			if (canvasHovered &&
			    mouse.x >= paddedMin.x && mouse.x <= paddedMax.x &&
			    mouse.y >= paddedMin.y && mouse.y <= paddedMax.y) {
				hoveredChart = static_cast<int>(i);
			}
		}

		// Per-triangle UVs. THIS is what shows packing quality that rectangles
		// cannot: how much of each rectangle the actual geometry fills, and
		// whether any chart folded over itself.
		if (m_DrawTriangleOverlay && overlayAffordable) {
			const uint32_t triangles = static_cast<uint32_t>(m_Result.cornerUV.size() / 3u);
			for (uint32_t t = 0; t < triangles; ++t) {
				if (t < m_Result.triangleChart.size() &&
				    m_Result.triangleChart[t] == LIGHTMAP_NO_CHART) continue;

				const glm::vec2& a = m_Result.cornerUV[size_t(t) * 3u + 0];
				const glm::vec2& b = m_Result.cornerUV[size_t(t) * 3u + 1];
				const glm::vec2& c = m_Result.cornerUV[size_t(t) * 3u + 2];
				// cornerUV is normalised atlas UV in [0,1], so the canvas mapping
				// is a straight scale by the canvas side.
				drawList->AddTriangle(
					{ origin.x + a.x * side, origin.y + a.y * side },
					{ origin.x + b.x * side, origin.y + b.y * side },
					{ origin.x + c.x * side, origin.y + c.y * side },
					IM_COL32(20, 20, 20, 200), 1.0f);
			}
		}

		drawList->PopClipRect();
		drawList->AddRect(origin, canvasMax, ImGui::GetColorU32(theme[EditorCol_Background4]));

		if (hoveredChart >= 0) {
			const LightmapChart& chart = m_Result.charts[hoveredChart];
			ImGui::BeginTooltip();
			ImGui::Text("Chart %d", hoveredChart);
			ImGui::Text("%u triangles", chart.triangleCount);
			ImGui::Text("padded %u x %u at (%u, %u) texels", chart.width, chart.height, chart.x, chart.y);
			ImGui::Text("content %u x %u texels",
				chart.innerWidth(m_Result.gutterTexels), chart.innerHeight(m_Result.gutterTexels));
			ImGui::Text("projection axis (%.2f, %.2f, %.2f)", chart.normal.x, chart.normal.y, chart.normal.z);
			ImGui::EndTooltip();
		}

		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		ImGui::TextWrapped("Filled rectangles are chart content; the ring around them is the gutter. "
		                   "Empty space between charts is wasted atlas -- bounding boxes are packed, "
		                   "not chart outlines, so expect roughly half the usable density of xatlas.");
		theme.PopColor();
	}

	void LightmapBakePanel::OnImGuiRender() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;

		ImGui::SetNextWindowSizeConstraints({ 420, 260 }, { FLT_MAX, FLT_MAX });
		ImGui::Begin(ICON_FA_MAP " Lightmap Bake");

		const bool simulating = m_EditorState->temp.isInRuntimeSimulation;
		if (simulating) ImGui::BeginDisabled();

		if (!m_ProjectManager->ProjectIsOpen()) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextWrapped("No project open.");
			theme.PopColor();
			if (simulating) ImGui::EndDisabled();
			ImGui::End();
			return;
		}

		// --- The banner. This is the first thing in the panel on purpose -----
		// Phase 10a's bake does not exist. A user who reads nothing else must
		// still not walk away believing this panel lit their scene.
		theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
		ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " X3 has NO lightmap bake pass. This panel generates the "
		                   "second UV set a bake would write into -- charting, packing, and the atlas layout. "
		                   "Nothing computes irradiance, nothing writes texels, and no shader samples a lightmap "
		                   "yet. DilateLightmap, the other half of the seam fix, is implemented and has no texels "
		                   "to dilate.");
		theme.PopColor();
		ImGui::Separator();

		LR_GUID meshGuid = LR_GUID::INVALID;
		std::string meshName, blockedReason;
		std::shared_ptr<MeshMetadata> mesh = ResolveTargetMesh(meshGuid, meshName, blockedReason);

		ImGui::Dummy({ 0.0f, 4.0f });
		theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
		ImGui::TextUnformatted("Target");
		theme.PopColor();
		ImGui::Separator();

		if (mesh) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::Text(ICON_FA_CUBE " %s  --  %u triangles", meshName.c_str(), mesh->TriCount);
			theme.PopColor();

			if (mesh->TriCount > kSlowUnwrapTriangles) {
				theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
				ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Large mesh: the unwrap runs on the UI thread, so "
				                   "the editor will stop responding until it finishes.");
				theme.PopColor();
			}
		}
		else {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextWrapped("%s", blockedReason.c_str());
			theme.PopColor();
		}

		ImGui::Dummy({ 0.0f, 6.0f });
		theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
		ImGui::TextUnformatted("Unwrap Settings");
		theme.PopColor();
		ImGui::Separator();
		ImGui::Dummy({ 0.0f, 3.0f });

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
		DrawSettings();
		ImGui::PopStyleVar();

		ImGui::Dummy({ 0.0f, 8.0f });

		// --- Actions ---------------------------------------------------------
		ImGui::BeginDisabled(mesh == nullptr);
		if (ImGui::Button(ICON_FA_BORDER_ALL " Generate Lightmap UVs")) {
			auto assetPool = m_ProjectManager->GetAssetManager()->GetAssetPool();

			// The pool's TriRefBuffer indexes VertexBuffer GLOBALLY, which is the
			// exact (buffer, range) shape GenerateLightmapUVs documents, so the
			// mesh's slice is named by firstTriIdx/TriCount and nothing is copied
			// or rebased first.
			m_RequestedChartAngle = m_Settings.maxChartAngleDegrees;
			const auto start = std::chrono::steady_clock::now();
			m_Result = GenerateLightmapUVs(assetPool->VertexBuffer, assetPool->TriRefBuffer,
			                               mesh->firstTriIdx, mesh->TriCount, m_Settings);
			const auto end = std::chrono::steady_clock::now();

			m_LastRunMs = std::chrono::duration<double, std::milli>(end - start).count();
			m_HasResult = true;
			m_ResultMeshGuid = meshGuid;
			m_ResultMeshName = meshName;
			m_ResultTriCount = mesh->TriCount;
			// Turning the overlay on by default for a mesh small enough to draw
			// it: the triangles are the point of the preview, and a user who has
			// to find a checkbox to see them mostly will not.
			m_DrawTriangleOverlay = (m_ResultTriCount <= kTriangleOverlayCap);

			LOG_EDITOR_INFO("Lightmap UVs for '{}': {} charts, {:.1f} ms, ok={}",
				m_ResultMeshName, m_Result.charts.size(), m_LastRunMs, m_Result.ok);
		}
		ImGui::EndDisabled();
		if (!mesh && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", blockedReason.c_str());

		ImGui::SameLine();

		// THE BAKE BUTTON EXISTS ONLY TO SAY IT DOES NOT WORK. Leaving it out
		// entirely would leave a user hunting for it; drawing it live would be a
		// lie. Disabled, labelled, and explained is the established pattern in
		// this codebase (see the primitive-mesh delete button in AssetsPanel).
		ImGui::BeginDisabled();
		ImGui::Button(ICON_FA_SUN " Bake Lightmap (not implemented)");
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("There is no bake pass in the engine. Baking needs a texel-to-world\n"
			                  "mapping driving rays through the existing BVH, an accumulation target,\n"
			                  "DilateLightmap over the coverage mask, and somewhere to store the\n"
			                  "result. Only the UV generation and the dilation exist today.");

		ImGui::SameLine();

		ImGui::BeginDisabled();
		ImGui::Button(ICON_FA_FLOPPY_DISK " Apply to Mesh (no storage)");
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Nowhere to write it. MeshMetadata has no lightmap UV field, CookedMesh\n"
			                  "has no lightmap section, and .lrmeta stores only a GUID and a source\n"
			                  "path. The generated set lives in this panel and is lost when the editor\n"
			                  "closes. Storing it also needs the vertex-split decision the LightmapUV\n"
			                  "header defers to the Phase 9 cook step, because the UVs are per\n"
			                  "triangle corner and Gpu::Vertex has no room for a second UV set.");

		// The result belongs to whatever mesh was selected when Generate ran. If
		// the selection has moved on, the preview below is about a different
		// object than the Target block above, and silently showing it would be
		// misleading.
		if (m_HasResult && m_ResultMeshGuid != meshGuid) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
			ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " The result below is for '%s', not the currently "
			                   "selected mesh.", m_ResultMeshName.c_str());
			theme.PopColor();
		}

		ImGui::Dummy({ 0.0f, 8.0f });
		theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
		ImGui::TextUnformatted("Progress");
		theme.PopColor();
		ImGui::Separator();
		ImGui::Dummy({ 0.0f, 3.0f });
		DrawProgress();

		ImGui::Dummy({ 0.0f, 8.0f });
		theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
		ImGui::TextUnformatted("Result");
		theme.PopColor();
		ImGui::Separator();
		ImGui::Dummy({ 0.0f, 3.0f });
		DrawResultStats();

		ImGui::Dummy({ 0.0f, 8.0f });
		theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
		ImGui::TextUnformatted("Atlas Preview");
		theme.PopColor();
		ImGui::Separator();
		ImGui::Dummy({ 0.0f, 3.0f });
		DrawAtlasPreview();

		ImGui::Dummy({ 0.0f, 8.0f });

		if (simulating) ImGui::EndDisabled();
		ImGui::End();
	}
}
