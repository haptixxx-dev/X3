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

	struct Options {
		fs::path outFolder  = fs::path(X3_SOURCE_DIR) / "TestProject";
		fs::path modelPath  = fs::path(X3_SOURCE_DIR) / "SampleModels" / "stanford_bunny_pbr.glb";
		fs::path skyboxPath = fs::path(X3_SOURCE_DIR) / "SampleSkyboxes" / "kloofendal_48d_partly_cloudy_puresky_4k.hdr";
		bool force = false;
		fs::path verifyOnly; // non-empty: check this project and exit
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
			if (idx >= pool.MeshBuffer.size()) { break; }
			const Triangle& tri = pool.MeshBuffer[idx];
			for (const glm::vec4& v : { tri.v0, tri.v1, tri.v2 }) {
				b.min = glm::min(b.min, glm::vec3(v));
				b.max = glm::max(b.max, glm::vec3(v));
			}
			b.valid = true;
		}
		return b;
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

int main(int argc, char** argv) {
	Log::Init();

	Options opts;
	if (!ParseArgs(argc, argv, opts)) { return 2; }

	if (!opts.verifyOnly.empty()) {
		return VerifyRoundTrip(opts.verifyOnly, LR_GUID::INVALID, LR_GUID::INVALID) ? 0 : 1;
	}

	LOG_ENGINE_INFO("fixture-gen: out={} model={} skybox={}",
		opts.outFolder.string(), opts.modelPath.string(), opts.skyboxPath.string());

	for (const auto& asset : { opts.modelPath, opts.skyboxPath }) {
		if (!fs::exists(asset)) {
			LOG_ENGINE_CRITICAL("asset does not exist: {}", asset.string());
			return 1;
		}
	}

	if (!ClearExistingProject(opts.outFolder, opts.force)) { return 1; }

	// --- create the project -------------------------------------------------
	ProjectManager pm;
	if (!pm.NewProject(opts.outFolder)) {
		LOG_ENGINE_CRITICAL("NewProject failed for {}", opts.outFolder.string());
		return 1;
	}

	// This branch is Vulkan-only; say so in the project file so the log does not
	// claim otherwise. (The factories are compile-time, so this is descriptive.)
	pm.GetMutableRuntimeRenderSettings().rendererAPI = RendererAPI::Vulkan;
	pm.GetMutableRuntimeRenderSettings().resolution = { 640, 360 };
	pm.GetMutableRuntimeRenderSettings().raysPerPixel = 1;
	pm.GetMutableRuntimeRenderSettings().bouncesPerRay = 3;

	auto assetManager = pm.GetAssetManager();
	auto sceneManager = pm.GetSceneManager();

	// --- import assets ------------------------------------------------------
	LR_GUID meshGuid = assetManager->ImportAsset(opts.modelPath);
	if (meshGuid == LR_GUID::INVALID) {
		LOG_ENGINE_CRITICAL("failed to import model {}", opts.modelPath.string());
		return 1;
	}
	LR_GUID skyboxGuid = assetManager->ImportAsset(opts.skyboxPath);
	if (skyboxGuid == LR_GUID::INVALID) {
		LOG_ENGINE_CRITICAL("failed to import skybox {}", opts.skyboxPath.string());
		return 1;
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

		auto& material = model.GetOrAddComponent<MaterialComponent>();
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

		auto& material = ground.GetOrAddComponent<MaterialComponent>();
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

		auto& material = lamp.GetOrAddComponent<MaterialComponent>();
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
