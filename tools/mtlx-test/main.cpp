// =============================================================================
// X3MtlxTest -- assertions over the Phase 12 MaterialX/OpenPBR importer.
//
// NO GPU, NO DISPLAY, NO ASSET FILES. The documents are string literals in this
// file on purpose: a .mtlx fixture on disk is a second thing to keep in sync
// with the assertions, and the failure mode of a stale fixture is a test that
// passes while checking the wrong document. Everything a case needs is visible
// beside the case.
//
// Every check is a property of the REDUCTION -- "ior 1.5 becomes specularLevel
// 0.5", "a weight the struct cannot hold is folded and reported", "silence
// keeps the engine default". None is a value recorded from a run. That is the
// same distinction X3MathTest draws, and it is what makes this a gate rather
// than a change detector: the mapping table in MaterialXImport.h is the
// specification, and these are its executable form.
//
// Exit code 0 only if every check passed.
// =============================================================================

#include "Project/Assets/MaterialXImport.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

	int g_Failures = 0;
	int g_Checks   = 0;

	void check(bool ok, const std::string& what) {
		++g_Checks;
		if (!ok) {
			++g_Failures;
			std::printf("  \033[31mFAIL\033[0m  %s\n", what.c_str());
		}
	}

	void checkClose(float a, float b, float tol, const std::string& what) {
		++g_Checks;
		if (!(std::abs(a - b) <= tol)) {
			++g_Failures;
			std::printf("  \033[31mFAIL\033[0m  %s  (%.6f vs %.6f, tol %.6f)\n",
			            what.c_str(), a, b, tol);
		}
	}

	constexpr float kTol = 1e-5f;

	/// The reduction log is part of the contract, not debug output: a lossy
	/// conversion that does not say what it lost is the exact failure the plan's
	/// "document your reduction explicitly" is aimed at. So it gets asserted on.
	bool HasNote(const X3::MtlxMaterial& mat, std::string_view needle) {
		for (const std::string& n : mat.reductions)
			if (n.find(needle) != std::string::npos) return true;
		return false;
	}

	// -------------------------------------------------------------------------
	// A full OpenPBR v1.x document, every channel this importer claims, in the
	// canonical spellings. Also carries three channels that MUST be dropped.
	// -------------------------------------------------------------------------
	constexpr const char* kOpenPbrDoc = R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- Written by hand; the comment and the prolog are here because real
     exporter output has both and the reader must skip them. -->
<materialx version="1.39" colorspace="lin_rec709">
  <open_pbr_surface name="SR_test" type="surfaceshader">
    <input name="base_weight" type="float" value="0.5" />
    <input name="base_color" type="color3" value="0.8, 0.4, 0.2" />
    <input name="base_metalness" type="float" value="1.0" />
    <input name="specular_roughness" type="float" value="0.25" />
    <input name="specular_ior" type="float" value="1.5" />
    <input name="specular_roughness_anisotropy" type="float" value="0.6" />
    <input name="coat_weight" type="float" value="0.75" />
    <input name="coat_roughness" type="float" value="0.05" />
    <input name="coat_ior" type="float" value="1.6" />
    <input name="fuzz_weight" type="float" value="0.5" />
    <input name="fuzz_color" type="color3" value="1.0, 0.5, 0.25" />
    <input name="fuzz_roughness" type="float" value="0.4" />
    <input name="emission_color" type="color3" value="0.1, 0.2, 0.3" />
    <input name="emission_luminance" type="float" value="1000" />
    <input name="geometry_opacity" type="color3" value="0.5, 0.5, 0.5" />
    <input name="transmission_weight" type="float" value="0.9" />
    <input name="subsurface_weight" type="float" value="0.3" />
    <input name="thin_film_thickness" type="float" value="300" />
  </open_pbr_surface>
  <surfacematerial name="TestMaterial" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_test" />
  </surfacematerial>
