// ============================================================================
// X3AssetCook -- the Phase 9 mesh cooker.
//
// Turns a source model into a .x3mesh: the decoded geometry plus the SAH BVH
// built over it, in the exact in-memory layout the GPU buffers already use. The
// point is that the BVH build -- which dominates mesh import and is currently
// paid on every single project open -- becomes a file read instead.
//
// IT DRIVES THE ENGINE'S OWN IMPORTER. AssetManager::ImportAsset does the
// assimp parse, the node-graph walk, the attribute build and the BVH build; this
// tool then slices the resulting AssetPool and serializes it. Nothing here
// re-implements any part of the import, so a cooked mesh cannot drift away from
// what the editor loads -- which is the failure mode a separate cooker
// invariably develops, and it shows up as geometry that is subtly different
// only in shipped builds.
//
//   X3AssetCook <input-model> <output.x3mesh> [--verify]
//   X3AssetCook --inspect <file.x3mesh>
//   X3AssetCook --self-test [--out <dir>]
//
// --verify is the round-trip gate the plan asks for: after writing, the file is
// read back and every array compared BYTE FOR BYTE against what went in. A
// serializer that is wrong in a way a value comparison forgives (a float that
// round trips to a different NaN payload, a -0.0 that became +0.0) is still
// wrong, because the buffer is uploaded to the GPU verbatim.
//
// --self-test runs that same round trip over SYNTHETIC data plus the negative
// cases (corrupted payload, truncation, bad magic, wrong version). It needs no
// model file, so it works on a checkout without SampleModels -- the same reason
// the primitive fixtures exist in X3FixtureGen. It is the check to wire into a
// gate; --verify on a real model is the check to run when cooking one.
//
// Exit code: 0 success, 1 failure, 2 usage error.
//
// WHAT THIS TOOL IS NOT. It cooks ONE mesh to ONE file. It does not compress
// textures (BC7), does not run meshoptimizer, does not write a binary scene, and
// nothing in the engine reads its output yet -- those are the rest of Phase 9.
// A cook step half-wired into the load path is worse than none, because then
// every asset bug has two possible sources.
// ============================================================================

#include "Core/Log.h"
#include "Core/GUID.h"
#include "Project/Assets/AssetCook.h"
#include "Project/Assets/AssetManager.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace fs = std::filesystem;
using namespace X3;

namespace {

	int g_Checks   = 0;
	int g_Failures = 0;

	void Check(bool ok, const std::string& what) {
		++g_Checks;
		if (ok) {
			std::printf("  \033[32mok\033[0m    %s\n", what.c_str());
		} else {
			++g_Failures;
			std::printf("  \033[31mFAIL\033[0m  %s\n", what.c_str());
		}
	}

	void PrintUsage() {
		std::printf(
			"X3AssetCook -- cooks a source model into a .x3mesh (geometry + prebuilt BVH)\n"
			"\n"
			"  X3AssetCook <input-model> <output%s> [--verify]\n"
			"  X3AssetCook --inspect <file%s>\n"
			"  X3AssetCook --self-test [--out <dir>]\n"
			"\n"
			"  --verify     read the file back and compare every array byte for byte\n"
			"  --inspect    validate and summarise an existing cooked file\n"
			"  --self-test  round-trip synthetic data plus the corruption cases\n"
			"\n"
			"Supported inputs are whatever the engine importer takes: .fbx .obj .gltf .glb\n"
			"Format: magic 'X3MESH', version %u\n",
			COOKED_MESH_FILE_EXTENSION, COOKED_MESH_FILE_EXTENSION, COOKED_MESH_VERSION);
	}

