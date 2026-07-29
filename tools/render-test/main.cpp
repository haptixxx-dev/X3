// =============================================================================
// X3RenderTest -- golden-image regression tests for the renderer.
//
// WHY THIS EXISTS. Every gate the engine had before this one answered "does it
// crash, and does Vulkan complain". None of them looked at the image. Phases 2
// through 5 all passed verify.sh with clean validation while the committed
// fixture rendered a completely untextured model; a human eyeballing a
// screenshot is what caught it.
//
// It also unblocks two gates the plan asks for and that were previously
// unrunnable:
//   * Phase 3 wanted the post-Slang image diffed against the pre-Slang one.
//   * Phase 7 wants every raster pass diffed against the path-traced reference,
//     which is the whole reason for keeping a reference renderer.
//
// HOW IT WORKS. For each scenario in tests/scenarios.yaml it opens a project,
// forces the scene's main camera, applies the scenario's RenderSettings, renders
// a fixed number of frames, reads the render target back with
// VulkanContext::readbackImage, and encodes it to PNG. If a golden exists it
// reports RMSE and max per-channel delta and writes a diff image on failure;
// with --update-goldens it overwrites the golden instead. Finally it tiles every
// output into one contact sheet, which is the thing to actually look at.
//
// DETERMINISM. Scenarios render a FIXED frame count with a fixed sample count
// and the scene camera, never the editor camera, so nothing depends on wall
// clock, input, or window size. The path tracer's RNG is seeded from
// (pixel, accumulated frame index, sample index) -- see InitRngState -- so a
// given frame count is reproducible.
//
// NOT HEADLESS. A display connection is still required: the frame lifecycle is
// built around swapchain acquire. The window is created unmapped so nothing
// flashes on screen, but this will not run over a bare SSH session. Making it
// truly surfaceless means an offscreen path through beginFrame/endFrame and is
// deliberately out of scope.
// =============================================================================

#include "X3.h"
#include "Core/application.h"
#include "Core/Events/RenderEvents.h"
#include "Core/Layers/ILayer.h"
#include "Core/Layers/LayerStack.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Project/ProjectManager.h"
#include "Renderer/RenderSettings.h"

#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifndef X3_SOURCE_DIR
#define X3_SOURCE_DIR "."
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// One row of tests/scenarios.yaml.
// ---------------------------------------------------------------------------
struct Scenario {
	std::string name;
	std::string project;          // relative to the repo root
	X3::ShaderType shader = X3::ShaderType::PATH_TRACING;
	uint32_t width  = 640;
	uint32_t height = 360;
	int  raysPerPixel  = 1;
	int  bouncesPerRay = 4;
	bool accumulate    = false;
	int  debugMode     = 0;
	/// How many frames to render before reading back. With accumulate=true this
	/// is the sample count multiplier, and it is what makes a path-traced
	/// scenario converge to something stable enough to diff.
	uint32_t frames = 8;

	/// Run the white-furnace energy assertions on this scenario's FLOAT output
	/// instead of only diffing its PNG. See res/shaders/FurnaceTest.slang.
	bool energyTest = false;
};

// ---------------------------------------------------------------------------
// White furnace energy check.
//
// RUNS ON THE FLOATS, NOT THE PNG, and that is the whole point. An 8-bit encode
// clamps at 1.0, so a BSDF reflecting 1.4x the energy it receives -- which makes
// a path tracer diverge as bounces rise -- encodes to exactly the same 255 as a
// perfectly conserving one. The hard error is invisible in the image and obvious
// in the buffer.
// ---------------------------------------------------------------------------
struct EnergyReport {
	float maxMetal = 0.0f, minMetal = 1e9f, meanMetal = 0.0f;
	float maxDielectric = 0.0f;
	// WHERE the maxima are. Without this a value over 1 is ambiguous between a
	// broken lobe and ordinary Monte-Carlo variance at the grazing edge, and
	// those want completely different responses.
	float maxMetalRoughness = 0.0f, maxMetalCosTheta = 0.0f;
	float maxDielRoughness  = 0.0f, maxDielCosTheta  = 0.0f;
	bool  gainsEnergy = false;
};