</materialx>
)";

	void TestOpenPbrScalars() {
		X3::MtlxImportOptions opt;
		// 1000 nits -> 1.0. Exercises the knob that exists so the nit-to-unit
		// convention is a caller's decision and not a constant in the .cpp.
		opt.emissionLuminanceScale = 0.001f;

		const auto r = X3::ImportMaterialXFromString(kOpenPbrDoc, opt);
		check(r.ok, "a well-formed OpenPBR document parses");
		check(r.error.empty(), "a well-formed document reports no error");
		check(r.materials.size() == 1, "one surface shader yields one material");
		if (r.materials.size() != 1) return;

		const X3::MtlxMaterial& m = r.materials[0];
		const X3::MaterialDesc& d = m.desc;

		// The name an artist sees is the <surfacematerial>'s, not the shader
		// node's -- those differ in every DCC export and only one of them is
		// what the artist typed.
		check(m.name == "TestMaterial", "the material takes its name from the surfacematerial that references it");
		check(m.shaderNode == "SR_test", "the shader node name is kept as well");
		check(m.shaderCategory == "open_pbr_surface", "the shader category is reported");

		// base_weight folds into albedo.
		checkClose(d.color.r, 0.4f, kTol, "base_color.r * base_weight lands in color.r");
		checkClose(d.color.g, 0.2f, kTol, "base_color.g * base_weight lands in color.g");
		checkClose(d.color.b, 0.1f, kTol, "base_color.b * base_weight lands in color.b");
		check(HasNote(m, "base_weight"), "the base_weight fold is reported as an approximation");

		checkClose(d.metallic,  1.0f,  kTol, "base_metalness -> metallic");
		checkClose(d.roughness, 0.25f, kTol, "specular_roughness -> roughness");

		// THE ONE MAPPING WITH REAL ARITHMETIC BEHIND IT. ior 1.5 is F0 0.04 is
		// specularLevel 0.5, which is the convention MaterialDesc.h documents.
		// If someone changes the F0 scale, this is what says so.
		checkClose(d.specularLevel, 0.5f, 1e-4f, "specular_ior 1.5 -> specularLevel 0.5 (F0 0.04)");

		checkClose(d.clearcoat,      0.75f, kTol, "coat_weight -> clearcoat");
		checkClose(d.clearcoatRough, 0.05f, kTol, "coat_roughness -> clearcoatRough");

		// fuzz_weight folds into the colour, which is what makes
		// hasExtendedLobes() correct without a separate weight field.
		checkClose(d.sheenColor.r, 0.5f,   kTol, "fuzz_color.r * fuzz_weight -> sheenColor.r");
		checkClose(d.sheenColor.g, 0.25f,  kTol, "fuzz_color.g * fuzz_weight -> sheenColor.g");
		checkClose(d.sheenColor.b, 0.125f, kTol, "fuzz_color.b * fuzz_weight -> sheenColor.b");
		checkClose(d.sheenRoughness, 0.4f, kTol, "fuzz_roughness -> sheenRoughness");

		checkClose(d.emission.r, 0.1f, kTol, "emission_color -> emission.rgb");
		checkClose(d.emission.g, 0.2f, kTol, "emission_color -> emission.rgb");
		checkClose(d.emission.b, 0.3f, kTol, "emission_color -> emission.rgb");
		checkClose(d.emission.a, 1.0f, kTol, "emission_luminance 1000 nits * scale 0.001 -> emission.w 1");

		checkClose(d.anisotropy, 0.6f, kTol, "specular_roughness_anisotropy -> anisotropy");
		checkClose(d.color.a,    0.5f, kTol, "geometry_opacity -> color.a");

		// The whole point of the extended tier: this material has a coat, sheen
		// and anisotropy, so it must be recognised as needing a MaterialExt.
		check(d.hasExtendedLobes(), "a coated, sheened, anisotropic material reports extended lobes");

		// Drops must be REPORTED, not silent. This is the plan's requirement in
		// executable form.
		check(HasNote(m, "transmission_weight"), "dropped transmission_weight is reported");
		check(HasNote(m, "subsurface_weight"),   "dropped subsurface_weight is reported");
		check(HasNote(m, "thin_film_thickness"), "dropped thin_film_thickness is reported");
		check(HasNote(m, "coat_ior"),            "dropped coat_ior is reported");

		// ...and a drop must never be reported for something we did consume,
		// or the log becomes noise nobody reads.
		check(!HasNote(m, "DROPPED base_color"),         "base_color is not reported as dropped");
		check(!HasNote(m, "DROPPED specular_roughness:"), "specular_roughness is not reported as dropped");
	}

	// -------------------------------------------------------------------------
	// Autodesk Standard Surface, which is what Maya, Houdini, Arnold and
	// Substance actually write today. Different spellings, and `emission` is a
	// unitless WEIGHT rather than nits -- conflating the two would make every
	// standard_surface emitter 1000x wrong in one direction or the other.
	// -------------------------------------------------------------------------
	constexpr const char* kStandardSurfaceDoc = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <standard_surface name="SS_alias" type="surfaceshader">
    <input name="base" type="float" value="1.0" />
    <input name="base_color" type="color3" value="0.5, 0.5, 0.5" colorspace="srgb_texture" />
    <input name="metalness" type="float" value="0.25" />
    <input name="specular_roughness" type="float" value="0.6" />
    <input name="specular_IOR" type="float" value="1.33" />
    <input name="sheen" type="float" value="0.4" />
    <input name="sheen_color" type="color3" value="1, 1, 1" />
    <input name="sheen_roughness" type="float" value="0.2" />
    <input name="emission" type="float" value="2.0" />
    <input name="emission_color" type="color3" value="1, 1, 1" />
    <input name="transmission" type="float" value="0.5" />
    <input name="opacity" type="float" value="0.25" />
  </standard_surface>