	void PrintSummary(const CookedMesh& mesh) {
		std::printf("    vertices     %zu\n", mesh.vertices.size());
		std::printf("    triangles    %zu\n", mesh.triPositions.size());
		std::printf("    bvh nodes    %zu\n", mesh.nodes.size());
		std::printf("    prim indices %zu\n", mesh.primIndex.size());
		std::printf("    submeshes    %zu\n", mesh.submeshes.size());
		for (const SubmeshInfo& s : mesh.submeshes) {
			std::printf("      [%u,+%u) slot %u  '%s'\n",
				s.firstTriIdx, s.triCount, s.materialSlot, s.name.c_str());
		}
		std::printf("    materials    %zu\n", mesh.materials.size());
	}


	// ------------------------------------------------------------------------
	// COOK
	// ------------------------------------------------------------------------
	int Cook(const fs::path& input, const fs::path& output, bool verify) {
		if (!fs::exists(input)) {
			std::printf("input does not exist: %s\n", input.string().c_str());
			return 1;
		}
		if (output.extension() != COOKED_MESH_FILE_EXTENSION) {
			// A warning rather than an error: the extension is a convention, and
			// refusing to write somewhere the caller asked for would be worse
			// than telling them the convention exists.
			std::printf("note: output '%s' does not use the %s extension\n",
				output.string().c_str(), COOKED_MESH_FILE_EXTENSION);
		}

		// THE EXISTING DECODE PATH, unmodified. AssetManager::DecodeMesh is
		// private -- deliberately, it is half of the decode/merge split that
		// keeps the AssetPool single-writer -- so this goes through the public
		// ImportAsset and reads the pool it filled. That costs the merge (an
		// append into empty buffers) and nothing else.
		AssetManager assetManager;
		const LR_GUID guid = assetManager.ImportAsset(input);
		if (guid == LR_GUID::INVALID) {
			std::printf("import failed: %s\n", input.string().c_str());
			return 1;
		}

		const std::shared_ptr<const AssetPool> pool = assetManager.GetAssetPool();
		if (!pool) {
			std::printf("import produced no asset pool\n");
			return 1;
		}

		std::optional<CookedMesh> mesh = ExtractCookedMesh(*pool, guid);
		if (!mesh) {
			std::printf("could not extract a mesh for GUID %llu (is the input a model?)\n",
				(unsigned long long)guid);
			return 1;
		}

		if (!WriteCookedMesh(output, *mesh)) {
			std::printf("write failed: %s\n", output.string().c_str());
			return 1;
		}

		std::error_code ec;
		const uintmax_t cookedBytes = fs::file_size(output, ec);
		const uintmax_t sourceBytes = fs::file_size(input, ec);
		std::printf("cooked %s -> %s\n", input.string().c_str(), output.string().c_str());
		PrintSummary(*mesh);
		std::printf("    source %ju bytes, cooked %ju bytes\n", sourceBytes, cookedBytes);

		if (!verify) return 0;

		std::printf("verifying round trip\n");
		const std::optional<CookedMesh> readBack = ReadCookedMesh(output);
		if (!readBack) {
			std::printf("  \033[31mFAIL\033[0m  the file just written could not be read back\n");
			return 1;
		}
		std::string differs;
		if (!CookedMeshBytesEqual(*mesh, *readBack, &differs)) {
			std::printf("  \033[31mFAIL\033[0m  round trip differs in: %s\n", differs.c_str());
			return 1;
		}
		std::printf("  \033[32mok\033[0m    every array is byte-identical\n");
		return 0;
	}


	// ------------------------------------------------------------------------
	// INSPECT
	//
	// Weaker than --verify by construction: with no in-memory original there is
	// nothing to compare against, so this proves the file is self-consistent
	// (magic, version, layout fingerprint, section extents, payload hash) and
	// nothing about whether it matches the model it came from. Say so rather
	// than letting a green line imply more than it means.
	// ------------------------------------------------------------------------
	int Inspect(const fs::path& file) {
		const std::optional<CookedMesh> mesh = ReadCookedMesh(file);
		if (!mesh) {
			std::printf("%s: rejected (see the log line above for which check failed)\n",
				file.string().c_str());
			return 1;
		}
		std::printf("%s: valid\n", file.string().c_str());
		PrintSummary(*mesh);
		return 0;
	}


