#pragma once

// =============================================================================
// MaterialXImport.h -- Phase 12. MaterialX (.mtlx) documents carrying an
// OpenPBR Surface node -> MaterialDesc.
//
// THIS IS A BAKE-TIME TOOL, NOT A RUNTIME EVALUATOR. Decision 3 of ENGINE_PLAN
// splits OpenPBR into an authoring interchange format (here) and a reduced
// runtime model (Phase 6's MaterialDesc / Gpu::Material). UE5 does exactly this
// with Substrate: import, translate, discard the document. Nothing in this
// header is reachable from a frame.
//
// -----------------------------------------------------------------------------
// WHY THERE IS NO MaterialX DEPENDENCY
// -----------------------------------------------------------------------------
// The plan asks the question and this file answers it: we need DOCUMENT PARSING
// ONLY -- read the parameter values off one node -- and not ShaderGen, not node
// graph evaluation, not colour management. The MaterialX C++ core is lean, but
// it arrives as a submodule (X3/libs/MaterialX was a dangling gitlink with no
// .gitmodules entry, deleted in Phase 0) whose useful build options
// MATERIALX_BUILD_OIIO / MATERIALX_BUILD_OCIO pull trees far larger than this
// engine. A ~350-line XML reader in the .cpp reads the subset .mtlx actually
// uses. It is not a general XML processor and does not pretend to be; see the
// limitations list at the top of MaterialXImport.cpp.
//
// The repo vendors no XML library. It was checked: X3/libs holds assimp, glm,
// yaml-cpp, entt, spdlog, enkiTS, Jolt, slang, stb, VMA and vk-bootstrap.
// assimp bundles pugixml deep in its contrib tree, but that is a private
// implementation detail of a target we consume as a library -- reaching into it
// would break on any assimp bump, which is exactly the sort of breakage that
// surfaces months later in someone else's build.
//
// -----------------------------------------------------------------------------
// THE REDUCTION
// -----------------------------------------------------------------------------
// OpenPBR v1.1 carries ~50 channels. MaterialDesc carries 14 scalars and 4
// texture slots. There is no blessed real-time subset of OpenPBR and no
// published reduced profile, so the mapping below IS the profile -- it is the
// contract, and it is deliberately in the header where an author reading the
// struct will find it.
//
// Aliases: the importer accepts three spellings per channel -- OpenPBR v1.x
// canonical, the OpenPBR draft names (which is where `specular_ior_level` and
// `sheen_weight`/`sheen_color` come from; v1.x renamed those to `specular_ior`
// and the `fuzz_*` set), and Autodesk Standard Surface. Real .mtlx files in the
// wild are overwhelmingly standard_surface today because that is what Maya,
// Houdini, Arnold and Substance still write. Refusing them would make the
// importer useless for a year while authoring tools catch up.
//
// | MaterialX / OpenPBR input                                   | MaterialDesc         | Treatment |
// |-------------------------------------------------------------|----------------------|-----------|
// | `base_color` (`base_color`)                                  | `color.rgb`          | direct, multiplied by `base_weight` |
// | `base_weight` (`base`)                                       | `color.rgb`          | APPROXIMATED: folded into the colour. OpenPBR weights the diffuse lobe; we scale albedo. Identical for the diffuse term, wrong for the specular-over-diffuse energy split. |
// | `base_metalness` (`metalness`)                               | `metallic`           | direct |
// | `specular_roughness`                                         | `roughness`          | direct |
// | `specular_ior` (`specular_IOR`)                              | `specularLevel`      | APPROXIMATED: `level = ((ior-1)/(ior+1))^2 / 0.08`, clamped to [0,1]. 1.5 -> 0.5 -> F0 0.04, matching the struct's documented convention. CLAMPS above ior ~1.79 -- gemstones and dense glass lose their extra reflectance. |
// | `specular_ior_level`                                         | `specularLevel`      | direct (draft-name synonym; already 0..1) |
// | `specular_weight` (`specular`)                               | `specularLevel`      | APPROXIMATED: multiplied into the level. OpenPBR scales the whole specular lobe; we scale only F0, so grazing-angle Fresnel still goes to 1. |
// | `specular_roughness_anisotropy` (`specular_anisotropy`)      | `anisotropy`         | direct magnitude; see DROPPED for the rotation |
// | `coat_weight` (`coat`)                                       | `clearcoat`          | direct |
// | `coat_roughness`                                             | `clearcoatRough`     | direct |
// | `fuzz_weight` (`sheen_weight`, `sheen`)                      | `sheenColor`         | APPROXIMATED: multiplied into the colour. MaterialDesc has no sheen weight and `hasExtendedLobes()` tests the colour, so folding is exactly right -- weight 0 gives black gives no cost. |
// | `fuzz_color` (`sheen_color`)                                 | `sheenColor`         | direct, scaled by the weight above |
// | `fuzz_roughness` (`sheen_roughness`)                         | `sheenRoughness`     | direct |
// | `emission_color`                                             | `emission.rgb`       | direct |
// | `emission_luminance`                                         | `emission.w`         | APPROXIMATED: OpenPBR's unit is the nit; X3 has no photometric pipeline (LightComponent::intensity defaults to a unitless 1.0) and no exposure control until Phase 11. Passed through unchanged and scaled by `MtlxImportOptions::emissionLuminanceScale`, which defaults to identity. A document authored in real nits WILL blow out; that is visible and fixable, whereas a hidden constant in here is neither. |
// | `emission` (standard_surface weight)                         | `emission.w`         | direct -- standard_surface's `emission` is a unitless weight, not nits, so no scale is applied |
// | `geometry_opacity` (`opacity`)                               | `color.a`            | APPROXIMATED: averaged to one channel. Per-channel (coloured) cutout is dropped. |
// | `geometry_normal` (`normal`)                                 | `normalTex`          | texture reference only; see below |
// | `normalmap` node's `scale`, on the normal chain              | `normalScale`        | direct |
// | image bound to `base_color`                                  | `baseColorTex`       | sRGB unless the document says otherwise |
// | image bound to `emission_color`                              | `emissiveTex`        | sRGB unless the document says otherwise |
// | images bound to `specular_roughness` and/or `base_metalness` | `metalRoughTex`      | APPROXIMATED: MaterialDesc has ONE ORM slot (glTF packing, G=rough B=metal). Taken when both inputs name the same file, or when only one is bound. Two DIFFERENT files cannot be represented: roughness wins and a warning is recorded. |
//
// DROPPED, with the reason. Every one of these is reported per-material in
// `MtlxMaterial::reductions` when the document actually sets it, so a bake is
// never silently lossy:
//
// | Dropped                                                                   | Why |
// |---------------------------------------------------------------------------|-----|
// | `transmission_weight/color/depth/scatter/dispersion*` (`transmission`)      | NO MaterialDesc FIELD EXISTS. Transmission needs a refraction path in the tracer and an ordered transparent pass in Forward+ (Phase 7 pass 4); adding a field the shaders ignore would be a lie in the struct. |
// | `subsurface_*`                                                             | Same: no field, no runtime lobe. |
// | `thin_film_weight/thickness/ior`                                           | Gpu::MaterialExt has thin-film slots (aniso.z/.w) but MaterialDesc does not expose them, and MaterialDesc is the struct this phase must not change. |
// | `coat_color`, `coat_ior`, `coat_darkening`, `coat_roughness_anisotropy`    | Gpu::MaterialExt carries a coat IOR but fixed at 1.5; the rest have no representation. Coat tint would need a second colour in the ext tier. |
// | `geometry_coat_normal`, `geometry_tangent`, `geometry_coat_tangent`        | One normal slot, one mesh tangent. A second normal set means a second UV chain through the whole vertex format. |
// | anisotropy ROTATION (`specular_roughness_anisotropy` has a tangent input)   | MaterialDesc::anisotropy is signed magnitude relative to the mesh tangent; Gpu::MaterialExt::aniso.y (rotation) is not reachable from MaterialDesc. |
// | `base_diffuse_roughness`                                                    | The BSDF library is Lambert-diffuse; Oren-Nayar is not implemented. |
// | `geometry_thin_walled`                                                      | No thin-walled path in the BSDF. |
// | any input driven by a PROCEDURAL node graph (multiply, mix, noise, ...)      | We do not evaluate graphs -- see below. |
//
// NOT PRESENT IN OpenPBR AT ALL: `MaterialDesc::ao`. OpenPBR has no occlusion
// channel because occlusion is a rasteriser approximation, not a material
// property. It keeps its 1.0 default. If a metal/rough ORM image is bound its R
// channel supplies AO at runtime anyway.
//
// -----------------------------------------------------------------------------
// WHAT THIS IMPORTER DOES NOT DO
// -----------------------------------------------------------------------------
// NO NODE GRAPH EVALUATION. An input is read if it is a literal `value`, or if
// the chain behind it terminates in an `image`/`tiledimage` file. Anything else
// -- a `multiply` of two constants, a `mix`, a noise -- is recorded as an
// unevaluated graph in `reductions` and the field keeps its default. Baking a
// procedural chain by guessing at one of its constants would produce a material
// that is plausibly wrong, which is far worse than one that is obviously
// unhandled.
//
// SILENCE MEANS KEEP THE ENGINE DEFAULT. An input the document does not set is
// not written, rather than being written with OpenPBR's default. OpenPBR's
// default roughness is not X3's; a document that sets only `base_color` must
// not silently move every other channel.
//
// VALIDATION AGAINST THE ADOBE REFERENCE IS OUTSTANDING. The plan asks for the
// reduction to be measured against github.com/adobe/openpbr-bsdf so the size of
// the accepted error is known. That has NOT been done -- this machine is
// offline and the reference is not vendored. What it would consist of: build
// the reference in its C++ target, sweep (view, light, roughness, metalness,
// ior, coat, fuzz) on a hemisphere, evaluate both the reference full model and
// X3's Bsdf.slang on the reduced MaterialDesc produced from the same .mtlx,
// and report per-lobe RMS and worst-case radiance error. The expected large
// errors are the four APPROXIMATED rows above -- base_weight folding, the
// specular_weight/F0 conflation, the ior clamp, and emission units -- and the
// point of measuring is to learn which of them is worth a MaterialDesc field.
// =============================================================================

