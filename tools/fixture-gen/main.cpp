// ============================================================================
// X3 fixture generator
// ----------------------------------------------------------------------------
// Writes the committed test project (TestProject/) by DRIVING THE ENGINE:
// ProjectManager::NewProject, AssetManager::ImportAsset, SceneManager and the
// real Scene serializer produce every byte on disk. Nothing here hand-authors
// YAML, so the fixture cannot drift away from the serialization code.
//
// After writing it reopens the project through ProjectManager::OpenProject and
// asserts the round-trip: boot scene present, a main camera, a mesh entity whose
// GUID resolves to real triangles, a light, and a skybox texture. A generator
// that writes a file the engine cannot read back is a failed generator, so the
// exit code covers both halves.
//
//   X3FixtureGen [--out <folder>] [--model <file>] [--skybox <file>] [--force]
//   X3FixtureGen --verify <project.lrproj>     check an existing fixture only
//
// Defaults point at the in-tree sample assets. Exit 0 only when the project was
// written and read back successfully.
// ============================================================================

#include "Core/Log.h"
#include "Core/GUID.h"
#include "Project/ProjectManager.h"
#include "Project/Scene/SceneManager.h"
#include "Project/Scene/Scene.h"
#include "Project/Scene/Components.h"
#include "Project/Scene/Entity.h"
#include "Project/Assets/AssetManager.h"

#include <limits>

namespace fs = std::filesystem;
using namespace X3;

namespace {

	// Which fixture to build. `smoke` is the original: an imported model, used by
	// verify.sh's render-path smoke test. The others are PRIMITIVE-ONLY, so they
	// depend on nothing in SampleModels and work on any checkout -- which matters
	// because they are the per-pass gates Phase 7 will be validated against, and
	// a gate that needs an asset someone forgot to fetch is not a gate.
	enum class Fixture { Smoke, Materials, Lights,
		Transparency,
		SoftShadows,
		LeakTest,
		AmbientFalloff
	};

	struct Options {
		fs::path outFolder  = fs::path(X3_SOURCE_DIR) / "TestProject";
		fs::path modelPath  = fs::path(X3_SOURCE_DIR) / "SampleModels" / "stanford_bunny_pbr.glb";
		fs::path skyboxPath = fs::path(X3_SOURCE_DIR) / "SampleSkyboxes" / "kloofendal_48d_partly_cloudy_puresky_4k.hdr";
		bool force = false;
		fs::path verifyOnly; // non-empty: check this project and exit
		std::string fixture = "smoke";
	};

	struct Bounds {
		glm::vec3 min{ std::numeric_limits<float>::max() };
		glm::vec3 max{ std::numeric_limits<float>::lowest() };
		bool valid = false;

		glm::vec3 center() const { return (min + max) * 0.5f; }
		glm::vec3 size()   const { return max - min; }
		float radius()     const { return glm::length(size()) * 0.5f; }
	};

	/// Axis-aligned bounds of one mesh, read out of the asset pool the engine
	/// just filled. Used to frame the camera on whatever model is imported
	/// instead of hard-coding numbers that only suit one file.
	Bounds MeshBounds(const AssetPool& pool, LR_GUID guid) {
		Bounds b;
		auto metadata = pool.find<MeshMetadata>(guid);
		if (!metadata) { return b; }

		for (uint32_t i = 0; i < metadata->TriCount; ++i) {
			const size_t idx = metadata->firstTriIdx + i;
			if (idx >= pool.TriPositionBuffer.size()) { break; }
			const Gpu::TrianglePositions& tri = pool.TriPositionBuffer[idx];
			for (const glm::vec4& v : { tri.v0, tri.v1, tri.v2 }) {
				b.min = glm::min(b.min, glm::vec3(v));
				b.max = glm::max(b.max, glm::vec3(v));
			}
			b.valid = true;
		}
		return b;
	}

	Fixture ParseFixture(const std::string& name) {
		if (name == "materials") return Fixture::Materials;
		if (name == "lights")    return Fixture::Lights;
		if (name == "transparency") return Fixture::Transparency;
		if (name == "softshadows")  return Fixture::SoftShadows;
		if (name == "leaktest")     return Fixture::LeakTest;
		if (name == "ambientfall")  return Fixture::AmbientFalloff;
		return Fixture::Smoke;
	}

