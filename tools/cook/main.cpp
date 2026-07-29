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
// cases (corrupted payload, truncation, bad magic, wrong version), the staleness
// key, and a round trip THROUGH THE ENGINE'S OWN LOAD PATH. It needs no model
// file, so it works on a checkout without SampleModels -- the same reason the
// primitive fixtures exist in X3FixtureGen. It is the check to wire into a gate;
// --verify on a real model is the check to run when cooking one.
//
// Exit code: 0 success, 1 failure, 2 usage error.
//
// THE ENGINE READS THESE FILES NOW. When this tool was written nothing did, and
// the paragraph here said as much. AssetManager::DecodeCookedMesh closes that
// gap, which changes what this tool owes its output in two concrete ways:
//   - it writes a SOURCE STAMP (size + mtime of the input) so the editor's
//     "load cooked" mode can tell a fresh cooked file from a stale one, and
//   - the naming convention matters: the editor looks for "<source>.x3mesh"
//     beside the source, e.g. "Bistro.glb" -> "Bistro.glb.x3mesh". Cook
//     somewhere else and the file is still valid, it is just never found.
//
// WHAT THIS TOOL IS STILL NOT. It cooks ONE mesh to ONE file. It does not
// compress textures (BC7), does not run meshoptimizer and does not write a
// binary scene -- those are the rest of Phase 9. A cooked mesh's materials
// therefore still reference texture assets the file does not contain.
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
			"Format: magic 'X3MESH', version %u\n"
			"\n"
			"The editor's load-cooked mode (X3_LOAD_COOKED=1) looks for a cooked file named\n"
			"'<source>%s' NEXT TO the source -- 'Bistro.glb' -> 'Bistro.glb%s' -- and uses it\n"
			"only when the source stamp written here still matches the source on disk.\n",
			COOKED_MESH_FILE_EXTENSION, COOKED_MESH_FILE_EXTENSION, COOKED_MESH_VERSION,
			COOKED_MESH_FILE_EXTENSION, COOKED_MESH_FILE_EXTENSION);
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

		// A COOKER MUST NEVER READ ITS OWN OUTPUT. ImportAsset now honours
		// X3_LOAD_COOKED, and a developer debugging the cooked path naturally has
		// it exported in their shell -- at which point cooking a model would load
		// the previous .x3mesh sitting next to it and re-serialize that instead of
		// re-importing the source. Mostly that is a no-op, which is what makes it
		// dangerous: "delete the cooked file and re-cook" would stop being a way
		// to recover from a bad cook, because the bad cook would reproduce itself.
		// The override is explicit and per-process, so it cannot be forgotten.
		AssetManager::SetLoadCookedMode(false);

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

		// SANITY BEFORE SERIALIZATION. The engine refuses a cooked file whose
		// indices do not line up with its arrays, so a file that fails this check
		// is one nothing will ever load. Catching it here names the cook that
		// produced it; catching it at load time names only the file.
		std::string whyNot;
		if (!CookedMeshIsSelfConsistent(*mesh, &whyNot)) {
			std::printf("refusing to cook an inconsistent mesh: %s\n", whyNot.c_str());
			return 1;
		}

		// THE STALENESS KEY, taken from the input AFTER the import has read it,
		// so a source edited mid-cook stamps as the newer file and the next run
		// re-cooks. Stamping first would record the pre-edit size and mtime for
		// post-edit geometry, which is a cooked file that claims to be fresh and
		// is not -- the one outcome the stamp exists to prevent.
		const std::optional<CookedSourceStamp> stamp = MakeCookedSourceStamp(input);
		if (!stamp) {
			// Not fatal: the file is still loadable, it just can never win a
			// freshness check. Said out loud because the symptom otherwise is
			// "load-cooked mode does nothing" with no error anywhere.
			std::printf("warning: could not stamp %s -- the editor will always treat this cooked "
			            "file as stale\n", input.string().c_str());
		}

		if (!WriteCookedMesh(output, *mesh, stamp ? &*stamp : nullptr)) {
			std::printf("write failed: %s\n", output.string().c_str());
			return 1;
		}

		std::error_code ec;
		const uintmax_t cookedBytes = fs::file_size(output, ec);
		const uintmax_t sourceBytes = fs::file_size(input, ec);
		std::printf("cooked %s -> %s\n", input.string().c_str(), output.string().c_str());
		PrintSummary(*mesh);
		std::printf("    source %ju bytes, cooked %ju bytes\n", sourceBytes, cookedBytes);

		// The editor finds a cooked file by NAME. Cooking to some other path is
		// legitimate (an export tree, a scratch copy) but silently means
		// load-cooked mode will never pick it up, so say so rather than leaving
		// someone to wonder why the mode appears to do nothing.
		const fs::path expected = AssetManager::CookedSiblingPath(input);
		if (fs::weakly_canonical(output, ec) != fs::weakly_canonical(expected, ec)) {
			std::printf("note: the editor looks for '%s'; this file is elsewhere and load-cooked "
			            "mode will not find it\n", expected.string().c_str());
		}

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

		// The stamp is only useful if a file just cooked from a source reads back
		// as fresh against that source. Checked here, on a REAL model, because
		// the self-test's synthetic sources cannot exercise a filesystem's actual
		// timestamp granularity -- which is the one thing likely to differ
		// between a developer's machine and a network share on CI.
		std::string why;
		const CookedFreshness freshness = CheckCookedMeshFreshness(output, input, &why);
		if (freshness != CookedFreshness::Fresh) {
			std::printf("  \033[31mFAIL\033[0m  the file just cooked does not read as fresh against "
			            "its source (%s: %s)\n", CookedFreshnessToString(freshness), why.c_str());
			return 1;
		}
		std::printf("  \033[32mok\033[0m    the source stamp matches %s\n", input.filename().string().c_str());
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

		// The framing was fine or ReadCookedMesh would have said so. Whether the
		// mesh MEANS anything is a separate question, and it is the one that
		// decides whether the engine will accept this file -- so --inspect has to
		// answer it too, or a file it calls "valid" can still be refused at load.
		std::string whyNot;
		if (!CookedMeshIsSelfConsistent(*mesh, &whyNot)) {
			std::printf("%s: intact but NOT LOADABLE -- %s\n", file.string().c_str(), whyNot.c_str());
			PrintSummary(*mesh);
			return 1;
		}

		std::printf("%s: valid\n", file.string().c_str());
		PrintSummary(*mesh);

		const std::optional<CookedSourceStamp> stamp = ReadCookedSourceStamp(file);
		if (stamp) {
			std::printf("    source stamp '%s', %ju bytes, mtime %lld ns\n",
				stamp->sourceName.c_str(), (uintmax_t)stamp->sourceSize,
				(long long)stamp->sourceMTimeNs);
		} else {
			std::printf("    source stamp <none> -- load-cooked mode will never use this file\n");
		}
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

	// A stand-in for a source model. The staleness key never looks INSIDE the
	// source -- it is size and mtime -- so a text file exercises it exactly as a
	// 400 MB .glb would, and the self-test keeps its promise of needing no
	// assets. (Nothing here ever asks the importer to parse it; the tests below
	// call the freshness API directly for that reason.)
	bool WriteFakeSource(const fs::path& path, const std::string& contents) {
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open()) return false;
		out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		out.flush();
		return out.good();
	}

	// Move a file's mtime by `delta`. Used to build the two cases that a naive
	// "is the cooked file newer" test cannot tell apart.
	bool ShiftMTime(const fs::path& path, std::chrono::seconds delta) {
		std::error_code ec;
		const fs::file_time_type now = fs::last_write_time(path, ec);
		if (ec) return false;
		fs::last_write_time(path, now + delta, ec);
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

		// --- 5. semantic validation -------------------------------------------
		// Everything above proves the file arrived intact. These prove that an
		// intact file describing an impossible mesh is still refused -- the
		// checks that stand between an untrusted .x3mesh and a GPU buffer.
		Check(CookedMeshIsSelfConsistent(original, nullptr), "the synthetic mesh is self-consistent");
		Check(CookedMeshIsSelfConsistent(emptyMesh, nullptr), "the empty mesh is self-consistent");
		{
			CookedMesh bad = original;
			bad.triRefs[3].i0 = static_cast<uint32_t>(bad.vertices.size());   // one past the end
			Check(!CookedMeshIsSelfConsistent(bad, nullptr), "a triangle indexing past the vertices is refused");
		}
		{
			CookedMesh bad = original;
			bad.triRefs[3].materialSlot = static_cast<uint32_t>(bad.materials.size());
			Check(!CookedMeshIsSelfConsistent(bad, nullptr), "a triangle in a nonexistent material slot is refused");
		}
		{
			CookedMesh bad = original;
			bad.primIndex[2] = static_cast<uint32_t>(bad.triPositions.size());
			Check(!CookedMeshIsSelfConsistent(bad, nullptr), "a BVH permutation entry past the triangles is refused");
		}
		{
			// The one that hangs a GPU rather than corrupting a pixel: an
			// interior node whose child is itself, which the traversal shader
			// follows forever with no bound of its own.
			CookedMesh bad = original;
			bad.nodes[0].triCount = 0u;
			bad.nodes[0].leftChild_Or_FirstTri = 0u;
			Check(!CookedMeshIsSelfConsistent(bad, nullptr), "a BVH node that is its own child is refused");
		}
		{
			CookedMesh bad = original;
			bad.nodes[2].leftChild_Or_FirstTri = static_cast<uint32_t>(bad.triPositions.size());
			Check(!CookedMeshIsSelfConsistent(bad, nullptr), "a BVH leaf running off the triangle list is refused");
		}
		{
			CookedMesh bad = original;
			bad.submeshes[2].triCount += 1;   // [8, +4) of 11
			Check(!CookedMeshIsSelfConsistent(bad, nullptr), "a submesh range past the triangles is refused");
		}

		// --- 6. the staleness key ---------------------------------------------
		// The source stamp round trips, and a cooked file that cannot prove it
		// matches its source is refused. Every case below is one a real editor
		// hits: an edited source, a restored older revision, a cooked file from
		// before stamps existed, a cooked file for a different model.
		const fs::path source      = dir / "fake-source.glb";
		const fs::path sourceTwin  = dir / "other-source.glb";
		const fs::path cookedForSource = fs::path(source).concat(COOKED_MESH_FILE_EXTENSION);
		const fs::path noStampFile = dir / ("nostamp" + std::string(COOKED_MESH_FILE_EXTENSION));

		// THE ORDER OF THESE IS LOAD-BEARING and every step says which state it
		// leaves behind. Each case has to fail for the reason it names, and the
		// cheap "is the cooked file newer" pre-check runs first -- so a test that
		// carelessly leaves the source in the future would make every later case
		// report OlderThanSource while still looking green.
		Check(WriteFakeSource(source, "pretend this is a glb"), "create a stand-in source file");
		// The twin is created NOW, with the same contents and the same timestamp,
		// so that later it differs from the real source in its NAME ALONE.
		Check(WriteFakeSource(sourceTwin, "pretend this is a glb"), "create an identical second source");
		{
			std::error_code twinEc;
			const fs::file_time_type sourceTime = fs::last_write_time(source, twinEc);
			fs::last_write_time(sourceTwin, sourceTime, twinEc);
			Check(!twinEc, "give the second source the first's exact timestamp");
		}

		Check(cookedForSource == AssetManager::CookedSiblingPath(source),
		      "the cooked sibling of 'fake-source.glb' is 'fake-source.glb" +
		      std::string(COOKED_MESH_FILE_EXTENSION) + "'");

		const std::optional<CookedSourceStamp> stamp = MakeCookedSourceStamp(source);
		Check(stamp.has_value(), "stamp the source");
		if (stamp) {
			Check(WriteCookedMesh(cookedForSource, original, &*stamp), "cook it with the stamp");

			const std::optional<CookedSourceStamp> stampBack = ReadCookedSourceStamp(cookedForSource);
			Check(stampBack.has_value(), "read the stamp back");
			if (stampBack) {
				Check(stampBack->sourceSize == stamp->sourceSize &&
				      stampBack->sourceMTimeNs == stamp->sourceMTimeNs &&
				      stampBack->sourceName == stamp->sourceName,
				      "the stamp round trips exactly");
				Check(stampBack->sourceName == "fake-source.glb", "the stamp names the source file");
			}

			// The whole point: a cooked file carrying a stamp that still matches.
			Check(CheckCookedMeshFreshness(cookedForSource, source, nullptr) == CookedFreshness::Fresh,
			      "a freshly cooked file is fresh");

			// A stamped file still round trips as a MESH -- adding a section must
			// not have disturbed the eight that were already there.
			const std::optional<CookedMesh> stampedBack = ReadCookedMesh(cookedForSource);
			Check(stampedBack.has_value(), "a stamped file still reads as a mesh");
			if (stampedBack) {
				Check(CookedMeshBytesEqual(original, *stampedBack, nullptr),
				      "adding the stamp section left every other section byte-identical");
			}

			// NAME-ONLY MISMATCH, and the reason the stamp records a filename at
			// all. Identical size, identical mtime, different model: one cooked
			// file copied over another's name, which a size+time comparison
			// cannot see and which serves entirely the wrong geometry.
			{
				const fs::path cookedForTwin = fs::path(sourceTwin).concat(COOKED_MESH_FILE_EXTENSION);
				Check(CopyFile(cookedForSource, cookedForTwin),
				      "copy the first model's cooked file onto the second's name");
				// Set explicitly rather than relying on whatever copy_file did to
				// the timestamp -- POSIX and Windows disagree about that, and the
				// pre-check must not be what fails here.
				Check(ShiftMTime(cookedForTwin, std::chrono::seconds(60)),
				      "make it comfortably newer than the second source");
				std::string why;
				const CookedFreshness f = CheckCookedMeshFreshness(cookedForTwin, sourceTwin, &why);
				Check(f == CookedFreshness::Stale,
				      std::string("a cooked file naming a different source is refused (got: ") +
				      CookedFreshnessToString(f) + ")");
			}

			// THE CASE THE NAIVE CHECK MISSES, and the reason the stamp exists at
			// all. The source is REPLACED BY AN OLDER REVISION -- a git checkout
			// of a previous commit, an undo, a restore from backup. The cooked
			// file is still the newer of the two, so "is the cache newer?" says
			// yes and serves geometry the source no longer describes. The stamp
			// sees that the mtime moved AT ALL and refuses.
			// Leaves: source an hour behind the cook.
			Check(ShiftMTime(source, std::chrono::seconds(-3600)), "roll the source back an hour");
			{
				std::string why;
				const CookedFreshness f = CheckCookedMeshFreshness(cookedForSource, source, &why);
				Check(f == CookedFreshness::Stale,
				      std::string("an older revision of the source makes the cook stale (got: ") +
				      CookedFreshnessToString(f) + ")");
			}

			// A source that changed SIZE, with its mtime pushed back behind the
			// cooked file's so the pre-check cannot be what catches it. Same
			// shape, different field.
			// Leaves: source an hour behind the cook, one byte longer.
			Check(WriteFakeSource(source, "pretend this is a glb, but longer"), "grow the source");
			Check(ShiftMTime(source, std::chrono::seconds(-3600)), "keep it older than the cook");
			{
				std::string why;
				const CookedFreshness f = CheckCookedMeshFreshness(cookedForSource, source, &why);
				Check(f == CookedFreshness::Stale,
				      std::string("a resized source makes the cook stale (got: ") + CookedFreshnessToString(f) + ")");
			}
		}

		// Cooked without a stamp -- what every file produced before the stamp
		// section existed looks like. Written now, so it is unambiguously the
		// newer file: exactly the configuration in which an mtime-only check
		// would wave it through, and it must not be waved through.
		Check(WriteCookedMesh(noStampFile, original, nullptr), "cook a file with no stamp");
		{
			std::string why;
			const CookedFreshness f = CheckCookedMeshFreshness(noStampFile, source, &why);
			Check(f == CookedFreshness::NoStamp,
			      std::string("a cooked file with no stamp is not usable as a cache (got: ") +
			      CookedFreshnessToString(f) + ")");
		}
		Check(CheckCookedMeshFreshness(dir / "does-not-exist.x3mesh", source, nullptr) ==
		      CookedFreshness::NoCookedFile, "a missing cooked file reports as missing, not as an error");
		Check(ReadCookedMesh(noStampFile).has_value(),
		      "...but it is still a perfectly loadable mesh");

		// The ordinary case, LAST because it leaves the source in the future:
		// edited after the cook, so the cheap pre-check refuses it before the
		// file is even opened.
		Check(ShiftMTime(source, std::chrono::seconds(7200)), "edit the source after the cook");
		Check(CheckCookedMeshFreshness(cookedForSource, source, nullptr) == CookedFreshness::OlderThanSource,
		      "a cooked file older than its source is refused");

		// --- 7. through the engine's load path --------------------------------
		// The check the rest of this file cannot make. Everything above compares
		// the serializer against itself; this cooks synthetic data, loads it back
		// through AssetManager -- the SAME LoadMesh the editor calls -- and pulls
		// the arrays out of the AssetPool to compare byte for byte with what went
		// in. A format that round trips perfectly and merges wrongly passes every
		// test above and this one fails.
		{
			AssetManager assetManager;
			const std::shared_ptr<const AssetPool> pool = assetManager.GetAssetPool();
			Check(pool != nullptr, "the asset manager has a pool");

			// FIRST, INTO AN EMPTY POOL. Offsets are all zero here, which is the
			// easy case -- and, per ExtractCookedMesh's own warning, the case a
			// round-trip test accidentally restricts itself to.
			const LR_GUID firstGuid = assetManager.ImportAsset(good);
			Check(firstGuid != LR_GUID::INVALID, "the engine imports a .x3mesh directly");

			// SECOND, INTO A POOL THAT IS NOT EMPTY. This is the one that matters.
			// The file stores MESH-LOCAL vertex indices because ExtractCookedMesh
			// subtracts firstVertexIdx; MergeMesh adds the new base back. Loading
			// only ever at offset zero would make a loader that forgot the rebase
			// look correct forever.
			const LR_GUID secondGuid = assetManager.ImportAsset(good);
			Check(secondGuid != LR_GUID::INVALID, "the engine imports it a second time");

			if (pool && firstGuid != LR_GUID::INVALID && secondGuid != LR_GUID::INVALID) {
				const std::shared_ptr<MeshMetadata> second = pool->find<MeshMetadata>(secondGuid);
				Check(second != nullptr, "the second mesh is in the pool");
				if (second) {
					Check(second->firstVertexIdx == original.vertices.size() &&
					      second->firstTriIdx == original.triPositions.size() &&
					      second->firstNodeIdx == original.nodes.size(),
					      "the second mesh landed at a non-zero offset, so the rebase is exercised");
					Check(second->nodeCount == original.nodes.size(),
					      "nodeCount came back from the file, not from a rebuild");
					Check(second->materialSlotCount == original.materials.size(),
					      "the material slot count survived the load");
				}

				// ExtractCookedMesh is the exact inverse of the merge, so what
				// comes back out must be what went in -- for BOTH meshes, at both
				// offsets.
				const std::optional<CookedMesh> firstBack  = ExtractCookedMesh(*pool, firstGuid);
				const std::optional<CookedMesh> secondBack = ExtractCookedMesh(*pool, secondGuid);
				Check(firstBack.has_value() && secondBack.has_value(), "both meshes extract from the pool");

				if (firstBack) {
					std::string differs;
					const bool same = CookedMeshBytesEqual(original, *firstBack, &differs);
					Check(same, same ? std::string("mesh loaded at offset 0 is byte-identical")
					                 : std::string("mesh at offset 0 differs in: ") + differs);
				}
				if (secondBack) {
					std::string differs;
					const bool same = CookedMeshBytesEqual(original, *secondBack, &differs);
					Check(same, same ? std::string("mesh loaded at a non-zero offset is byte-identical")
					                 : std::string("mesh at a non-zero offset differs in: ") + differs);
				}
			}
		}

		// A file the loader must REFUSE rather than merge. Truncation rather than
		// a poked byte, because a truncated file is unambiguously damaged -- a
		// poke can land on a byte that already held that value. What is being
		// asserted here is not the guard (section 4 did that) but the LOAD PATH's
		// behaviour around it: a bad cooked file is a failed import, never a
		// crash and never a half-merged pool.
		if (CopyFile(good, broken)) {
			fs::resize_file(broken, goodBytes.size() / 3, ec);
			Check(!ec, "truncate a copy for the loader");
			AssetManager assetManager;
			Check(assetManager.ImportAsset(broken) == LR_GUID::INVALID,
			      "the engine refuses a corrupt .x3mesh instead of merging it");
			const std::shared_ptr<const AssetPool> pool = assetManager.GetAssetPool();
			Check(pool && pool->VertexBuffer.empty() && pool->TriPositionBuffer.empty(),
			      "and leaves the asset pool untouched");
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