EnergyReport checkEnergy(const std::vector<float>& rgba, uint32_t width, uint32_t height) {
	EnergyReport r;
	double sum = 0.0;
	size_t n = 0;

	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			const size_t i = (size_t(y) * width + x) * 4;
			const float metal = rgba[i + 0];
			const float diel  = rgba[i + 1];

			// The shader's parameterisation: x is roughness, y is cos(theta_o).
			const float roughness = (float(x) + 0.5f) / float(width);
			const float cosTheta  = (float(y) + 0.5f) / float(height);

			if (metal > r.maxMetal) {
				r.maxMetal = metal;
				r.maxMetalRoughness = roughness;
				r.maxMetalCosTheta  = cosTheta;
			}
			if (diel > r.maxDielectric) {
				r.maxDielectric = diel;
				r.maxDielRoughness = roughness;
				r.maxDielCosTheta  = cosTheta;
			}
			r.minMetal = std::min(r.minMetal, metal);
			sum += metal;
			++n;
		}
	}
	r.meanMetal = n ? float(sum / double(n)) : 0.0f;

	// Tolerance is Monte-Carlo noise, not slack in the physics. 4096 samples of
	// a bounded estimator leaves well under a percent; anything above this is a
	// real over-unity lobe.
	constexpr float kGainTolerance = 1.01f;
	r.gainsEnergy = r.maxMetal > kGainTolerance || r.maxDielectric > kGainTolerance;
	return r;
}

X3::ShaderType parseShader(const std::string& s) {
	if (s == "pathtracing" || s == "path_tracing") return X3::ShaderType::PATH_TRACING;
	if (s == "pbr")                                return X3::ShaderType::PBR;
	if (s == "phong")                              return X3::ShaderType::PHONG;
	if (s == "furnace")                            return X3::ShaderType::FURNACE_TEST;
	LOG_ENGINE_CRITICAL("unknown shader '{}' in scenarios.yaml", s);
	return X3::ShaderType::PATH_TRACING;
}

// ---------------------------------------------------------------------------
// Encoding. The viewport samples the RGBA32F target directly and lets the
// hardware clamp, so the PNG does the same: clamp to [0,1] and scale. NO extra
// tonemap or gamma is applied here -- PBR and Phong already tonemap and
// gamma-correct internally, and the path tracer deliberately does not, so
// applying anything here would make the PNG disagree with what the editor shows.
// ---------------------------------------------------------------------------
std::vector<unsigned char> encode(const std::vector<float>& rgba,
                                  uint32_t width, uint32_t height) {
	// THE VERTICAL FLIP IS REQUIRED, not cosmetic. The render target is stored
	// with Vulkan's top-left origin, but the camera ray in the shaders builds NDC
	// with +y up, so row 0 of the image is the BOTTOM of the picture. Both display
	// paths already compensate -- blitImageToSwapchain carries an explicit Y-flip
	// packing, and the editor samples through flipped ImGui UVs -- so a PNG
	// written from the raw rows comes out upside down and would silently disagree
	// with every screenshot anyone takes of the same scene.
	std::vector<unsigned char> out(rgba.size());
	const size_t rowFloats = size_t(width) * 4;

	for (uint32_t y = 0; y < height; ++y) {
		const size_t srcRow = size_t(height - 1 - y) * rowFloats;
		const size_t dstRow = size_t(y) * rowFloats;
		for (size_t i = 0; i < rowFloats; ++i) {
			const float c = std::clamp(rgba[srcRow + i], 0.0f, 1.0f);
			out[dstRow + i] = static_cast<unsigned char>(c * 255.0f + 0.5f);
		}
	}
	return out;
}

struct Comparison {
	bool   goldenExisted = false;
	bool   sizeMismatch  = false;
	double rmse = 0.0;       // normalised to [0,1]
	int    maxDelta = 0;     // per channel, 0-255
	float  diffGain = 1.0f;  // amplification applied to the written diff image
};