#include "lrpch.h"

#include "Core/GUID.h"
#include "Project/Assets/MaterialDesc.h"

namespace X3
{

	/// One image the document bound to a shader input, reported whether or not
	/// MaterialDesc had a slot for it -- an unbound coat-roughness map is
	/// something the person running the bake needs to be told about.
	struct MtlxTextureRef {
		std::string           input;      ///< the shader input it came from, e.g. "base_color"
		std::string           file;       ///< exactly as written in the document
		std::filesystem::path resolved;   ///< `file` made absolute against the document's directory
		std::string           colorSpace; ///< the declared MaterialX colorspace, empty if the document was silent
		bool                  isSRGB = false; ///< what we concluded, after the per-slot default
		bool                  used   = false; ///< false when MaterialDesc has no slot for it
	};

	struct MtlxMaterial {
		std::string  name;           ///< the <surfacematerial> name if one references the shader, else the shader node's
		std::string  shaderNode;     ///< the node name
		std::string  shaderCategory; ///< "open_pbr_surface", "standard_surface", ...
		MaterialDesc desc;

		std::vector<MtlxTextureRef> textures;

		/// Every approximation and every drop this material actually incurred,
		/// as human-readable lines. Populated from what the document SETS, not
		/// from the static table above: a document that never mentions
		/// transmission does not get told its transmission was dropped.
		///
		/// A bake tool prints these. They are the difference between a lossy
		/// conversion and a silently lossy one.
		std::vector<std::string> reductions;
	};