</materialx>
)";

	void TestStandardSurfaceAliases() {
		X3::MtlxImportOptions opt;
		opt.emissionLuminanceScale = 0.001f;   // must NOT apply: `emission` is not nits

		const auto r = X3::ImportMaterialXFromString(kStandardSurfaceDoc, opt);
		check(r.ok && r.materials.size() == 1, "a standard_surface document imports");
		if (r.materials.size() != 1) return;

		const X3::MaterialDesc& d = r.materials[0].desc;

		// A COLOUR LITERAL CARRYING ITS OWN ENCODING. Ignoring the colorspace
		// attribute leaves every Substance albedo visibly too bright, and it
		// reads as a lighting bug.
		const float expected = std::pow((0.5f + 0.055f) / 1.055f, 2.4f);   // sRGB EOTF at 0.5
		checkClose(d.color.r, expected, 1e-4f, "an srgb_texture colour literal is decoded to linear");

		checkClose(d.metallic,  0.25f, kTol, "metalness (standard_surface spelling) -> metallic");
		checkClose(d.roughness, 0.6f,  kTol, "specular_roughness -> roughness");

		// Water, ior 1.33: F0 = ((0.33)/(2.33))^2 = 0.02006, level = F0/0.08.
		checkClose(d.specularLevel, 0.250736f, 1e-4f, "specular_IOR 1.33 -> specularLevel ~0.2507");

		checkClose(d.sheenColor.r,   0.4f, kTol, "sheen * sheen_color -> sheenColor");
		checkClose(d.sheenRoughness, 0.2f, kTol, "sheen_roughness -> sheenRoughness");

		// The unit distinction. 2.0 stays 2.0 -- the nit scale is for
		// emission_luminance only.
		checkClose(d.emission.a, 2.0f, kTol,
		           "standard_surface's unitless `emission` weight is not scaled as if it were nits");

		checkClose(d.color.a, 0.25f, kTol, "scalar opacity -> color.a");
		check(HasNote(r.materials[0], "transmission"), "dropped transmission is reported");
	}

	// -------------------------------------------------------------------------
	// Textures through a nodegraph, which is how every real export binds maps:
	// image nodes inside a <nodegraph>, referenced by nodegraph=+output=. The
	// normal goes through a <normalmap> whose scale MaterialDesc has a field for.
	// -------------------------------------------------------------------------
	constexpr const char* kTexturedDoc = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG_tex">
    <image name="tex_base" type="color3">
      <input name="file" type="filename" value="textures/base_color.png" colorspace="srgb_texture" />
    </image>
    <image name="tex_orm" type="vector3">
      <input name="file" type="filename" value="textures/orm.png" />
    </image>
    <image name="tex_normal" type="vector3">
      <input name="file" type="filename" value="textures/normal&amp;bump.png" />
    </image>
    <normalmap name="nmap" type="vector3">
      <input name="in" type="vector3" nodename="tex_normal" />
      <input name="scale" type="float" value="0.75" />
    </normalmap>
    <output name="out_base"   type="color3"  nodename="tex_base" />
    <output name="out_orm"    type="vector3" nodename="tex_orm" />
    <output name="out_normal" type="vector3" nodename="nmap" />
  </nodegraph>
  <open_pbr_surface name="SR_tex" type="surfaceshader">
    <input name="base_color"         type="color3"  nodegraph="NG_tex" output="out_base" />
    <input name="base_metalness"     type="float"   nodegraph="NG_tex" output="out_orm" />
    <input name="specular_roughness" type="float"   nodegraph="NG_tex" output="out_orm" />
    <input name="emission_color"     type="color3"  nodegraph="NG_tex" output="out_base" />
    <input name="geometry_normal"    type="vector3" nodegraph="NG_tex" output="out_normal" />
  </open_pbr_surface>