Comparison compareToGolden(const std::vector<unsigned char>& actual,
                           uint32_t width, uint32_t height,
                           const fs::path& goldenPath,
                           const fs::path& diffPath) {
	Comparison result;
	if (!fs::exists(goldenPath)) return result;
	result.goldenExisted = true;

	int gw = 0, gh = 0, gc = 0;
	stbi_set_flip_vertically_on_load_thread(0);
	unsigned char* golden = stbi_load(goldenPath.string().c_str(), &gw, &gh, &gc, 4);
	if (!golden) {
		LOG_ENGINE_WARN("could not read golden {}", goldenPath.string());
		result.goldenExisted = false;
		return result;
	}

	if (uint32_t(gw) != width || uint32_t(gh) != height) {
		result.sizeMismatch = true;
		stbi_image_free(golden);
		return result;
	}

	double sumSq = 0.0;
	size_t counted = 0;

	for (size_t i = 0; i < actual.size(); ++i) {
		if ((i % 4) == 3) continue;   // alpha excluded from the metric
		const int d = int(actual[i]) - int(golden[i]);
		result.maxDelta = std::max(result.maxDelta, std::abs(d));
		const double n = double(d) / 255.0;
		sumSq += n * n;
		++counted;
	}
	result.rmse = counted ? std::sqrt(sumSq / double(counted)) : 0.0;

	// THE DIFF IS NORMALISED TO ITS OWN MAXIMUM, in a second pass, rather than
	// amplified by a fixed factor. A regression worth catching is often only two
	// or three levels out of 255 -- the deliberate one used to test this harness
	// was max 3 -- and at any fixed gain that renders as near-black, which reads
	// as "nothing changed" precisely when something did. Scaling to the max makes
	// every failure legible; the MAGNITUDE is what the reported rmse and max are
	// for, so the image only has to answer WHERE.
	if (result.maxDelta > 0) {
		result.diffGain = 255.0f / float(result.maxDelta);

		std::vector<unsigned char> diff(actual.size(), 255);
		for (size_t i = 0; i < actual.size(); ++i) {
			if ((i % 4) == 3) continue;   // keep alpha opaque
			const int ad = std::abs(int(actual[i]) - int(golden[i]));
			diff[i] = static_cast<unsigned char>(
				std::min(255.0f, float(ad) * result.diffGain));
		}

		fs::create_directories(diffPath.parent_path());
		stbi_write_png(diffPath.string().c_str(), int(width), int(height), 4,
		               diff.data(), int(width) * 4);
	}

	stbi_image_free(golden);
	return result;
}

// ---------------------------------------------------------------------------
// Contact sheet: every scenario's output tiled into one image, so the whole
// renderer's state is one glance rather than N file opens. Nearest-neighbour
// placement, no scaling -- cells are sized to the largest output and smaller
// ones sit top-left in their cell.
// ---------------------------------------------------------------------------
struct Tile {
	std::vector<unsigned char> pixels;
	uint32_t width = 0, height = 0;
};

void writeContactSheet(const std::vector<Tile>& tiles, const fs::path& path) {
	if (tiles.empty()) return;

	uint32_t cellW = 0, cellH = 0;
	for (const Tile& t : tiles) {
		cellW = std::max(cellW, t.width);
		cellH = std::max(cellH, t.height);
	}

	const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(double(tiles.size()))));
	const uint32_t rows = static_cast<uint32_t>((tiles.size() + cols - 1) / cols);

	const uint32_t pad = 8;
	const uint32_t sheetW = cols * cellW + (cols + 1) * pad;
	const uint32_t sheetH = rows * cellH + (rows + 1) * pad;

	// Mid grey background: both a black and a white render read clearly against
	// it, and a missing tile is obvious.
	std::vector<unsigned char> sheet(size_t(sheetW) * sheetH * 4, 48);
	for (size_t i = 3; i < sheet.size(); i += 4) sheet[i] = 255;

	for (size_t i = 0; i < tiles.size(); ++i) {
		const Tile& t = tiles[i];
		const uint32_t col = static_cast<uint32_t>(i % cols);
		const uint32_t row = static_cast<uint32_t>(i / cols);
		const uint32_t ox = pad + col * (cellW + pad);
		const uint32_t oy = pad + row * (cellH + pad);

		for (uint32_t y = 0; y < t.height; ++y) {
			unsigned char* dst = &sheet[(size_t(oy + y) * sheetW + ox) * 4];
			const unsigned char* src = &t.pixels[size_t(y) * t.width * 4];
			std::memcpy(dst, src, size_t(t.width) * 4);
		}
	}

	fs::create_directories(path.parent_path());
	stbi_write_png(path.string().c_str(), int(sheetW), int(sheetH), 4,
	               sheet.data(), int(sheetW) * 4);
}