	bool ParseArgs(int argc, char** argv, Options& opts) {
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			auto next = [&](const char* what) -> const char* {
				if (i + 1 >= argc) {
					LOG_ENGINE_ERROR("missing value for {}", what);
					return nullptr;
				}
				return argv[++i];
			};

			if (arg == "--force") { opts.force = true; }
			else if (arg == "--fixture") { auto v = next("--fixture"); if (!v) return false; opts.fixture = v; }
			else if (arg == "--out")    { auto v = next("--out");    if (!v) return false; opts.outFolder  = v; }
			else if (arg == "--model")  { auto v = next("--model");  if (!v) return false; opts.modelPath  = v; }
			else if (arg == "--skybox") { auto v = next("--skybox"); if (!v) return false; opts.skyboxPath = v; }
			else if (arg == "--verify") { auto v = next("--verify"); if (!v) return false; opts.verifyOnly = v; }
			else {
				LOG_ENGINE_ERROR("unknown argument '{}'", arg);
				return false;
			}
		}
		return true;
	}

	/// Removes a previous fixture so the generator is re-runnable, but only when
	/// the folder really is a project folder. Never deletes an arbitrary path.
	bool ClearExistingProject(const fs::path& folder, bool force) {
		if (!fs::exists(folder)) { return true; }

		if (!force) {
			LOG_ENGINE_ERROR("output folder already exists: {} (pass --force to regenerate)", folder.string());
			return false;
		}

		bool looksLikeProject = fs::is_directory(folder) && fs::is_empty(folder);
		if (!looksLikeProject) {
			for (const auto& entry : fs::directory_iterator(folder)) {
				if (entry.path().extension() == PROJECT_FILE_EXTENSION) {
					looksLikeProject = true;
					break;
				}
			}
		}
		if (!looksLikeProject) {
			LOG_ENGINE_ERROR("refusing to --force over {}: no {} file found there",
				folder.string(), PROJECT_FILE_EXTENSION);
			return false;
		}

		std::error_code ec;
		fs::remove_all(folder, ec);
		if (ec) {
			LOG_ENGINE_ERROR("failed to remove {}: {}", folder.string(), ec.message());
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------------
	// Round-trip check: reopen what we just wrote and confirm the renderer would
	// find everything it needs. Mirrors Renderer::Parse's requirements.
	// ------------------------------------------------------------------------
	bool VerifyRoundTrip(const fs::path& projectFile, LR_GUID expectedMeshGuid, LR_GUID expectedSkyboxGuid) {
		LOG_ENGINE_INFO("--- verifying round-trip: reopening {} ---", projectFile.string());

		ProjectManager pm;
		if (!pm.OpenProject(projectFile)) {
			LOG_ENGINE_CRITICAL("round-trip: OpenProject failed");
			return false;
		}

		auto sceneManager = pm.GetSceneManager();
		auto assetManager = pm.GetAssetManager();
		auto scene = sceneManager->GetOpenScene();
		if (!scene) {
			LOG_ENGINE_CRITICAL("round-trip: boot scene did not open (bootSceneGuid {})",
				(uint64_t)pm.GetBootSceneGuid());
			return false;
		}
		auto pool = assetManager->GetAssetPool();

		int mainCameras = 0, lights = 0, renderableMeshes = 0;
		size_t totalTriangles = 0;

		for (auto [e, id] : scene->GetRegistry()->view<IDComponent>().each()) {
			EntityHandle entity(e, scene->GetRegistry());
			if (entity.HasComponent<CameraComponent>() && entity.GetComponent<CameraComponent>().isMain) {
				++mainCameras;
			}
			if (entity.HasComponent<LightComponent>()) {
				++lights;
			}
			if (entity.HasComponent<MeshComponent>() && entity.HasComponent<TransformComponent>()) {
				auto metadata = pool->find<MeshMetadata>(entity.GetComponent<MeshComponent>().guid);
				if (metadata && metadata->TriCount > 0) {
					++renderableMeshes;
					totalTriangles += metadata->TriCount;
				} else {
					LOG_ENGINE_ERROR("round-trip: mesh entity '{}' has unresolvable mesh GUID {}",
						entity.GetComponent<TagComponent>().Tag,
						(uint64_t)entity.GetComponent<MeshComponent>().guid);
				}
			}
		}

		bool ok = true;
		auto require = [&ok](bool condition, const std::string& what) {
			if (!condition) {
				LOG_ENGINE_CRITICAL("round-trip FAILED: {}", what);
				ok = false;
			} else {
				LOG_ENGINE_INFO("round-trip ok: {}", what);
			}
		};

		// Renderer::Parse returns nullptr without a camera, and RenderLayer then
		// produces no frame at all, so this one is load-bearing for the smoke test.
		require(mainCameras == 1, "exactly one main camera (found " + std::to_string(mainCameras) + ")");
		require(lights >= 1, "at least one light (found " + std::to_string(lights) + ")");
		require(renderableMeshes >= 2, "at least two renderable mesh entities (found " + std::to_string(renderableMeshes) + ")");
		require(totalTriangles > 0, "mesh entities reference " + std::to_string(totalTriangles) + " triangles");
		require(pool->find<TextureMetadata>(scene->skyboxGuid) != nullptr, "skybox GUID resolves to a texture");

		// Only meaningful right after generating: --verify does not know them.
		if (expectedMeshGuid != LR_GUID::INVALID) {
			require(pool->find<MeshMetadata>(expectedMeshGuid) != nullptr, "imported model GUID resolves after reload");
			require(scene->skyboxGuid == expectedSkyboxGuid, "skybox GUID survived the round-trip");
		}

		return ok;
	}

} // namespace

namespace {

	/// A grid of spheres sweeping the BSDF's parameter space.
	///
	/// EXISTS TO BE DIFFED, not admired. Phase 6 added clearcoat, sheen and
	/// anisotropy with no scene that exercises any of them, so the only coverage
	/// they had was the numeric furnace test -- which validates energy, not
	/// appearance. Rows are chosen so a regression in one lobe moves one row and
	/// leaves the others alone, which is what makes a golden diff diagnostic
	/// rather than merely red.
	void BuildMaterialsScene(std::shared_ptr<Scene> scene) {
		constexpr int kCols = 5;
		const float spacing = 1.4f;
		const float x0 = -0.5f * spacing * float(kCols - 1);

		// Camera looks down +Z at identity rotation, so it sits on -Z.
		{
			EntityHandle cam = scene->CreateEntity("Main Camera");
			cam.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 3.7f, -9.5f });
			auto& camera = cam.GetOrAddComponent<CameraComponent>();
			camera.isMain = true;
			camera.fov = 55.0f;
		}

		struct Row { const char* name; int index; };
		const Row rows[] = {
			{ "metal",     0 },   // metallic sweep of roughness
			{ "dielectric", 1 },  // dielectric sweep of roughness
			{ "clearcoat", 2 },   // coat over a rough dielectric
			{ "sheen",     3 },   // sheen colour over a rough base
			{ "aniso",     4 },   // anisotropy sweep on a smooth metal
		};

		for (const Row& row : rows) {
			for (int col = 0; col < kCols; ++col) {
				const float t = float(col) / float(kCols - 1);

				EntityHandle e = scene->CreateEntity(
					std::string(row.name) + "_" + std::to_string(col));
				auto& transform = e.GetComponent<TransformComponent>();
				// ROWS GO UP, NOT BACK. Laying them out along Z put every row
				// directly behind the one in front from the camera's position,
				// so the grid rendered as a single row with four hidden ones.
				transform.SetTranslation({ x0 + spacing * float(col),
				                           0.9f + spacing * float(row.index),
				                           0.0f });
				transform.SetScale(glm::vec3(1.1f));

				auto& mesh = e.GetOrAddComponent<MeshComponent>();
				mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::SPHERE);
				mesh.sourceName = "Sphere";

				auto& m = e.GetOrAddComponent<MaterialComponent>().slots[0];
				m.color = { 0.9f, 0.85f, 0.8f, 1.0f };
				m.roughness = glm::mix(0.05f, 1.0f, t);

				switch (row.index) {
					case 0: m.metallic = 1.0f; break;
					case 1: m.metallic = 0.0f; break;
					case 2: m.metallic = 0.0f; m.roughness = 0.6f;
					        m.clearcoat = t; m.clearcoatRough = 0.05f; break;
					case 3: m.metallic = 0.0f; m.roughness = 0.8f;
					        m.sheenColor = { t, t * 0.4f, 0.1f }; m.sheenRoughness = 0.3f; break;
					case 4: m.metallic = 1.0f; m.roughness = 0.25f;
					        m.anisotropy = glm::mix(-0.9f, 0.9f, t); break;
				}
			}
		}

		// Ground, so the spheres are grounded and cast something.
		{
			EntityHandle ground = scene->CreateEntity("Ground");
			auto& transform = ground.GetComponent<TransformComponent>();
			transform.SetTranslation({ 0.0f, 0.0f, 0.0f });
			transform.SetScale({ 30.0f, 1.0f, 30.0f });
			auto& mesh = ground.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE);
			mesh.sourceName = "Plane";
			auto& m = ground.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.35f, 0.35f, 0.38f, 1.0f };
			m.roughness = 0.9f;
		}

		// One directional light, pitched down. A single light keeps a material
		// regression from being masked by a lighting one.
		{
			EntityHandle sun = scene->CreateEntity("Sun");
			auto& transform = sun.GetComponent<TransformComponent>();
			transform.SetRotation({ 50.0f, -30.0f, 0.0f });
			auto& light = sun.GetOrAddComponent<LightComponent>();
			light.type = LightType::DIRECTIONAL;
			light.color = { 1.0f, 0.97f, 0.92f };
			light.intensity = 3.0f;
		}
	}

	/// One object per light type, spatially separated.
	///
	/// Separation is the point: directional, point and spot each get their own
	/// sphere and their own patch of floor, so a regression in the spot cone
	/// maths cannot hide behind correct directional shading. The shadow each
	/// casts is part of what is being tested -- Phase 8 replaces the shadow path
	/// entirely and this is what will say whether it agrees with the tracer.
	// A TRANSPARENCY FIXTURE, built so the answer is checkable rather than merely
	// plausible.
	//
	// Three panes at three alphas stand in front of three opaque posts, and the
	// panes OVERLAP each other in depth -- a single layer of transparency looks
	// correct under almost any ordering, so a fixture without overlap tests the
	// blend factor and nothing about the sorting.
	//
	// The reference is the path tracer with alpha-as-coverage, which needs no
	// ordering at all, so a raster pass that sorts wrongly disagrees with it.
	void BuildTransparencyScene(std::shared_ptr<Scene> scene) {
		{
			EntityHandle cam = scene->CreateEntity("Main Camera");
			cam.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 2.4f, -7.0f });
			cam.GetComponent<TransformComponent>().SetRotation({ 8.0f, 0.0f, 0.0f });
			auto& camera = cam.GetOrAddComponent<CameraComponent>();
			camera.isMain = true;
			camera.fov = 55.0f;
		}

		{
			EntityHandle ground = scene->CreateEntity("Ground");
			auto& transform = ground.GetComponent<TransformComponent>();
			transform.SetScale({ 40.0f, 1.0f, 40.0f });
			auto& mesh = ground.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE);
			mesh.sourceName = "Plane";
			auto& m = ground.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.55f, 0.55f, 0.58f, 1.0f };
			m.roughness = 0.85f;
		}

		// Opaque posts, so there is something with a known colour BEHIND the
		// panes. Transparency over sky alone cannot distinguish a blend that is
		// too strong from one that is too weak.
		for (int i = 0; i < 3; ++i) {
			EntityHandle e = scene->CreateEntity("Post" + std::to_string(i));
			auto& transform = e.GetComponent<TransformComponent>();
			transform.SetTranslation({ -3.0f + 3.0f * float(i), 0.9f, 1.6f });
			transform.SetScale({ 1.1f, 1.8f, 1.1f });
			auto& mesh = e.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::CUBE);
			mesh.sourceName = "Cube";
			auto& m = e.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.85f, 0.25f, 0.2f, 1.0f };
			m.roughness = 0.5f;
		}

		// The panes. Alpha descends left to right, and each sits at a different
		// depth so every pair overlaps from this camera.
		struct Pane { float x; float z; float alpha; glm::vec3 tint; };
		const Pane panes[] = {
			{ -3.0f, -0.6f, 0.75f, { 0.30f, 0.55f, 0.95f } },
			{  0.0f, -1.4f, 0.50f, { 0.35f, 0.90f, 0.45f } },
			{  3.0f, -2.2f, 0.25f, { 0.95f, 0.85f, 0.30f } },
		};
		for (int i = 0; i < 3; ++i) {
			const Pane& p = panes[i];
			EntityHandle e = scene->CreateEntity("Pane" + std::to_string(i));
			auto& transform = e.GetComponent<TransformComponent>();
			transform.SetTranslation({ p.x, 1.3f, p.z });
			// A plane is XZ with its normal along +Y, so it is rotated upright.
			// MINUS 90, not plus: +90 stands it up with the normal pointing AWAY
			// from a camera at -Z, and the path tracer rejects back faces -- the
			// panes then render as nothing at all while still casting shadows,
			// which reads as a transparency bug rather than a winding one.
			transform.SetRotation({ -90.0f, 0.0f, 0.0f });
			transform.SetScale({ 4.2f, 1.0f, 2.6f });
			auto& mesh = e.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE);
			mesh.sourceName = "Plane";
			auto& m = e.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { p.tint.x, p.tint.y, p.tint.z, p.alpha };
			m.roughness = 0.25f;
		}

		{
			EntityHandle sun = scene->CreateEntity("Directional");
			sun.GetComponent<TransformComponent>().SetRotation({ 55.0f, 20.0f, 0.0f });
			auto& light = sun.GetOrAddComponent<LightComponent>();
			light.type = LightType::DIRECTIONAL;
			light.color = { 1.0f, 0.96f, 0.9f };
			light.intensity = 2.4f;
		}
	}

	// SOFT SHADOWS, built so the penumbra is the thing being measured.
	//
	// Three posts at three HEIGHTS above the ground, lit by one wide light. A
	// penumbra widens with the distance between caster and receiver, so posts at
	// different heights produce visibly different softness from the same light --
	// a fixture with everything at one height would test the tap pattern and
	// nothing about whether the penumbra scales at all.
	//
	// The light is deliberately wide (4 degrees, about eight times the sun) so
	// the eight-tap banding is visible rather than hidden. If it were subtle, a
	// regression that collapsed soft shadows back to hard ones would move the
	// image by almost nothing.
	void BuildSoftShadowsScene(std::shared_ptr<Scene> scene) {
		{
			EntityHandle cam = scene->CreateEntity("Main Camera");
			cam.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 4.0f, -9.0f });
			cam.GetComponent<TransformComponent>().SetRotation({ 18.0f, 0.0f, 0.0f });
			auto& camera = cam.GetOrAddComponent<CameraComponent>();
			camera.isMain = true;
			camera.fov = 55.0f;
		}
		{
			EntityHandle ground = scene->CreateEntity("Ground");
			auto& t = ground.GetComponent<TransformComponent>();
			t.SetScale({ 40.0f, 1.0f, 40.0f });
			auto& mesh = ground.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE);
			mesh.sourceName = "Plane";
			auto& m = ground.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.6f, 0.6f, 0.62f, 1.0f };
			m.roughness = 0.9f;
		}

		const float heights[] = { 0.8f, 2.0f, 3.6f };
		for (int i = 0; i < 3; ++i) {
			EntityHandle e = scene->CreateEntity("Caster" + std::to_string(i));
			auto& t = e.GetComponent<TransformComponent>();
			t.SetTranslation({ -3.4f + 3.4f * float(i), heights[i], 0.0f });
			t.SetScale({ 1.2f, 0.18f, 1.2f });
			auto& mesh = e.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::CUBE);
			mesh.sourceName = "Cube";
			auto& m = e.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.8f, 0.4f, 0.3f, 1.0f };
			m.roughness = 0.6f;
		}

		{
			// A POINT light, not the directional one, so this exercises the
			// TRACED soft path rather than the cascaded map -- the cascades are
			// hard-edged and belong to the directional light alone.
			EntityHandle pl = scene->CreateEntity("WideLight");
			pl.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 9.0f, -1.0f });
			auto& light = pl.GetOrAddComponent<LightComponent>();
			light.type = LightType::POINT;
			light.color = { 1.0f, 0.97f, 0.92f };
			light.intensity = 22.0f;
			light.range = 40.0f;
			light.attenuation = 0.02f;
			light.softnessDegrees = 4.0f;
		}
	}

	// THE LIGHT-LEAK TEST, which ENGINE_PLAN.md asks for by name: "leaking is the
	// classic DDGI failure and will not be obvious from one screenshot".
	//
	// A SEALED BOX with the camera inside it, under a strong sun outside. No
	// light path exists into the interior, so the correct image is BLACK and the
	// test is a single number: the mean interior brightness. Any light at all
	// arrived through a wall.
	//
	// The first version of this was an open-sided slab above a floor, which
	// tested nothing -- light legitimately entered from the sides, so a leak and
	// a correct result looked the same. Enclosure is the whole point.
	//
	// Walls are FOUR UNITS THICK, and that number is measured rather than picked.
	// The probe volume is auto-fitted to the entity origins plus a margin, which
	// for this scene gives a spacing of roughly 2.4 units horizontally and 3.3
	// vertically. A wall thinner than the spacing has probes sitting INSIDE it
	// that can see both faces, and no visibility weighting can recover from a
	// probe that is genuinely lit on both sides -- that is a probe-placement
	// failure, not a leak, and a fixture built that way tests the wrong thing.
	//
	// The first version used one-unit walls and the sealed interior came out at
	// a mean of 123/255, which is the failure above rather than a leak through
	// intact geometry. Four units is comfortably wider than the spacing.
	void BuildLeakTestScene(std::shared_ptr<Scene> scene) {
		{
			EntityHandle cam = scene->CreateEntity("Main Camera");
			cam.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 1.4f, -2.2f });
			auto& camera = cam.GetOrAddComponent<CameraComponent>();
			camera.isMain = true;
			camera.fov = 70.0f;
		}

		// Floor, ceiling and four walls, forming a closed 8 x 3 x 8 interior.
		// WHITE, deliberately: a dark interior would absorb a leak and hide it,
		// and the bounce feedback would damp it further. White is the worst case
		// and therefore the honest one.
		struct Slab { const char* name; glm::vec3 pos; glm::vec3 scale; };
		const Slab slabs[] = {
			{ "Floor",    {  0.0f, -2.0f,  0.0f }, { 16.0f, 4.0f, 16.0f } },
			{ "Ceiling",  {  0.0f,  5.0f,  0.0f }, { 16.0f, 4.0f, 16.0f } },
			{ "WallNorth",{  0.0f,  1.5f,  6.0f }, { 16.0f, 11.0f,  4.0f } },
			{ "WallSouth",{  0.0f,  1.5f, -6.0f }, { 16.0f, 11.0f,  4.0f } },
			{ "WallEast", {  6.0f,  1.5f,  0.0f }, {  4.0f, 11.0f, 16.0f } },
			{ "WallWest", { -6.0f,  1.5f,  0.0f }, {  4.0f, 11.0f, 16.0f } },
		};
		for (const Slab& sl : slabs) {
			EntityHandle e = scene->CreateEntity(sl.name);
			auto& t = e.GetComponent<TransformComponent>();
			t.SetTranslation(sl.pos);
			t.SetScale(sl.scale);
			auto& mesh = e.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::CUBE);
			mesh.sourceName = "Cube";
			auto& m = e.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.9f, 0.9f, 0.9f, 1.0f };
			m.roughness = 0.9f;
		}

		{
			// Outside and bright. The whole exterior is in full sun, so the probe
			// volume is full of high radiance a hand's breadth from the interior
			// -- which is precisely the gradient a leak exploits.
			EntityHandle sun = scene->CreateEntity("Directional");
			sun.GetComponent<TransformComponent>().SetRotation({ 60.0f, 25.0f, 0.0f });
			auto& light = sun.GetOrAddComponent<LightComponent>();
			light.type = LightType::DIRECTIONAL;
			light.color = { 1.0f, 1.0f, 1.0f };
			light.intensity = 8.0f;
		}
	}

	// AMBIENT OUTSIDE THE PROBE VOLUME, and it exists because a reported bug
	// lived exactly here.
	//
	// NO DIRECTIONAL LIGHT AT ALL. Every other fixture has a sun, and a sun
	// masks this completely: the ground is lit whether or not the ambient term
	// works. With the sun removed, ambient is all there is, and a surface that
	// receives none is unmistakably BLACK rather than slightly dark.
	//
	// A DELIBERATELY HUGE GROUND PLANE -- 120 units against a probe volume fitted
	// to entity origins plus a few units of margin. Most of this plane is
	// therefore OUTSIDE the volume, where DdgiSampleIrradiance returns zero. That
	// zero means "no opinion", not "no light", and a shading path that treats the
	// two alike renders the whole outer plane black with a lit patch in the
	// middle. That is precisely what was reported.
	//
	// One point light gives a small island of direct lighting, so a completely
	// black frame is distinguishable from a completely unlit one.
	void BuildAmbientFalloffScene(std::shared_ptr<Scene> scene) {
		{
			EntityHandle cam = scene->CreateEntity("Main Camera");
			cam.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 14.0f, -26.0f });
			cam.GetComponent<TransformComponent>().SetRotation({ 24.0f, 0.0f, 0.0f });
			auto& camera = cam.GetOrAddComponent<CameraComponent>();
			camera.isMain = true;
			camera.fov = 60.0f;
		}
		{
			EntityHandle ground = scene->CreateEntity("Ground");
			auto& t = ground.GetComponent<TransformComponent>();
			t.SetScale({ 120.0f, 1.0f, 120.0f });
			auto& mesh = ground.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE);
			mesh.sourceName = "Plane";
			auto& m = ground.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.75f, 0.75f, 0.78f, 1.0f };
			m.roughness = 0.9f;
		}
		{
			EntityHandle s = scene->CreateEntity("Sphere");
			auto& t = s.GetComponent<TransformComponent>();
			t.SetTranslation({ 0.0f, 2.0f, 0.0f });
			t.SetScale(glm::vec3(3.0f));
			auto& mesh = s.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::SPHERE);
			mesh.sourceName = "Sphere";
			auto& m = s.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.8f, 0.78f, 0.75f, 1.0f };
			m.roughness = 0.4f;
		}
		{
			EntityHandle pl = scene->CreateEntity("Point");
			pl.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 6.0f, 0.0f });
			auto& light = pl.GetOrAddComponent<LightComponent>();
			light.type = LightType::POINT;
			light.color = { 1.0f, 0.9f, 0.8f };
			light.intensity = 30.0f;
			light.range = 25.0f;
			light.attenuation = 0.05f;
		}
	}

	void BuildLightsScene(std::shared_ptr<Scene> scene) {
		{
			EntityHandle cam = scene->CreateEntity("Main Camera");
			cam.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 3.2f, -8.0f });
			cam.GetComponent<TransformComponent>().SetRotation({ 12.0f, 0.0f, 0.0f });
			auto& camera = cam.GetOrAddComponent<CameraComponent>();
			camera.isMain = true;
			camera.fov = 55.0f;
		}

		{
			EntityHandle ground = scene->CreateEntity("Ground");
			auto& transform = ground.GetComponent<TransformComponent>();
			transform.SetScale({ 40.0f, 1.0f, 40.0f });
			auto& mesh = ground.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE);
			mesh.sourceName = "Plane";
			auto& m = ground.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.55f, 0.55f, 0.58f, 1.0f };
			m.roughness = 0.85f;
		}

		struct Post { const char* name; float x; uint64_t prim; const char* primName; };
		const Post posts[] = {
			{ "UnderDirectional", -3.2f, PrimitiveMeshGUIDs::SPHERE,   "Sphere" },
			{ "UnderPoint",        0.0f, PrimitiveMeshGUIDs::CUBE,     "Cube" },
			{ "UnderSpot",         3.2f, PrimitiveMeshGUIDs::CYLINDER, "Cylinder" },
		};
		for (const Post& p : posts) {
			EntityHandle e = scene->CreateEntity(p.name);
			auto& transform = e.GetComponent<TransformComponent>();
			transform.SetTranslation({ p.x, 0.7f, 0.0f });
			transform.SetScale(glm::vec3(1.3f));
			auto& mesh = e.GetOrAddComponent<MeshComponent>();
			mesh.guid = static_cast<LR_GUID>(p.prim);
			mesh.sourceName = p.primName;
			auto& m = e.GetOrAddComponent<MaterialComponent>().slots[0];
			m.color = { 0.8f, 0.78f, 0.75f, 1.0f };
			m.roughness = 0.45f;
		}

		{
			EntityHandle sun = scene->CreateEntity("Directional");
			sun.GetComponent<TransformComponent>().SetRotation({ 55.0f, 20.0f, 0.0f });
			auto& light = sun.GetOrAddComponent<LightComponent>();
			light.type = LightType::DIRECTIONAL;
			light.color = { 1.0f, 0.95f, 0.85f };
			light.intensity = 2.2f;
		}
		{
			EntityHandle pl = scene->CreateEntity("Point");
			pl.GetComponent<TransformComponent>().SetTranslation({ 0.0f, 2.6f, -1.6f });
			auto& light = pl.GetOrAddComponent<LightComponent>();
			light.type = LightType::POINT;
			light.color = { 0.4f, 0.7f, 1.0f };
			light.intensity = 12.0f;
			light.range = 9.0f;
			light.attenuation = 0.25f;
		}
		{
			EntityHandle sp = scene->CreateEntity("Spot");
			auto& transform = sp.GetComponent<TransformComponent>();
			transform.SetTranslation({ 3.2f, 4.0f, -1.2f });
			transform.SetRotation({ 70.0f, 0.0f, 0.0f });
			auto& light = sp.GetOrAddComponent<LightComponent>();
			light.type = LightType::SPOT;
			light.color = { 1.0f, 0.6f, 0.3f };
			light.intensity = 25.0f;
			light.range = 14.0f;
			light.attenuation = 0.15f;
			light.innerConeAngle = 14.0f;
			light.outerConeAngle = 26.0f;
		}
	}

}