	// ------------------------------------------------------------------------
	// SELF TEST
	// ------------------------------------------------------------------------

	// Synthetic mesh built to be HOSTILE to a serializer, not to be plausible
	// geometry. Every value here exists to break something specific:
	//   - NaN, -0.0, +/-inf and a denormal, because a value comparison passes
	//     them wrongly in both directions;
	//   - counts that are not multiples of the 16-byte section alignment, so the
	//     padding between sections is actually exercised;
	//   - a submesh name that is empty, one with an embedded NUL, and one with
	//     multi-byte UTF-8, because a NUL-terminated string table silently
	//     truncates two of those three;
	//   - a material with every extended lobe set, so a field this format
	//     forgets to carry fails the comparison.
	CookedMesh MakeSyntheticMesh() {
		CookedMesh m;

		const float nan  = std::numeric_limits<float>::quiet_NaN();
		const float inf  = std::numeric_limits<float>::infinity();
		const float tiny = std::numeric_limits<float>::denorm_min();

		constexpr uint32_t kVerts = 37;   // not a multiple of anything convenient
		constexpr uint32_t kTris  = 11;

		m.vertices.reserve(kVerts);
		for (uint32_t i = 0; i < kVerts; ++i) {
			const float f = static_cast<float>(i);
			Gpu::Vertex v{};
			v.positionU = { f * 0.5f, -f, 1.0f / (f + 1.0f), f * 0.01f };
			v.normalV   = { 0.0f, 1.0f, 0.0f, -f * 0.01f };
			v.tangent   = { 1.0f, 0.0f, 0.0f, (i % 2) ? 1.0f : -1.0f };
			m.vertices.push_back(v);
		}
		// The pathological lanes, placed in the middle so a writer that only
		// gets the first and last element right still fails.
		m.vertices[5].positionU  = { nan, -0.0f, inf, -inf };
		m.vertices[6].normalV    = { tiny, -tiny, 0.0f, nan };
		m.vertices[7].tangent    = { 0.0f, 0.0f, 0.0f, 0.0f };  // tangent.w == 0: the no-tangent sentinel

		m.triRefs.reserve(kTris);
		m.triPositions.reserve(kTris);
		for (uint32_t t = 0; t < kTris; ++t) {
			const uint32_t i0 = (t * 3 + 0) % kVerts;
			const uint32_t i1 = (t * 3 + 1) % kVerts;
			const uint32_t i2 = (t * 3 + 2) % kVerts;
			m.triRefs.push_back(Gpu::TriRef{ i0, i1, i2, t % 3 });
			m.triPositions.push_back(Gpu::TrianglePositions{
				m.vertices[i0].positionU * glm::vec4(1, 1, 1, 0),
				m.vertices[i1].positionU * glm::vec4(1, 1, 1, 0),
				m.vertices[i2].positionU * glm::vec4(1, 1, 1, 0) });
		}

		// A shape a real build could produce: interior nodes with triCount 0,
		// leaves with a first-primitive index.
		m.nodes = {
			BVHAccel::Node{ { -1.0f, -1.0f, -1.0f }, 1u, {  1.0f, 1.0f, 1.0f }, 0u },
			BVHAccel::Node{ { -1.0f, -1.0f, -1.0f }, 0u, {  0.0f, 1.0f, 1.0f }, 5u },
			BVHAccel::Node{ {  0.0f, -1.0f, -1.0f }, 5u, {  1.0f, 1.0f, 1.0f }, 6u },
		};

		// A permutation, not the identity -- an identity permutation is exactly
		// what a writer that dropped this array would appear to produce.
		m.primIndex = { 4, 0, 9, 2, 7, 1, 10, 3, 8, 5, 6 };

		m.submeshes = {
			SubmeshInfo{ 0, 5, 0, "" },
			SubmeshInfo{ 5, 3, 1, std::string("with\0embedded nul", 17) },
			SubmeshInfo{ 8, 3, 2, "submesh \xC3\xA9\xC3\xBC\xE4\xB8\xAD" },  // "submesh éü中" as UTF-8
		};

		MaterialDesc plain;
		MaterialDesc loaded;
		loaded.emission       = { 1.0f, 0.5f, 0.25f, 3.0f };
		loaded.color          = { 0.1f, 0.2f, 0.3f, 0.9f };
		loaded.metallic       = 1.0f;
		loaded.roughness      = 0.13f;
		loaded.ao             = 0.77f;
		loaded.normalScale    = 2.5f;
		loaded.baseColorTex   = LR_GUID(0x0123456789ABCDEFull);
		loaded.normalTex      = LR_GUID(0xFEDCBA9876543210ull);
		loaded.metalRoughTex  = LR_GUID::INVALID;
		loaded.emissiveTex    = LR_GUID(1ull);
		loaded.specularLevel  = 0.8f;
		loaded.clearcoat      = 0.6f;
		loaded.clearcoatRough = 0.05f;
		loaded.sheenRoughness = 0.42f;
		loaded.sheenColor     = { 0.9f, 0.1f, 0.05f };
		loaded.anisotropy     = -0.33f;
		m.materials = { plain, loaded, plain };

		return m;
	}