</materialx>
)";

	void TestTexturesAndColourSpace() {
		std::vector<std::pair<std::string, bool>> resolved;   // (path, isSRGB) in call order

		X3::MtlxImportOptions opt;
		opt.documentDir = "/project/materials";
		opt.resolveTexture = [&resolved](const std::filesystem::path& p, bool isSRGB) {
			resolved.emplace_back(p.generic_string(), isSRGB);
			return X3::LR_GUID(1000 + resolved.size());
		};

		const auto r = X3::ImportMaterialXFromString(kTexturedDoc, opt);
		check(r.ok && r.materials.size() == 1, "a textured document imports");
		if (r.materials.size() != 1) return;

		const X3::MtlxMaterial& m = r.materials[0];
		const X3::MaterialDesc& d = m.desc;

		check(d.baseColorTex  != X3::LR_GUID::INVALID, "base_color's image reaches baseColorTex");
		check(d.metalRoughTex != X3::LR_GUID::INVALID, "the shared ORM image reaches metalRoughTex");
		check(d.normalTex     != X3::LR_GUID::INVALID, "geometry_normal's image reaches normalTex through the normalmap");
		check(d.emissiveTex   != X3::LR_GUID::INVALID, "emission_color's image reaches emissiveTex");

		// The normalmap's scale is the only thing on that chain MaterialDesc can
		// hold, and it is easy to walk straight past on the way to the image.
		checkClose(d.normalScale, 0.75f, kTol, "the normalmap node's scale -> normalScale");

		// RELATIVE PATHS RESOLVE AGAINST THE DOCUMENT. Against the process cwd
		// they work for whoever authored the file and nobody else.
		bool foundBase = false, foundNormal = false;
		for (const X3::MtlxTextureRef& t : m.textures) {
			if (t.input == "base_color") {
				foundBase = true;
				check(t.resolved.generic_string() == "/project/materials/textures/base_color.png",
				      "a relative file path resolves against the document directory");
				check(t.isSRGB, "an srgb_texture-tagged base colour map is marked sRGB");
			}
			if (t.input == "geometry_normal") {
				foundNormal = true;
				// COLOUR SPACE IS NOT COSMETIC: a normal map read through an
				// sRGB view has an EOTF applied to numbers that are not light,
				// and it looks like a shading bug rather than a format bug.
				check(!t.isSRGB, "an untagged normal map defaults to linear, not sRGB");
				check(t.resolved.filename().generic_string() == "normal&bump.png",
				      "&amp; in a file path is decoded to '&'");
			}
		}
		check(foundBase,   "the base colour texture is reported in MtlxMaterial::textures");
		check(foundNormal, "the normal texture is reported in MtlxMaterial::textures");

		// The resolver is the only route to a GUID, so if it was not called the
		// texture never reached the asset system.
		check(resolved.size() >= 4, "the texture resolver was called for every bound slot");

		// One image, two inputs: the glTF ORM packing MaterialDesc assumes.
		check(HasNote(m, "ORM"), "sharing one image between metalness and roughness is reported as an ORM read");
	}

	// -------------------------------------------------------------------------
	// Two DIFFERENT images on metalness and roughness. MaterialDesc has one ORM
	// slot, so one of them cannot survive -- and the whole value of the
	// reduction log is that this says so instead of quietly using half the
	// material's maps.
	// -------------------------------------------------------------------------
	constexpr const char* kSplitOrmDoc = R"(<materialx version="1.39">
  <image name="tex_m" type="float"><input name="file" type="filename" value="m.png" /></image>
  <image name="tex_r" type="float"><input name="file" type="filename" value="r.png" /></image>
  <open_pbr_surface name="SR_split" type="surfaceshader">
    <input name="base_metalness" type="float" nodename="tex_m" />
    <input name="specular_roughness" type="float" nodename="tex_r" />
  </open_pbr_surface>