	struct MtlxImportOptions {
		/// Base directory for relative `file` attributes. Set automatically by
		/// ImportMaterialXFile; must be set by hand for the string overload if
		/// the document has relative texture paths.
		std::filesystem::path documentDir;

		/// Multiplier applied to `emission_luminance` (nits) on its way into
		/// MaterialDesc::emission.w. Identity by default -- see the emission row
		/// of the table. This exists so a project can pick its own nit-to-unit
		/// convention without a constant buried in a .cpp.
		float emissionLuminanceScale = 1.0f;

		/// Optional. Given a resolved texture path and whether it carries colour
		/// (and therefore belongs in an sRGB view), return the GUID the engine
		/// will key it under. Left empty, the texture GUID fields stay INVALID
		/// and only MtlxMaterial::textures is populated.
		///
		/// A CALLBACK RATHER THAN AN AssetManager DEPENDENCY on purpose: this
		/// importer must be usable from a console tool that has no AssetPool, no
		/// Vulkan device and no initialised logger, and AssetManager's texture
		/// loader wants all three.
		std::function<LR_GUID(const std::filesystem::path& resolved, bool isSRGB)> resolveTexture;
	};

	struct MtlxImportResult {
		bool        ok = false;
		std::string error;   ///< set only on a parse or structural failure; ok materials are still returned when possible

		std::vector<MtlxMaterial> materials;

		/// Document-level problems: an unknown version, a dangling nodename, a
		/// second surface shader we could not name. Per-material losses go in
		/// MtlxMaterial::reductions instead.
		std::vector<std::string> warnings;
	};

	/// Parse an in-memory MaterialX document. Never throws, never logs -- the
	/// engine logger is a null shared_ptr until Log::Init(), which a console
	/// tool has no reason to have called.
	MtlxImportResult ImportMaterialXFromString(std::string_view xml,
	                                           const MtlxImportOptions& options = {});

	/// Read and parse a .mtlx file. Fills options.documentDir from the file's
	/// own directory when the caller left it empty, because a texture path
	/// resolved against the process's cwd is the classic "works on my machine"
	/// asset bug.
	MtlxImportResult ImportMaterialXFile(const std::filesystem::path& path,
	                                     MtlxImportOptions options = {});

}