// ---------------------------------------------------------------------------
// Catches the render target the RenderLayer produced this frame. RenderLayer
// already dispatches it; the harness only has to listen, which is why nothing
// in the engine needed a new accessor for this.
// ---------------------------------------------------------------------------
class FrameCatcherLayer : public X3::ILayer
{
public:
	void onEvent(std::shared_ptr<X3::IEvent> event) override {
		if (event->GetType() == X3::EventType::NEW_FRAME_RENDERED_EVENT)
			m_LastFrame = std::dynamic_pointer_cast<X3::NewFrameRenderedEvent>(event)->frame;
	}
	X3::VulkanImage* lastFrame() const { return m_LastFrame; }

private:
	X3::VulkanImage* m_LastFrame = nullptr;
};

} // namespace

namespace X3
{

class RenderTestApp : public Application
{
public:
	RenderTestApp()
		: Application(WindowProps{ "X3 RenderTest", 1280, 720,
		                           /*VSync*/ false, /*CustomTitlebar*/ false,
		                           /*Hidden*/ true }) {
		m_Catcher = std::make_shared<FrameCatcherLayer>();
		_LayerStack->PushLayer(m_Catcher);
	}

	/// Renders one scenario and returns the encoded RGBA8 image.
	bool render(const Scenario& scenario, const fs::path& repoRoot,
	            std::vector<unsigned char>& outPixels,
	            uint32_t& outWidth, uint32_t& outHeight,
	            std::vector<float>& outFloats) {
		const fs::path projectPath = repoRoot / scenario.project;
		if (!fs::exists(projectPath)) {
			LOG_ENGINE_CRITICAL("scenario '{}': project not found: {}",
				scenario.name, projectPath.string());
			return false;
		}

		if (!_ProjectManager->OpenProject(projectPath)) {
			LOG_ENGINE_CRITICAL("scenario '{}': failed to open project", scenario.name);
			return false;
		}

		RenderSettings settings;
		settings.shaderType    = scenario.shader;
		settings.resolution    = { scenario.width, scenario.height };
		settings.raysPerPixel  = scenario.raysPerPixel;
		settings.bouncesPerRay = scenario.bouncesPerRay;
		settings.accumulate    = scenario.accumulate;
		settings.debugMode     = scenario.debugMode;
		settings.vSync         = false;   // never wait on a refresh rate in a test
		_LayerStack->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(settings));

		// THE SCENE CAMERA, NOT THE EDITOR CAMERA. The editor camera starts at
		// the origin looking at empty sky, which is what made the first manual
		// screenshot of this fixture show nothing but clouds. The scene camera is
		// authored to frame the content, so it is the only reproducible choice.
		_LayerStack->dispatchEvent(std::make_shared<UpdateEditorCameraEvent>(
			false, glm::mat4(1.0f), 60.0f));

		VulkanContext* context = VulkanContext::Get();

		// A FIXED FRAME COUNT, not "until it looks converged". Reproducibility is
		// the whole point; a time- or convergence-based bound would make the
		// golden depend on how fast this machine is.
		for (uint32_t i = 0; i < scenario.frames; ++i) {
			_Window->pollEvents();

			const FrameContext* frame = context->beginFrame();
			if (!frame) {
				// Swapchain went out of date and was recreated. Retry the frame
				// rather than counting it, exactly as Application::run does.
				--i;
				continue;
			}
			_LayerStack->onUpdate();
			context->endFrame();
			context->present();
		}

		VulkanImage* image = m_Catcher->lastFrame();
		if (!image || !image->valid()) {
			LOG_ENGINE_CRITICAL("scenario '{}': no frame was produced", scenario.name);
			return false;
		}

		// Out of frame by construction: the loop above closed the last one.
		context->readbackImage(*image, outWidth, outHeight, outFloats);
		outPixels = encode(outFloats, outWidth, outHeight);
		return true;
	}

	void shutdown() { Shutdown(); }

private:
	std::shared_ptr<FrameCatcherLayer> m_Catcher;
};

// The entrypoint header's factory. Unused here -- this tool drives its own loop
// rather than Application::run -- but the symbol must exist to link.
Application* CreateApplication(const std::filesystem::path&) { return nullptr; }

}

