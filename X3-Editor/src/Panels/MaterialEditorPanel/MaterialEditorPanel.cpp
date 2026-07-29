#include <IconsFontAwesome6.h>
#include "Panels/MaterialEditorPanel/MaterialEditorPanel.h"
#include "Panels/DNDPayloads.h"
#include "Project/Assets/AssetManager.h"
#include "Project/Scene/SceneManager.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

// =============================================================================
// THE "LIVE PREVIEW" QUESTION, ANSWERED HONESTLY UP FRONT
// -----------------------------------------------------------------------------
// Phase 13 asks for a material editor "with live preview". Two different things
// could be meant by that, and only one of them is reachable from where the
// engine is today:
//
//   1. LIVE IN THE VIEWPORT. This one is real and is already true, for free.
//      Renderer::Parse() rebuilds ParsedScene::MaterialDescs from
//      MeshMetadata::importedMaterials EVERY FRAME (Renderer.cpp, the
//      renderableView loop), and the material SSBO is re-resolved and
//      re-uploaded every frame too (TextureTable::resolve in the per-frame
//      buffer block) rather than being version-gated like the mesh buffers. So
//      a slider dragged here changes the next rendered frame with no
//      invalidation call, no MarkUpdated, and no event dispatch. That is the
//      "live" this panel delivers, and it is why nothing in here signals the
//      renderer.
//
//   2. AN INLINE MATERIAL BALL. NOT BUILT, and not fakeable. It would need,
//      concretely: an offscreen colour target sized to the preview widget plus
//      its own depth/sync, a preview "scene" (a sphere mesh, a key light, an
//      environment) that is NOT the user's scene and therefore cannot come from
//      SceneManager, and a SECOND Renderer instance driven with its own
//      RenderSettings so that changing preview quality does not change the
//      viewport's -- Renderer already assumes it owns the frame's descriptor
//      ring, which is why a second instance is a real piece of work rather than
//      a second call. The resulting VkImage would then be registered with
//      ImGui_ImplVulkan_AddTexture and cached exactly the way
//      ViewportPanel.cpp does for the compute output. NONE of that exists, and
//      this panel does not add it.
//
// So the preview here is a SWATCH AND PARAMETER READOUT: the raw authored
// factors drawn as colour, plus the values the shader will actually derive from
// them (F0, the equivalent IOR, which runtime tier the material lands in, which
// pass it draws in). Everything shown is computed with the same expressions the
// engine uses, and nothing shown pretends to be a render.
//
// -----------------------------------------------------------------------------
// PERSISTENCE, WHICH IS THE SHARP EDGE OF THIS PANEL
// -----------------------------------------------------------------------------
// AssetManager::SaveAssetPoolToFolder writes .lrmeta sidecars, and an
// AssetMetaFile holds a GUID and a source path -- nothing else. importedMaterials
// are rebuilt from the model file by DecodeMesh on every project load. Therefore
// EDITS MADE HERE ARE LIVE BUT NOT SAVED. MaterialComponent::slots, by contrast,
// IS serialized (Scene.cpp writes Albedo/Metallic/.../SpecularLevel per slot).
// The panel says this in the UI and offers "Copy to scene overrides" as the
// route that persists, rather than quietly losing the user's work on reload.
// =============================================================================

namespace {

	/// The derived dielectric F0 the shader ends up using.
	/// MaterialDesc::specularLevel is documented as an F0 SCALE where 0.5 gives
	/// the standard 0.04, i.e. F0 = 0.08 * level -- the same relation
	/// MaterialXImport.h inverts when it maps specular_ior onto it.
	inline float DielectricF0(float specularLevel) { return 0.08f * specularLevel; }

	/// Inverse of Schlick's F0 = ((n-1)/(n+1))^2 for the air-to-surface case.
	/// Shown next to the slider because "IOR 1.5" means something to an artist
	/// and "specular level 0.5" does not.
	inline float F0ToIOR(float f0) {
		const float s = std::sqrt(std::clamp(f0, 0.0f, 0.999f));
		return (1.0f + s) / (1.0f - s);
	}