</materialx>
)";

	void TestSplitMetalRoughIsReported() {
		X3::MtlxImportOptions opt;
		opt.resolveTexture = [](const std::filesystem::path&, bool) { return X3::LR_GUID(7); };

		const auto r = X3::ImportMaterialXFromString(kSplitOrmDoc, opt);
		check(r.ok && r.materials.size() == 1, "a split metal/rough document imports");
		if (r.materials.size() != 1) return;

		const X3::MtlxMaterial& m = r.materials[0];
		check(m.desc.metalRoughTex != X3::LR_GUID::INVALID, "the roughness image takes the single ORM slot");
		check(HasNote(m, "one ORM slot"), "losing the metalness image is reported, not silent");

		bool sawUnused = false;
		for (const X3::MtlxTextureRef& t : m.textures)
			if (t.file == "m.png") sawUnused = !t.used;
		check(sawUnused, "the dropped image is still listed, marked unused");
	}

	// -------------------------------------------------------------------------
	// SILENCE MEANS KEEP THE ENGINE DEFAULT. Writing OpenPBR's defaults over
	// channels the document never mentions would move roughness from X3's 0.5 to
	// OpenPBR's 0.3 on every material anyone ever imports.
	// -------------------------------------------------------------------------
	void TestSilenceKeepsEngineDefaults() {
		constexpr const char* doc = R"(<materialx version="1.39">
  <open_pbr_surface name="SR_min" type="surfaceshader">
    <input name="base_color" type="color3" value="0.2, 0.3, 0.4" />
  </open_pbr_surface>
</materialx>
)";
		const auto r = X3::ImportMaterialXFromString(doc);
		check(r.ok && r.materials.size() == 1, "a minimal document imports");
		if (r.materials.size() != 1) return;

		const X3::MaterialDesc& d = r.materials[0].desc;
		const X3::MaterialDesc  fresh{};

		checkClose(d.color.r, 0.2f, kTol, "the one input the document sets is applied");
		checkClose(d.roughness,     fresh.roughness,     kTol, "an unset roughness keeps the engine default");
		checkClose(d.metallic,      fresh.metallic,      kTol, "an unset metalness keeps the engine default");
		checkClose(d.specularLevel, fresh.specularLevel, kTol, "an unset specular keeps the engine default");
		checkClose(d.color.a,       fresh.color.a,       kTol, "an unset opacity keeps the engine default");
		checkClose(d.emission.a,    fresh.emission.a,    kTol, "an unset emission keeps the engine default");
		check(!d.hasExtendedLobes(), "a plain material needs no extended tier");
	}

	// -------------------------------------------------------------------------
	// A procedural chain is REFUSED, not guessed at. Baking one constant out of
	// a multiply produces a material that is plausibly wrong, which costs far
	// more to find than one that is obviously unhandled.
	//
	// The same document is a CYCLE (a -> b -> a). Without a depth bound the
	// walker recurses until the stack dies, and a bake tool that segfaults is
	// indistinguishable from a broken build.
	// -------------------------------------------------------------------------
	void TestProceduralGraphIsRefusedAndCyclesTerminate() {
		constexpr const char* doc = R"(<materialx version="1.39">
  <multiply name="a" type="color3"><input name="in1" type="color3" nodename="b" /></multiply>
  <multiply name="b" type="color3"><input name="in1" type="color3" nodename="a" /></multiply>
  <open_pbr_surface name="SR_cycle" type="surfaceshader">
    <input name="base_color" type="color3" nodename="a" />
  </open_pbr_surface>
</materialx>
)";
		const auto r = X3::ImportMaterialXFromString(doc);
		check(r.ok, "a document with a reference cycle still returns (no hang, no stack overflow)");
		if (r.materials.size() != 1) { check(false, "the cycling document yields one material"); return; }

		const X3::MtlxMaterial& m = r.materials[0];
		const X3::MaterialDesc  fresh{};
		checkClose(m.desc.color.r, fresh.color.r, kTol, "an unevaluated graph leaves the field at its default");
		check(HasNote(m, "unevaluated"), "an unevaluated graph is reported rather than guessed at");
	}

	// -------------------------------------------------------------------------
	// Malformed input must fail with a locatable message. A bake over a hundred
	// materials that says only "parse error" is not actionable.
	// -------------------------------------------------------------------------
	void TestMalformedDocumentsFailCleanly() {
		const char* cases[] = {
			"<materialx version=\"1.39\"><open_pbr_surface name=\"x\"></materialx>",  // mismatched close
			"<materialx version=1.39></materialx>",                                    // unquoted attribute
			"<materialx",                                                              // truncated
			"",                                                                        // empty
		};
		for (const char* c : cases) {
			const auto r = X3::ImportMaterialXFromString(c);
			check(!r.ok, "a malformed document is rejected");
			check(!r.error.empty(), "a rejected document explains why");
		}

		// A well-formed document that is not MaterialX at all.
		const auto notMtlx = X3::ImportMaterialXFromString("<scene><entity name=\"a\" /></scene>");
		check(!notMtlx.ok, "a non-MaterialX root element is rejected");

		// A .mtlx with no surface shader is NOT an error -- a library of
		// nodegraphs is a legitimate document -- but it must warn, or a bake
		// that silently produced nothing looks like it worked.
		const auto empty = X3::ImportMaterialXFromString(
			"<materialx version=\"1.39\"><nodegraph name=\"NG\" /></materialx>");
		check(empty.ok, "a document with no surface shader is not an error");
		check(empty.materials.empty(), "...and produces no materials");
		check(!empty.warnings.empty(), "...but does warn");
	}

	// -------------------------------------------------------------------------
	// Published graph parameters (interfacename) and 1.37's <parameter> element.
	// Both are what "make this tweakable in the DCC" produces, and without them
	// a document reads as though every value were unset -- which imports as a
	// default grey material with no error anywhere.
	// -------------------------------------------------------------------------
	void TestInterfaceAndLegacyParameter() {
		constexpr const char* doc = R"(<materialx version="1.37">
  <nodegraph name="NG_i">
    <input name="rough" type="float" value="0.33" />
    <constant name="c" type="float">
      <input name="value" type="float" interfacename="rough" />
    </constant>
    <output name="out" type="float" nodename="c" />
  </nodegraph>
  <standard_surface name="SS_i" type="surfaceshader">
    <input name="specular_roughness" type="float" nodegraph="NG_i" output="out" />
    <parameter name="metalness" type="float" value="0.8" />
  </standard_surface>
</materialx>
)";
		const auto r = X3::ImportMaterialXFromString(doc);
		check(r.ok && r.materials.size() == 1, "a 1.37-style document imports");
		if (r.materials.size() != 1) return;

		checkClose(r.materials[0].desc.roughness, 0.33f, kTol,
		           "a value published as a nodegraph interface input is read through interfacename");
		checkClose(r.materials[0].desc.metallic, 0.8f, kTol,
		           "MaterialX 1.37's <parameter> element is read like an <input>");
	}

	// -------------------------------------------------------------------------
	// Node names are unique per SCOPE, not per document. Two nodegraphs each
	// containing "tex" is ordinary DCC output, and a flat name table resolves
	// half the references to the wrong image -- a material textured with another
	// material's maps, which looks like an art mistake.
	// -------------------------------------------------------------------------
	void TestNodeNamesAreScoped() {
		constexpr const char* doc = R"(<materialx version="1.39">
  <nodegraph name="NG_a">
    <image name="tex" type="color3"><input name="file" type="filename" value="a.png" /></image>
    <output name="out" type="color3" nodename="tex" />
  </nodegraph>
  <nodegraph name="NG_b">
    <image name="tex" type="color3"><input name="file" type="filename" value="b.png" /></image>
    <output name="out" type="color3" nodename="tex" />
  </nodegraph>
  <open_pbr_surface name="SR_a" type="surfaceshader">
    <input name="base_color" type="color3" nodegraph="NG_b" output="out" />
  </open_pbr_surface>
</materialx>
)";
		const auto r = X3::ImportMaterialXFromString(doc);
		check(r.ok && r.materials.size() == 1, "a two-nodegraph document imports");
		if (r.materials.size() != 1) return;

		bool correct = false;
		for (const X3::MtlxTextureRef& t : r.materials[0].textures)
			if (t.input == "base_color") correct = (t.file == "b.png");
		check(correct, "a name that exists in two nodegraphs resolves within the referenced one");
	}

	// -------------------------------------------------------------------------
	// Several materials in one file, which is what a material library is.
	// -------------------------------------------------------------------------
	void TestMultipleMaterials() {
		constexpr const char* doc = R"(<materialx version="1.39">
  <open_pbr_surface name="SR_gold" type="surfaceshader">
    <input name="base_color" type="color3" value="0.944, 0.776, 0.373" />
    <input name="base_metalness" type="float" value="1" />
    <input name="specular_roughness" type="float" value="0.02" />
  </open_pbr_surface>
  <surfacematerial name="Gold" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_gold" />
  </surfacematerial>
  <open_pbr_surface name="SR_plastic" type="surfaceshader">
    <input name="base_color" type="color3" value="0.1, 0.4, 0.8" />
    <input name="specular_roughness" type="float" value="0.35" />
  </open_pbr_surface>
  <surfacematerial name="Plastic" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_plastic" />
  </surfacematerial>
</materialx>
)";
		const auto r = X3::ImportMaterialXFromString(doc);
		check(r.ok && r.materials.size() == 2, "two shaders in one document yield two materials");
		if (r.materials.size() != 2) return;

		check(r.materials[0].name == "Gold",    "the first material is named from its own surfacematerial");
		check(r.materials[1].name == "Plastic", "the second material is named from its own surfacematerial");
		checkClose(r.materials[0].desc.metallic,  1.0f,  kTol, "the gold material is metallic");
		checkClose(r.materials[1].desc.metallic,  0.0f,  kTol, "the plastic material is not");
		checkClose(r.materials[1].desc.roughness, 0.35f, kTol, "each material reads its own roughness");
	}

	// -------------------------------------------------------------------------
	// Two authoring mistakes worth naming, because both look correct in the DCC
	// and render as nothing: a colour set without the weight that OpenPBR
	// defaults to zero.
	// -------------------------------------------------------------------------
	void TestZeroWeightAuthoringMistakesAreReported() {
		constexpr const char* doc = R"(<materialx version="1.39">
  <open_pbr_surface name="SR_oops" type="surfaceshader">
    <input name="emission_color" type="color3" value="1, 0.5, 0.2" />
    <input name="fuzz_color" type="color3" value="0.9, 0.9, 1" />
  </open_pbr_surface>
</materialx>
)";
		const auto r = X3::ImportMaterialXFromString(doc);
		check(r.ok && r.materials.size() == 1, "the document imports");
		if (r.materials.size() != 1) return;

		const X3::MtlxMaterial& m = r.materials[0];
		const X3::MaterialDesc  fresh{};
		checkClose(m.desc.emission.a, fresh.emission.a, kTol,
		           "emission_color without emission_luminance emits nothing (OpenPBR's luminance default is 0)");
		checkClose(m.desc.sheenColor.r, fresh.sheenColor.r, kTol,
		           "fuzz_color without fuzz_weight sheens nothing (OpenPBR's weight default is 0)");
		check(HasNote(m, "emission_color is set but"), "the emission authoring mistake is reported");
		check(HasNote(m, "fuzz_weight/sheen_weight is not"), "the sheen authoring mistake is reported");
	}

	// -------------------------------------------------------------------------
	// A high IOR cannot be represented: specularLevel 1 is F0 0.08, ior ~1.79.
	// Clamping is the honest failure; letting the level run past 1 would push F0
	// beyond what the shader's dielectric term expects and break grazing angles
	// as well as normal incidence.
	// -------------------------------------------------------------------------
	void TestHighIorClampsAndSaysSo() {
		constexpr const char* doc = R"(<materialx version="1.39">
  <open_pbr_surface name="SR_diamond" type="surfaceshader">
    <input name="specular_ior" type="float" value="2.42" />
  </open_pbr_surface>
</materialx>
)";
		const auto r = X3::ImportMaterialXFromString(doc);
		check(r.ok && r.materials.size() == 1, "a high-IOR document imports");
		if (r.materials.size() != 1) return;

		checkClose(r.materials[0].desc.specularLevel, 1.0f, kTol, "ior 2.42 clamps specularLevel to 1");
		check(HasNote(r.materials[0], "clamps"), "the clamp is reported as an accepted error");
	}

} // namespace

int main() {
	std::printf("X3MtlxTest\n");

	TestOpenPbrScalars();
	TestStandardSurfaceAliases();
	TestTexturesAndColourSpace();
	TestSplitMetalRoughIsReported();
	TestSilenceKeepsEngineDefaults();
	TestProceduralGraphIsRefusedAndCyclesTerminate();
	TestMalformedDocumentsFailCleanly();
	TestInterfaceAndLegacyParameter();
	TestNodeNamesAreScoped();
	TestMultipleMaterials();
	TestZeroWeightAuthoringMistakesAreReported();
	TestHighIorClampsAndSaysSo();

	if (g_Failures == 0) {
		std::printf("  \033[32m%d checks passed\033[0m\n", g_Checks);
		return 0;
	}
	std::printf("  \033[31m%d of %d checks failed\033[0m\n", g_Failures, g_Checks);
	return 1;
}