namespace {

std::vector<Scenario> loadScenarios(const fs::path& path) {
	std::vector<Scenario> out;
	YAML::Node root = YAML::LoadFile(path.string());
	YAML::Node list = root["scenarios"];
	if (!list || !list.IsSequence()) {
		LOG_ENGINE_CRITICAL("{}: no 'scenarios' sequence", path.string());
		return out;
	}

	for (const auto& node : list) {
		Scenario s;
		s.name    = node["name"].as<std::string>();
		s.project = node["project"].as<std::string>();
		s.shader  = parseShader(node["shader"].as<std::string>("pathtracing"));
		if (node["width"])         s.width         = node["width"].as<uint32_t>();
		if (node["height"])        s.height        = node["height"].as<uint32_t>();
		if (node["raysPerPixel"])  s.raysPerPixel  = node["raysPerPixel"].as<int>();
		if (node["bouncesPerRay"]) s.bouncesPerRay = node["bouncesPerRay"].as<int>();
		if (node["accumulate"])    s.accumulate    = node["accumulate"].as<bool>();
		if (node["debugMode"])     s.debugMode     = node["debugMode"].as<int>();
		if (node["frames"])        s.frames        = node["frames"].as<uint32_t>();
		if (node["energyTest"])    s.energyTest    = node["energyTest"].as<bool>();
		out.push_back(std::move(s));
	}
	return out;
}

void usage() {
	std::printf(
		"X3RenderTest -- golden-image regression tests for the renderer\n"
		"\n"
		"  --scenarios <path>   scenario table   (default tests/scenarios.yaml)\n"
		"  --golden-dir <path>  golden images    (default tests/golden)\n"
		"  --out <path>         output directory (default build/render-test)\n"
		"  --filter <substr>    only run scenarios whose name contains <substr>\n"
		"  --update-goldens     overwrite goldens instead of comparing\n"
		"  --tolerance <rmse>   max RMSE to still pass (default 0.002)\n"
		"\n"
		"Requires a display: the window is created unmapped, not headless.\n");
}

} // namespace