	// Overwrite one byte in the middle of a file, or truncate it, to prove the
	// reader's guards actually reject. A guard that has never been seen to fire
	// is a guard nobody knows is inverted.
	bool PokeByte(const fs::path& path, uint64_t offset, unsigned char value) {
		std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
		if (!f.is_open()) return false;
		f.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
		f.write(reinterpret_cast<const char*>(&value), 1);
		return f.good();
	}

	bool CopyFile(const fs::path& from, const fs::path& to) {
		std::error_code ec;
		fs::remove(to, ec);
		fs::copy_file(from, to, ec);
		return !ec;
	}

	std::vector<unsigned char> SlurpFile(const fs::path& path) {
		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in.is_open()) return {};
		const std::streamoff size = in.tellg();
		std::vector<unsigned char> bytes(static_cast<size_t>(size));
		in.seekg(0, std::ios::beg);
		if (size > 0) in.read(reinterpret_cast<char*>(bytes.data()), size);
		return bytes;
	}

	int SelfTest(const fs::path& dir) {
		std::error_code ec;
		fs::create_directories(dir, ec);
		std::printf("self-test in %s\n", dir.string().c_str());

		const fs::path good  = dir / ("synthetic" + std::string(COOKED_MESH_FILE_EXTENSION));
		const fs::path again = dir / ("synthetic-again" + std::string(COOKED_MESH_FILE_EXTENSION));
		const fs::path empty = dir / ("empty" + std::string(COOKED_MESH_FILE_EXTENSION));
		const fs::path broken = dir / ("broken" + std::string(COOKED_MESH_FILE_EXTENSION));

		// --- 1. the round trip ------------------------------------------------
		const CookedMesh original = MakeSyntheticMesh();
		Check(WriteCookedMesh(good, original), "write synthetic mesh");

		const std::optional<CookedMesh> readBack = ReadCookedMesh(good);
		Check(readBack.has_value(), "read it back");
		if (readBack) {
			std::string differs;
			const bool same = CookedMeshBytesEqual(original, *readBack, &differs);
			Check(same, same ? std::string("every array byte-identical")
			                 : std::string("arrays differ in: ") + differs);
		}

		// --- 2. determinism ---------------------------------------------------
		// Two cooks of identical input must produce identical bytes. This is what
		// makes a cooked file comparable by a build system and a checked-in
		// fixture diffable; it is also the cheapest possible detector for
		// uninitialised padding leaking into the output, which would otherwise
		// show up as a file that "changes" for no reason.
		Check(WriteCookedMesh(again, original), "write the same mesh a second time");
		Check(SlurpFile(good) == SlurpFile(again), "both writes are byte-identical");

		// --- 3. the empty mesh ------------------------------------------------
		// Zero-length sections, an empty string blob, no materials. Every
		// pointer-and-length path in the writer and reader gets a null and a
		// zero here, which is where memcpy and memcmp are undefined.
		const CookedMesh emptyMesh;
		Check(WriteCookedMesh(empty, emptyMesh), "write an empty mesh");
		const std::optional<CookedMesh> emptyBack = ReadCookedMesh(empty);
		Check(emptyBack.has_value(), "read the empty mesh back");
		if (emptyBack) {
			Check(CookedMeshBytesEqual(emptyMesh, *emptyBack, nullptr), "the empty mesh round trips");
		}

		// --- 4. the guards ----------------------------------------------------
		const std::vector<unsigned char> goodBytes = SlurpFile(good);
		Check(goodBytes.size() > 512, "the cooked file is big enough to corrupt meaningfully");

		// Payload corruption -- one flipped byte deep in the vertex data. This is
		// the case the payload hash exists for and the ONLY one nothing else
		// would catch: the file is the right size, the right version and the
		// right shape, and the mesh is simply wrong.
		if (CopyFile(good, broken)) {
			Check(PokeByte(broken, goodBytes.size() / 2, 0xA5u), "flip a byte in the payload");
			Check(!ReadCookedMesh(broken).has_value(), "a corrupted payload is rejected");
		} else {
			Check(false, "copy the cooked file for corruption");
		}

		// Truncation -- the state a cook killed mid-write would leave if it wrote
		// in place instead of renaming into place.
		if (CopyFile(good, broken)) {
			fs::resize_file(broken, goodBytes.size() / 2, ec);
			Check(!ec, "truncate a copy");
			Check(!ReadCookedMesh(broken).has_value(), "a truncated file is rejected");
		}

		// Bad magic -- an entirely different file that happens to be pointed at.
		if (CopyFile(good, broken)) {
			Check(PokeByte(broken, 0, 'Z'), "clobber the magic");
			Check(!ReadCookedMesh(broken).has_value(), "a file with the wrong magic is rejected");
		}

		// Wrong version -- the byte at offset 8 is the version's low byte.
		if (CopyFile(good, broken)) {
			Check(PokeByte(broken, 8, static_cast<unsigned char>(COOKED_MESH_VERSION + 1)),
			      "bump the version field");
			Check(!ReadCookedMesh(broken).has_value(), "a future version is rejected");
		}

		// Wrong layout fingerprint -- sizeof(Gpu::Vertex) lives at offset 16 and
		// is what catches an old file meeting a rebuilt engine.
		if (CopyFile(good, broken)) {
			Check(PokeByte(broken, 16, 40u), "claim sizeof(Gpu::Vertex) == 40");
			Check(!ReadCookedMesh(broken).has_value(), "a stale struct layout is rejected");
		}

		std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
		return g_Failures == 0 ? 0 : 1;
	}

}


int main(int argc, char** argv) {
	Log::Init();

	std::vector<std::string> args(argv + 1, argv + argc);
	if (args.empty()) { PrintUsage(); return 2; }

	if (args[0] == "--help" || args[0] == "-h") { PrintUsage(); return 0; }

	if (args[0] == "--self-test") {
		fs::path dir = fs::temp_directory_path() / "x3-assetcook-selftest";
		if (args.size() >= 3 && args[1] == "--out") dir = args[2];
		return SelfTest(dir);
	}

	if (args[0] == "--inspect") {
		if (args.size() < 2) { PrintUsage(); return 2; }
		return Inspect(args[1]);
	}

	if (args.size() < 2 || args[0].starts_with("--")) { PrintUsage(); return 2; }

	bool verify = false;
	for (size_t i = 2; i < args.size(); ++i) {
		if (args[i] == "--verify") { verify = true; continue; }
		std::printf("unknown argument: %s\n", args[i].c_str());
		PrintUsage();
		return 2;
	}

	return Cook(args[0], args[1], verify);
}