	/// EXACTLY the rule TextureTable::resolve applies, not MaterialDesc's
	/// hasExtendedLobes(). The two disagree on purpose: hasExtendedLobes() tests
	/// the lobes only, while resolve() ALSO allocates an ext entry for a
	/// non-default specularLevel, because that value has nowhere else to ride.
	/// A readout that claimed "base (no extra cost)" for a material with
	/// specularLevel 0.7 would be wrong about the thing it exists to report.
	inline bool NeedsExtendedTier(const X3::MaterialDesc& d) {
		const bool features = d.clearcoat > 0.0f
			|| d.sheenColor.r > 0.0f || d.sheenColor.g > 0.0f || d.sheenColor.b > 0.0f
			|| d.anisotropy != 0.0f;
		return features || d.specularLevel != 0.5f;
	}

	/// Case-insensitive substring test for the list filter. std::search with a
	/// tolower comparator rather than transforming copies of every name every
	/// frame.
	bool ContainsCI(const std::string& haystack, const char* needle) {
		if (!needle || !*needle) return true;
		const std::string n(needle);
		auto it = std::search(haystack.begin(), haystack.end(), n.begin(), n.end(),
			[](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
		return it != haystack.end();
	}
}

namespace X3
{

	std::vector<MaterialEditorPanel::MaterialRef> MaterialEditorPanel::CollectMaterials() const {
		std::vector<MaterialRef> out;
		if (!m_ProjectManager || !m_ProjectManager->ProjectIsOpen()) return out;

		auto assetManager = m_ProjectManager->GetAssetManager();
		if (!assetManager) return out;
		auto assetPool = assetManager->GetAssetPool();
		if (!assetPool) return out;

		for (const auto& [guid, metadataPair] : assetPool->Metadata) {
			const auto& [metadata, metadataExtension] = metadataPair;
			const MeshMetadata* mesh = dynamic_cast<const MeshMetadata*>(metadata.get());
			if (!mesh) continue;   // textures have no materials

			// Primitives have no source file, so their sourcePath is empty and
			// the filename would render as a blank row.
			std::string meshName;
			if (AssetManager::IsPrimitiveMesh(guid)) {
				meshName = AssetManager::GetPrimitiveMeshName(guid);
			}
			else if (metadataExtension) {
				meshName = metadataExtension->sourcePath.filename().string();
			}
			if (meshName.empty()) meshName = guid.string();

			// A mesh can declare more slots than it shipped materials for (see
			// the `imported` null branch in Renderer::Parse), so the row count is
			// the max of the two and the excess rows are listed but not editable.
			const uint32_t slotCount = std::max<uint32_t>(
				mesh->materialSlotCount, static_cast<uint32_t>(mesh->importedMaterials.size()));

			for (uint32_t slot = 0; slot < slotCount; ++slot) {
				MaterialRef ref;
				ref.meshGuid = guid;
				ref.slot     = slot;
				ref.meshName = meshName;
				ref.editable = slot < mesh->importedMaterials.size();

				// Slot names come from SubmeshInfo where the model file named the
				// submesh; otherwise the index is all there is.
				ref.slotName = "Slot " + std::to_string(slot);
				for (const SubmeshInfo& sm : mesh->submeshes) {
					if (sm.materialSlot == slot && !sm.name.empty()) {
						ref.slotName += "  (" + sm.name + ")";
						break;
					}
				}
				out.push_back(std::move(ref));
			}
		}

		// AssetPool::Metadata is an unordered_map, so iteration order is not
		// stable between runs -- and a list whose rows move when nothing changed
		// is unusable. Sort by name, then slot.
		std::sort(out.begin(), out.end(), [](const MaterialRef& a, const MaterialRef& b) {
			if (a.meshName != b.meshName) return a.meshName < b.meshName;
			return a.slot < b.slot;
		});
		return out;
	}

	uint32_t MaterialEditorPanel::CountSceneOverrides(LR_GUID meshGuid, uint32_t slot) const {
		auto sceneManager = m_ProjectManager ? m_ProjectManager->GetSceneManager() : nullptr;
		if (!sceneManager) return 0;
		std::shared_ptr<Scene> scene = sceneManager->GetOpenScene();
		if (!scene) return 0;

		uint32_t count = 0;
		for (auto e : scene->GetRegistry()->view<MeshComponent, MaterialComponent>()) {
			EntityHandle entity(e, scene->GetRegistry());
			if (entity.GetComponent<MeshComponent>().guid != meshGuid) continue;
			// The override only WINS for slots the component actually holds --
			// Renderer::Parse falls through to the imported material for any slot
			// past the end of MaterialComponent::slots.
			if (slot < entity.GetComponent<MaterialComponent>().slots.size()) ++count;
		}
		return count;
	}

	uint32_t MaterialEditorPanel::PushToSceneOverrides(LR_GUID meshGuid, uint32_t slot, const MaterialDesc& desc) {
		auto sceneManager = m_ProjectManager ? m_ProjectManager->GetSceneManager() : nullptr;
		if (!sceneManager) return 0;
		std::shared_ptr<Scene> scene = sceneManager->GetOpenScene();
		if (!scene) return 0;

		uint32_t touched = 0;
		for (auto e : scene->GetRegistry()->view<MeshComponent>()) {
			EntityHandle entity(e, scene->GetRegistry());
			if (entity.GetComponent<MeshComponent>().guid != meshGuid) continue;

			auto& materialComponent = entity.GetOrAddComponent<MaterialComponent>();
			// GROWING THE VECTOR SEEDS FROM THE IMPORTED MATERIALS, not from
			// MaterialDesc{}, for the same reason InspectorPanel's slot-count
			// reconciliation does: an entity that suddenly gains six default-white
			// slots because one of them was edited has lost its model's materials.
			if (materialComponent.slots.size() <= slot) {
				auto assetPool = m_ProjectManager->GetAssetManager()->GetAssetPool();
				std::shared_ptr<MeshMetadata> mesh = assetPool ? assetPool->find<MeshMetadata>(meshGuid) : nullptr;
				for (size_t i = materialComponent.slots.size(); i <= slot; ++i) {
					materialComponent.slots.push_back(
						(mesh && i < mesh->importedMaterials.size()) ? mesh->importedMaterials[i] : MaterialDesc{});
				}
			}
			materialComponent.slots[slot] = desc;
			++touched;
		}
		return touched;
	}

	void MaterialEditorPanel::DrawPreview(const MaterialDesc& desc) {
		EditorTheme& theme = m_EditorState->temp.editorTheme;
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		const float width  = ImGui::GetContentRegionAvail().x;
		const float height = 56.0f;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		// Reserve the layout space FIRST so everything below flows correctly;
		// the draw-list calls that follow paint into the reserved rect.
		ImGui::Dummy({ width, height });

		const float half = std::floor(width * 0.5f) - 4.0f;

		// --- Base colour, over a checkerboard -------------------------------
		// The checkerboard is not decoration: color.w below 1 moves the material
		// into the sorted transparent pass, and a flat swatch cannot show the
		// difference between alpha 1.0 and alpha 0.98.
		{
			const ImVec2 a = origin;
			const ImVec2 b = { origin.x + half, origin.y + height };
			const float cell = 8.0f;
			drawList->PushClipRect(a, b, true);
			for (float y = a.y; y < b.y; y += cell) {
				for (float x = a.x; x < b.x; x += cell) {
					const bool dark = (static_cast<int>((x - a.x) / cell) + static_cast<int>((y - a.y) / cell)) & 1;
					drawList->AddRectFilled({ x, y }, { x + cell, y + cell },
						dark ? IM_COL32(70, 70, 70, 255) : IM_COL32(110, 110, 110, 255));
				}
			}
			drawList->PopClipRect();
			// RAW AUTHORED FACTORS, undisplayed through no tonemap and no
			// gamma conversion -- deliberately the same numbers ColorEdit3 shows,
			// so the swatch and the picker can never disagree.
			drawList->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(desc.color.r, desc.color.g, desc.color.b, desc.color.a)));
			drawList->AddRect(a, b, ImGui::GetColorU32(theme[EditorCol_Background4]));
		}

		// --- Emission -------------------------------------------------------
		// colour * strength, CLIPPED at 1 for display. Emission is an HDR
		// radiance and the swatch is 8-bit, so anything above 1 looks identical;
		// the numeric readout beside it is the honest part.
		{
			const ImVec2 a = { origin.x + half + 8.0f, origin.y };
			const ImVec2 b = { origin.x + width, origin.y + height };
			const glm::vec3 e = glm::vec3(desc.emission) * desc.emission.w;
			drawList->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(
				std::min(e.r, 1.0f), std::min(e.g, 1.0f), std::min(e.b, 1.0f), 1.0f)));
			drawList->AddRect(a, b, ImGui::GetColorU32(theme[EditorCol_Background4]));
		}

		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		ImGui::TextUnformatted("base colour (alpha over checker)          emission (clipped at 1)");
		theme.PopColor();

		ImGui::Dummy({ 0.0f, 4.0f });

		// --- Derived values the shader will actually use ---------------------
		if (ImGui::BeginTable("##MaterialDerived", 2, ImGuiTableFlags_SizingStretchProp)) {
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

			const float f0 = DielectricF0(desc.specularLevel);
			Row("Dielectric F0", std::format("{:.4f}  (IOR ~{:.2f})", f0, F0ToIOR(f0)),
				"Reflectance at normal incidence for the non-metal part of the surface.\n"
				"F0 = 0.08 * specular level, the convention MaterialDesc documents\n"
				"and MaterialXImport inverts when it maps specular_ior.");

			// At metallic 1 the base colour IS the reflectance; saying so is the
			// difference between "my metal looks black" being a mystery and being
			// obvious.
			Row("Metal reflectance", desc.metallic > 0.0f
					? std::format("{:.2f}, {:.2f}, {:.2f}  (base colour x {:.2f} metallic)",
						desc.color.r, desc.color.g, desc.color.b, desc.metallic)
					: std::string("n/a (fully dielectric)"),
				"A metal's specular colour is its base colour. Metallic blends\n"
				"between the dielectric F0 above and this.");

			Row("Render pass", desc.color.a < 1.0f
					? std::string("transparent (sorted back-to-front, per object)")
					: std::string("opaque"),
				"Alpha below 1 moves the material out of the opaque pass.");

			const bool ext = NeedsExtendedTier(desc);
			Row("Runtime tier", ext ? std::string("extended (allocates a Gpu::MaterialExt)")
			                        : std::string("base (no second-tier entry)"),
				"TextureTable::resolve allocates an ext entry when any lobe is\n"
				"non-zero OR specular level is not exactly 0.5. Note this is a\n"
				"stricter test than MaterialDesc::hasExtendedLobes(), which\n"
				"ignores specular level.");

			int boundTextures = 0;
			if (desc.baseColorTex  != LR_GUID::INVALID) ++boundTextures;
			if (desc.normalTex     != LR_GUID::INVALID) ++boundTextures;
			if (desc.metalRoughTex != LR_GUID::INVALID) ++boundTextures;
			if (desc.emissiveTex   != LR_GUID::INVALID) ++boundTextures;
			Row("Textures bound", std::format("{} of 4", boundTextures),
				"The swatch above shows FACTORS ONLY. A bound base-colour map\n"
				"multiplies into it per-texel and is not previewed here.");

			ImGui::EndTable();
		}
	}

	void MaterialEditorPanel::DrawFields(MaterialDesc& desc) {
		EditorTheme& theme = m_EditorState->temp.editorTheme;
		constexpr float kLabelWidth = 150.0f;

		auto Label = [&theme](const char* text) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(text);
			theme.PopColor();
			ImGui::SameLine(kLabelWidth);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		};
		auto Slider = [&theme, &Label](const char* text, const char* id, float* value,
		                               float lo, float hi, const char* fmt = "%.2f",
		                               ImGuiSliderFlags flags = 0) {
			Label(text);
			theme.PushColor(ImGuiCol_FrameBg, EditorCol_Primary1);
			ImGui::SliderFloat(id, value, lo, hi, fmt, flags);
			theme.PopColor();
		};
		auto Section = [&theme](const char* text) {
			ImGui::Dummy({ 0.0f, 6.0f });
			theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
			ImGui::TextUnformatted(text);
			theme.PopColor();
			ImGui::Separator();
			ImGui::Dummy({ 0.0f, 3.0f });
		};

		Section("Base");

		Label("Base Colour:");
		ImGui::ColorEdit3("##color", glm::value_ptr(desc.color),
			ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoInputs);

		// ALPHA IS ITS OWN CONTROL, not the fourth channel of the picker, for the
		// reason InspectorPanel states: it decides which pass the object draws
		// in, and a decision that large should not be two clicks deep in a popup.
		Slider("Alpha:", "##alpha", &desc.color.w, 0.0f, 1.0f, "%.3f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Below 1 moves this material into the transparent pass.");

		Slider("Metallic:",   "##metallic",  &desc.metallic,  0.0f, 1.0f);
		Slider("Roughness:",  "##roughness", &desc.roughness, 0.0f, 1.0f);
		Slider("Ambient Occlusion:", "##ao", &desc.ao,        0.0f, 1.0f);

		Slider("Normal Scale:", "##normalScale", &desc.normalScale, 0.0f, 4.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Scales the tangent-space normal perturbation.\n"
			                  "Does nothing without a normal map AND mesh tangents\n"
			                  "(Gpu::Vertex::tangent.w == 0 means the import produced none).");

		Section("Emission");

		Label("Emission Colour:");
		ImGui::ColorEdit3("##emissionColor", glm::value_ptr(desc.emission),
			ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoInputs);

		// Logarithmic because the useful range spans "faint glow" to "this is the
		// only light source in the room" and a linear slider spends 90% of its
		// travel above the point where the surface is already blown out.
		Slider("Emission Strength:", "##emissionStrength", &desc.emission.w,
			0.0f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);

		Section("Specular");

		Slider("Specular Level:", "##specularLevel", &desc.specularLevel, 0.0f, 1.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Dielectric F0 scale. 0.5 is the standard 0.04 (IOR 1.5).\n"
			                  "Anything other than exactly 0.5 costs a Gpu::MaterialExt entry.");

		Section("Extended Lobes");

		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		ImGui::TextWrapped("Non-default values put this material on the extended tier and compile in "
		                   "the coat / sheen / anisotropy paths. A lobe dragged back to zero stops "
		                   "costing anything.");
		theme.PopColor();
		ImGui::Dummy({ 0.0f, 3.0f });

		Slider("Clearcoat:",      "##clearcoat",      &desc.clearcoat,      0.0f, 1.0f);
		Slider("Coat Roughness:", "##clearcoatRough", &desc.clearcoatRough, 0.0f, 1.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Only reaches the shader while clearcoat weight is above zero.");

		Label("Sheen Colour:");
		ImGui::ColorEdit3("##sheenColor", glm::value_ptr(desc.sheenColor),
			ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoInputs);

		Slider("Sheen Roughness:", "##sheenRough", &desc.sheenRoughness, 0.0f, 1.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Only reaches the shader while sheen colour is not black.");

		Slider("Anisotropy:", "##anisotropy", &desc.anisotropy, -1.0f, 1.0f);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Stretches the specular lobe along the mesh tangent.\n"
			                  "A mesh imported without UVs has no tangents and shades isotropically.");

		Section("Textures");

		// DRAG A TEXTURE FROM THE ASSETS PANEL. This is a real assignment: the
		// GUID stored here is resolved to a bound table index by
		// TextureTable::getOrCreate on the next frame, uploading the texture if
		// this is its first use. Renderer::Parse's comment asked for exactly this
		// ("Revisit when Phase 13's material editor can actually assign and clear
		// a texture") -- note it was talking about MaterialComponent overrides,
		// where clearing a map is still ambiguous with "inherit". Here, on the
		// IMPORTED material, there is no inheritance rule above it, so clear
		// means clear and the trash button is unambiguous.
		auto TextureSlot = [&](const char* label, LR_GUID& guid) {
			ImGui::PushID(label);
			const bool bound = (guid != LR_GUID::INVALID);
			const float trashWidth = ImGui::CalcTextSize(ICON_FA_TRASH).x
				+ ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;

			std::string display = "none";
			if (bound) {
				display = guid.string();
				auto assetPool = m_ProjectManager->GetAssetManager()->GetAssetPool();
				if (assetPool) {
					auto it = assetPool->Metadata.find(guid);
					if (it != assetPool->Metadata.end() && it->second.second) {
						std::string name = it->second.second->sourcePath.filename().string();
						if (!name.empty()) display = name;
					}
					else {
						// A GUID with no pool entry is a dangling reference: the
						// texture was deleted, or the model shipped one the
						// importer could not read. TextureTable resolves it to
						// INVALID_TEXTURE and the factor is used alone.
						display = "MISSING (" + guid.string() + ")";
					}
				}
			}

			DragDropWidget(label, DNDPayloadTypes::TEXTURE, display,
				[&guid](const DNDPayload& payload) { guid = payload.guid; },
				theme,
				"Drag a texture asset here from the Assets panel.",
				{ ImGui::GetContentRegionAvail().x - trashWidth, 0 },
				bound);

			ImGui::SameLine();
			ImGui::BeginDisabled(!bound);
			if (ImGui::Button(ICON_FA_TRASH)) guid = LR_GUID::INVALID;
			ImGui::EndDisabled();
			if (!bound && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Nothing to clear -- this slot already uses the factor alone.");
			ImGui::PopID();
		};

		TextureSlot("Base Colour:", desc.baseColorTex);
		TextureSlot("Normal:",      desc.normalTex);
		TextureSlot("MetalRough:",  desc.metalRoughTex);
		TextureSlot("Emissive:",    desc.emissiveTex);
	}

	void MaterialEditorPanel::OnImGuiRender() {
		EditorTheme& theme = m_EditorState->temp.editorTheme;

		ImGui::SetNextWindowSizeConstraints({ 520, 260 }, { FLT_MAX, FLT_MAX });
		ImGui::Begin(ICON_FA_PALETTE " Material Editor");

		// Same guard every other panel uses: editing authoring data while the
		// runtime simulation owns the scene would be edited-out from under the
		// user on stop.
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

		auto assetPool = m_ProjectManager->GetAssetManager()->GetAssetPool();
		const std::vector<MaterialRef> materials = CollectMaterials();

		if (materials.empty()) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextWrapped("This project has no materials.\n\n"
			                   "X3 has no standalone material asset: a material is one slot of an "
			                   "imported mesh (MeshMetadata::importedMaterials). Import a model in "
			                   "the Assets panel and its materials appear here.");
			theme.PopColor();
			if (simulating) ImGui::EndDisabled();
			ImGui::End();
			return;
		}

		if (ImGui::BeginTable("##MaterialEditorSplit", 2,
				ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
			ImGui::TableSetupColumn("##List",   ImGuiTableColumnFlags_WidthFixed, 220.0f);
			ImGui::TableSetupColumn("##Editor", ImGuiTableColumnFlags_WidthStretch);

			// ---------------------------------------------------------------
			// LEFT: the mesh x slot table, flattened and filtered.
			// ---------------------------------------------------------------
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
			ImGui::InputTextWithHint("##MaterialFilter", "Filter...", m_Filter, sizeof(m_Filter));

			ImGui::BeginChild("##MaterialList", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
			{
				std::string lastMesh;
				for (size_t i = 0; i < materials.size(); ++i) {
					const MaterialRef& ref = materials[i];
					if (!ContainsCI(ref.meshName, m_Filter) && !ContainsCI(ref.slotName, m_Filter))
						continue;

					// One header per mesh, since a mesh's slots are always
					// adjacent after the sort.
					if (ref.meshName != lastMesh) {
						lastMesh = ref.meshName;
						ImGui::Dummy({ 0.0f, 3.0f });
						theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
						ImGui::Text(ICON_FA_CUBE " %s", ref.meshName.c_str());
						theme.PopColor();
					}

					ImGui::PushID(static_cast<int>(i));
					const bool selected = (ref.meshGuid == m_SelectedMeshGuid && ref.slot == m_SelectedSlot);

					// A slot the mesh declares but shipped no material for has
					// nothing behind it to edit -- the renderer default-constructs
					// one per frame. Disabled with the reason, rather than
					// selectable and then mysteriously empty.
					ImGui::BeginDisabled(!ref.editable);
					if (selected) theme.PushColor(ImGuiCol_Header, EditorCol_Secondary1);
					if (ImGui::Selectable(("    " + ref.slotName).c_str(), selected)) {
						m_SelectedMeshGuid = ref.meshGuid;
						m_SelectedSlot     = ref.slot;
						m_LastPushCount    = -1;
					}
					if (selected) theme.PopColor();
					ImGui::EndDisabled();
					if (!ref.editable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("This mesh declares the slot but shipped no material for it.\n"
						                  "The renderer uses a default MaterialDesc; there is no stored\n"
						                  "value to edit. Assign a MaterialComponent override in the\n"
						                  "Inspector instead.");
					ImGui::PopID();
				}
			}
			ImGui::EndChild();

			// ---------------------------------------------------------------
			// RIGHT: preview + fields for the selection.
			// ---------------------------------------------------------------
			ImGui::TableNextColumn();

			// THE WRITABLE PATH. GetAssetPool() hands back a
			// shared_ptr<const AssetPool>, but AssetPool::find<T>() is a const
			// member that dynamic_pointer_casts out of a
			// shared_ptr<Metadata> and therefore yields a NON-const
			// shared_ptr<MeshMetadata>. That is the only route by which anything
			// outside AssetManager can mutate pool contents, and InspectorPanel
			// already relies on it (it takes the same shared_ptr to read submesh
			// names). It is a const-correctness hole rather than a designed API;
			// it is used here because the alternative -- a mutable accessor on
			// AssetManager -- lives in X3/src, which this panel must not touch.
			std::shared_ptr<MeshMetadata> mesh =
				(m_SelectedMeshGuid != LR_GUID::INVALID && assetPool)
					? assetPool->find<MeshMetadata>(m_SelectedMeshGuid) : nullptr;

			const bool haveSelection = mesh && m_SelectedSlot < mesh->importedMaterials.size();
			if (!haveSelection) {
				// Covers first open, and the case where the selected mesh was
				// deleted from the Assets panel while this panel held it.
				m_SelectedMeshGuid = LR_GUID::INVALID;
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::TextWrapped("Select a material slot on the left.");
				theme.PopColor();
				ImGui::EndTable();
				if (simulating) ImGui::EndDisabled();
				ImGui::End();
				return;
			}

			MaterialDesc& desc = mesh->importedMaterials[m_SelectedSlot];

			ImGui::BeginChild("##MaterialEditorBody", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
			{
				// --- Where this edit lands, stated before any slider ---------
				// Two facts that a user will otherwise learn the hard way: the
				// edit is shared by every entity using the mesh, and it is not
				// written to disk.
				const uint32_t overrides = CountSceneOverrides(m_SelectedMeshGuid, m_SelectedSlot);

				theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
				ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " Edits here change the mesh's IMPORTED material, "
				                   "shared by every entity that uses it, and are NOT saved: .lrmeta stores only a "
				                   "GUID and a source path, so the model file's materials are re-read on the next "
				                   "project load.");
				theme.PopColor();

				if (overrides > 0) {
					theme.PushColor(ImGuiCol_Text, EditorCol_Error);
					ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %u entit%s in the open scene override this slot "
					                   "with a MaterialComponent and will NOT change. Edit those in the Inspector.",
					                   overrides, overrides == 1 ? "y" : "ies");
					theme.PopColor();
				}

				// The persisting route, and the only one. Disabled when there is
				// no open scene, because there is then nothing to write into.
				std::shared_ptr<Scene> openScene = m_ProjectManager->GetSceneManager()->GetOpenScene();
				ImGui::BeginDisabled(openScene == nullptr);
				if (ImGui::Button(ICON_FA_COPY " Copy to scene overrides")) {
					m_LastPushCount = static_cast<int>(
						PushToSceneOverrides(m_SelectedMeshGuid, m_SelectedSlot, desc));
					LOG_EDITOR_INFO("Material editor: copied slot {} of mesh {} onto {} scene entities",
						m_SelectedSlot, m_SelectedMeshGuid.string(), m_LastPushCount);
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
					if (!openScene)
						ImGui::SetTooltip("No scene is open, so there are no entities to write overrides onto.");
					else
						ImGui::SetTooltip("Writes this material into the MaterialComponent of every entity in the\n"
						                  "open scene using this mesh. MaterialComponent IS serialized with the\n"
						                  "scene, so this is what makes the edit survive a reload.");
				}
				if (m_LastPushCount >= 0) {
					ImGui::SameLine();
					theme.PushColor(ImGuiCol_Text, m_LastPushCount > 0 ? EditorCol_Success : EditorCol_Warning);
					ImGui::AlignTextToFramePadding();
					ImGui::Text("%d %s", m_LastPushCount, m_LastPushCount == 1 ? "entity" : "entities");
					theme.PopColor();
				}

				ImGui::Dummy({ 0.0f, 6.0f });

				// --- Preview ------------------------------------------------
				theme.PushColor(ImGuiCol_Text, EditorCol_Accent1);
				ImGui::TextUnformatted("Preview");
				theme.PopColor();
				ImGui::Separator();
				theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
				ImGui::TextWrapped("Swatch and derived values, not a render. Changes are live in the viewport: "
				                   "Renderer::Parse rebuilds the material buffer every frame.");
				ImGui::TextWrapped("With path-traced accumulation on, an edit blends in over many frames rather "
				                   "than appearing at once -- toggle Accumulate in Render Settings to restart it.");
				theme.PopColor();
				ImGui::Dummy({ 0.0f, 4.0f });

				DrawPreview(desc);

				// --- Fields --------------------------------------------------
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
				DrawFields(desc);
				ImGui::PopStyleVar();

				ImGui::Dummy({ 0.0f, 8.0f });
			}
			ImGui::EndChild();

			ImGui::EndTable();
		}

		if (simulating) ImGui::EndDisabled();
		ImGui::End();
	}
}