int main(int argc, char** argv) {
	Log::Init();

	Options opts;
	if (!ParseArgs(argc, argv, opts)) { return 2; }

	if (!opts.verifyOnly.empty()) {
		return VerifyRoundTrip(opts.verifyOnly, LR_GUID::INVALID, LR_GUID::INVALID) ? 0 : 1;
	}

	LOG_ENGINE_INFO("fixture-gen: out={} model={} skybox={}",
		opts.outFolder.string(), opts.modelPath.string(), opts.skyboxPath.string());

	const Fixture fixture = ParseFixture(opts.fixture);

	// The primitive fixtures need only the skybox. Requiring the model as well
	// would make them fail on a checkout that skipped SampleModels, which is
	// exactly the dependency they exist to avoid.
	if (fixture == Fixture::Smoke && !fs::exists(opts.modelPath)) {
		LOG_ENGINE_CRITICAL("asset does not exist: {}", opts.modelPath.string());
		return 1;
	}
	if (!fs::exists(opts.skyboxPath)) {
		LOG_ENGINE_CRITICAL("asset does not exist: {}", opts.skyboxPath.string());
		return 1;
	}

	if (!ClearExistingProject(opts.outFolder, opts.force)) { return 1; }

	// --- create the project -------------------------------------------------
	ProjectManager pm;
	if (!pm.NewProject(opts.outFolder)) {
		LOG_ENGINE_CRITICAL("NewProject failed for {}", opts.outFolder.string());
		return 1;
	}

	pm.GetMutableRuntimeRenderSettings().resolution = { 640, 360 };
	pm.GetMutableRuntimeRenderSettings().raysPerPixel = 1;
	pm.GetMutableRuntimeRenderSettings().bouncesPerRay = 3;

	auto assetManager = pm.GetAssetManager();
	auto sceneManager = pm.GetSceneManager();

	// --- import assets ------------------------------------------------------
	LR_GUID meshGuid = LR_GUID::INVALID;
	if (fixture == Fixture::Smoke) {
		meshGuid = assetManager->ImportAsset(opts.modelPath);
		if (meshGuid == LR_GUID::INVALID) {
			LOG_ENGINE_CRITICAL("failed to import model {}", opts.modelPath.string());
			return 1;
		}
	}
	LR_GUID skyboxGuid = assetManager->ImportAsset(opts.skyboxPath);
	if (skyboxGuid == LR_GUID::INVALID) {
		LOG_ENGINE_CRITICAL("failed to import skybox {}", opts.skyboxPath.string());
		return 1;
	}

	// --- the primitive fixtures -------------------------------------------
	if (fixture != Fixture::Smoke) {
		const char* sceneName = (fixture == Fixture::Materials)    ? "MaterialsScene"
		                      : (fixture == Fixture::Transparency) ? "TransparencyScene"
		                      : (fixture == Fixture::SoftShadows)  ? "SoftShadowsScene"
		                      : (fixture == Fixture::LeakTest)     ? "LeakTestScene"
		                      : (fixture == Fixture::AmbientFalloff) ? "AmbientFalloffScene"
		                                                           : "LightsScene";
		LR_GUID sceneGuid = sceneManager->CreateScene(sceneName);
		auto scene = sceneManager->find(sceneGuid);
		if (!scene) {
			LOG_ENGINE_CRITICAL("CreateScene returned a GUID with no scene behind it");
			return 1;
		}
		scene->skyboxGuid = skyboxGuid;
		scene->skyboxName = opts.skyboxPath.filename().string();

		if (fixture == Fixture::Materials)         BuildMaterialsScene(scene);
		else if (fixture == Fixture::Transparency) BuildTransparencyScene(scene);
		else if (fixture == Fixture::SoftShadows)  BuildSoftShadowsScene(scene);
		else if (fixture == Fixture::LeakTest)     BuildLeakTestScene(scene);
		else if (fixture == Fixture::AmbientFalloff) BuildAmbientFalloffScene(scene);
		else                               BuildLightsScene(scene);

		sceneManager->SetOpenSceneGuid(sceneGuid);
		pm.SetBootSceneGuid(sceneGuid);
		if (!pm.SaveProject()) {
			LOG_ENGINE_CRITICAL("SaveProject failed");
			return 1;
		}
		LOG_ENGINE_INFO("fixture-gen: wrote {} to {}", sceneName, opts.outFolder.string());
		return 0;
	}

	const Bounds bounds = MeshBounds(*assetManager->GetAssetPool(), meshGuid);
	if (!bounds.valid) {
		LOG_ENGINE_CRITICAL("imported model has no triangles: {}", opts.modelPath.string());
		return 1;
	}
	LOG_ENGINE_INFO("model bounds: min=({:.3f},{:.3f},{:.3f}) max=({:.3f},{:.3f},{:.3f}) radius={:.3f}",
		bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y, bounds.max.z, bounds.radius());

	// --- build the scene ----------------------------------------------------
	LR_GUID sceneGuid = sceneManager->CreateScene("SmokeScene");
	auto scene = sceneManager->find(sceneGuid);
	if (!scene) {
		LOG_ENGINE_CRITICAL("CreateScene returned a GUID with no scene behind it");
		return 1;
	}
	scene->skyboxGuid = skyboxGuid;
	scene->skyboxName = opts.skyboxPath.filename().string();

	// Sample models come in wildly different units (the bunny is ~160 units
	// across). Normalize the imported model to roughly unit radius, sitting on
	// y = 0 at the origin, so the rest of the scene - and the editor camera,
	// which starts 10 units out - can use fixed, readable numbers.
	const float modelScale = 1.0f / glm::max(bounds.radius(), 0.001f);
	const float modelHeight = bounds.size().y * modelScale;
	const glm::vec3 modelOffset = -bounds.center() * modelScale + glm::vec3(0.0f, modelHeight * 0.5f, 0.0f);

	// CAMERA. Identity rotation looks down +Z (see Renderer::Parse and the light
	// forward vector), so the camera sits at -Z looking back at the model.
	{
		EntityHandle cam = scene->CreateEntity("Main Camera");
		auto& transform = cam.GetComponent<TransformComponent>();
		transform.SetTranslation({ 0.0f, modelHeight * 0.5f, -3.5f });
		auto& camera = cam.GetOrAddComponent<CameraComponent>();
		camera.isMain = true;
		camera.fov = 60.0f;
	}

	// MODEL
	{
		EntityHandle model = scene->CreateEntity(opts.modelPath.stem().string());
		auto& transform = model.GetComponent<TransformComponent>();
		transform.SetScale(glm::vec3(modelScale));
		transform.SetTranslation(modelOffset);

		auto& mesh = model.GetOrAddComponent<MeshComponent>();
		mesh.guid = meshGuid;
		mesh.sourceName = opts.modelPath.filename().string();

		auto& material = model.GetOrAddComponent<MaterialComponent>().slots[0];
		material.color = { 0.80f, 0.72f, 0.62f, 1.0f };
		material.emission = { 0.0f, 0.0f, 0.0f, 0.0f };
		material.metallic = 0.0f;
		material.roughness = 0.35f;
		material.ao = 1.0f;
	}

	// GROUND (built-in primitive plane, no asset file needed)
	{
		EntityHandle ground = scene->CreateEntity("Ground");
		auto& transform = ground.GetComponent<TransformComponent>();
		transform.SetTranslation({ 0.0f, 0.0f, 0.0f });
		transform.SetScale({ 20.0f, 1.0f, 20.0f });

		auto& mesh = ground.GetOrAddComponent<MeshComponent>();
		mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::PLANE);
		mesh.sourceName = "Plane";

		auto& material = ground.GetOrAddComponent<MaterialComponent>().slots[0];
		material.color = { 0.45f, 0.45f, 0.48f, 1.0f };
		material.metallic = 0.0f;
		material.roughness = 0.85f;
		material.ao = 1.0f;
	}

	// EMISSIVE SPHERE — gives the path tracer a bright, obviously-lit surface.
	{
		EntityHandle lamp = scene->CreateEntity("Emissive Sphere");
		auto& transform = lamp.GetComponent<TransformComponent>();
		transform.SetTranslation({ 1.8f, modelHeight + 0.8f, -0.8f });
		transform.SetScale(glm::vec3(0.8f));

		auto& mesh = lamp.GetOrAddComponent<MeshComponent>();
		mesh.guid = static_cast<LR_GUID>(PrimitiveMeshGUIDs::SPHERE);
		mesh.sourceName = "Sphere";

		auto& material = lamp.GetOrAddComponent<MaterialComponent>().slots[0];
		material.emission = { 1.0f, 0.92f, 0.80f, 6.0f };
		material.color = { 1.0f, 1.0f, 1.0f, 1.0f };
	}

	// SUN (directional). Forward is +Z rotated by the transform, so pitch it down.
	{
		EntityHandle sun = scene->CreateEntity("Sun");
		auto& transform = sun.GetComponent<TransformComponent>();
		transform.SetTranslation({ 0.0f, 6.0f, 0.0f });
		transform.SetRotation({ 50.0f, -30.0f, 0.0f });

		auto& light = sun.GetOrAddComponent<LightComponent>();
		light.type = LightType::DIRECTIONAL;
		light.color = { 1.0f, 0.96f, 0.88f };
		light.intensity = 3.0f;
	}

	// FILL (point light)
	{
		EntityHandle fill = scene->CreateEntity("Fill Light");
		auto& transform = fill.GetComponent<TransformComponent>();
		transform.SetTranslation({ -2.5f, 2.0f, -2.0f });

		auto& light = fill.GetOrAddComponent<LightComponent>();
		light.type = LightType::POINT;
		light.color = { 0.55f, 0.68f, 1.0f };
		light.intensity = 2.0f;
		light.range = 12.0f;
		light.attenuation = 1.0f;
	}

	sceneManager->SetOpenSceneGuid(sceneGuid);
	pm.SetBootSceneGuid(sceneGuid);

	if (!pm.SaveProject()) {
		LOG_ENGINE_CRITICAL("SaveProject failed");
		return 1;
	}

	const fs::path projectFile = ComposeProjectFilepath(opts.outFolder);
	LOG_ENGINE_INFO("wrote fixture: {}", projectFile.string());
	LOG_ENGINE_INFO("  scene GUID  {}", (uint64_t)sceneGuid);
	LOG_ENGINE_INFO("  mesh GUID   {}", (uint64_t)meshGuid);
	LOG_ENGINE_INFO("  skybox GUID {}", (uint64_t)skyboxGuid);

	if (!VerifyRoundTrip(projectFile, meshGuid, skyboxGuid)) {
		return 1;
	}

	LOG_ENGINE_INFO("fixture-gen: OK -> {}", projectFile.string());
	return 0;
}