int main(int argc, char** argv) {
	const fs::path exeDir = fs::weakly_canonical(fs::path(argv[0])).parent_path();
	// Log::Init is NOT called here: Application's constructor does it, and
	// spdlog throws on a duplicate logger name rather than ignoring it.
	X3::EngineCfg::Init(exeDir);

	const fs::path repoRoot = X3_SOURCE_DIR;
	fs::path scenariosPath = repoRoot / "tests" / "scenarios.yaml";
	fs::path goldenDir     = repoRoot / "tests" / "golden";
	fs::path outDir        = repoRoot / "build" / "render-test";
	std::string filter;
	bool updateGoldens = false;
	double tolerance = 0.002;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		auto next = [&](const char* what) -> std::string {
			if (i + 1 >= argc) { std::printf("%s needs a value\n", what); std::exit(2); }
			return argv[++i];
		};
		if      (arg == "--scenarios")      scenariosPath = next("--scenarios");
		else if (arg == "--golden-dir")     goldenDir     = next("--golden-dir");
		else if (arg == "--out")            outDir        = next("--out");
		else if (arg == "--filter")         filter        = next("--filter");
		else if (arg == "--tolerance")      tolerance     = std::stod(next("--tolerance"));
		else if (arg == "--update-goldens") updateGoldens = true;
		else if (arg == "--help" || arg == "-h") { usage(); return 0; }
		else { std::printf("unknown argument '%s'\n\n", arg.c_str()); usage(); return 2; }
	}

	if (!fs::exists(scenariosPath)) {
		std::printf("scenario table not found: %s\n", scenariosPath.string().c_str());
		return 2;
	}

	std::vector<Scenario> scenarios = loadScenarios(scenariosPath);
	if (!filter.empty()) {
		std::erase_if(scenarios, [&](const Scenario& s) {
			return s.name.find(filter) == std::string::npos;
		});
	}
	if (scenarios.empty()) {
		std::printf("no scenarios to run\n");
		return 2;
	}

	fs::create_directories(outDir);
	fs::create_directories(goldenDir);

	// WIPE STALE DIFFS BEFORE RUNNING. A diff image only means anything relative
	// to the run that produced it, and a passing scenario writes none -- so
	// without this, yesterday's failure sits in the output directory looking
	// exactly like today's, and the directory quietly lies about the current
	// state. A diff is mostly black wherever the two images agree, which makes a
	// stale one especially easy to misread as "the render is broken".
	std::error_code ec;
	fs::remove_all(outDir / "diff", ec);

	X3::RenderTestApp app;

	std::vector<Tile> tiles;
	int passed = 0, failed = 0, written = 0;
	std::printf("\n");

	for (const Scenario& scenario : scenarios) {
		std::vector<unsigned char> pixels;
		std::vector<float> floats;
		uint32_t width = 0, height = 0;

		if (!app.render(scenario, repoRoot, pixels, width, height, floats)) {
			std::printf("  \033[31mERROR\033[0m %-32s could not render\n", scenario.name.c_str());
			++failed;
			continue;
		}

		const fs::path outPath    = outDir / (scenario.name + ".png");
		const fs::path goldenPath = goldenDir / (scenario.name + ".png");
		const fs::path diffPath   = outDir / "diff" / (scenario.name + ".png");

		fs::create_directories(outPath.parent_path());
		stbi_write_png(outPath.string().c_str(), int(width), int(height), 4,
		               pixels.data(), int(width) * 4);
		tiles.push_back(Tile{ pixels, width, height });

		if (updateGoldens) {
			stbi_write_png(goldenPath.string().c_str(), int(width), int(height), 4,
			               pixels.data(), int(width) * 4);
			std::printf("  \033[36mWROTE\033[0m %-32s %ux%u\n",
				scenario.name.c_str(), width, height);
			++written;
			continue;
		}

		// The energy assertions run BEFORE the image comparison and can fail a
		// scenario on their own: a BSDF that creates energy is broken whether or
		// not it happens to match a golden that was recorded while it was broken.
		if (scenario.energyTest) {
			const EnergyReport e = checkEnergy(floats, width, height);
			std::printf("        energy: metal      max %.4f at rough %.3f cos %.3f   min %.4f  mean %.4f\n",
				e.maxMetal, e.maxMetalRoughness, e.maxMetalCosTheta, e.minMetal, e.meanMetal);
			std::printf("                dielectric max %.4f at rough %.3f cos %.3f\n",
				e.maxDielectric, e.maxDielRoughness, e.maxDielCosTheta);
			if (e.gainsEnergy) {
				std::printf("  \033[31mFAIL\033[0m  %-32s BSDF CREATES ENERGY (albedo > 1)\n",
					scenario.name.c_str());
				++failed;
				continue;
			}
		}

		const Comparison cmp = compareToGolden(pixels, width, height, goldenPath, diffPath);

		if (!cmp.goldenExisted) {
			// NOT a pass. A missing golden means this scenario is unverified, and
			// reporting it as green would make an empty tests/golden look like a
			// clean run.
			std::printf("  \033[33mNEW\033[0m   %-32s no golden -- run --update-goldens\n",
				scenario.name.c_str());
			++failed;
		} else if (cmp.sizeMismatch) {
			std::printf("  \033[31mFAIL\033[0m  %-32s size differs from golden\n",
				scenario.name.c_str());
			++failed;
		} else if (cmp.rmse > tolerance) {
			std::printf("  \033[31mFAIL\033[0m  %-32s rmse %.4f  max %d\n",
				scenario.name.c_str(), cmp.rmse, cmp.maxDelta);
			std::printf("        -> %s  (diff amplified %.0fx)\n",
				diffPath.string().c_str(), cmp.diffGain);
			++failed;
		} else {
			std::printf("  \033[32mPASS\033[0m  %-32s rmse %.4f\n",
				scenario.name.c_str(), cmp.rmse);
			++passed;
		}
	}

	const fs::path sheetPath = outDir / "contact.png";
	writeContactSheet(tiles, sheetPath);

	std::printf("\n  contact sheet: %s  (%zu scenarios)\n",
		sheetPath.string().c_str(), tiles.size());
	if (updateGoldens)
		std::printf("  %d golden%s written\n\n", written, written == 1 ? "" : "s");
	else
		std::printf("  %d passed, %d failed\n\n", passed, failed);

	app.shutdown();
	return failed == 0 ? 0 : 1;
}
