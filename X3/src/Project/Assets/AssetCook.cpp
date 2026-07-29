#include "Project/Assets/AssetCook.h"

#include <bit>
#include <cstring>
#include <limits>

namespace X3
{

	// =========================================================================
	// GUARD 1 -- the build-time half of the layout contract (see AssetCook.h).
	//
	// These repeat asserts that already exist in GpuTypes.h and BVHAccel.h. That
	// repetition is the point: they are not here to prove the structs are the
	// size they say they are, they are here so that CHANGING one of them fails
	// to compile IN THIS FILE. Whoever makes that change is then forced to
	// decide what happens to files cooked before it, which is a decision that is
	// otherwise very easy not to notice making.
	// =========================================================================
	static_assert(sizeof(Gpu::Vertex)            == 48, "cooked layout changed -- bump COOKED_MESH_VERSION");
	static_assert(sizeof(Gpu::TriRef)            == 16, "cooked layout changed -- bump COOKED_MESH_VERSION");
	static_assert(sizeof(Gpu::TrianglePositions) == 48, "cooked layout changed -- bump COOKED_MESH_VERSION");
	static_assert(sizeof(BVHAccel::Node)         == 32, "cooked layout changed -- bump COOKED_MESH_VERSION");
	static_assert(sizeof(uint32_t)               ==  4);

	// The arrays are written with memcpy, which is only defined for these.
	static_assert(std::is_trivially_copyable_v<Gpu::Vertex>);
	static_assert(std::is_trivially_copyable_v<Gpu::TriRef>);
	static_assert(std::is_trivially_copyable_v<Gpu::TrianglePositions>);
	static_assert(std::is_trivially_copyable_v<BVHAccel::Node>);

	// THE FORMAT IS LITTLE-ENDIAN AND IEEE-754, because it is a raw image of
	// host memory and no byte swapping is performed anywhere. A big-endian host
	// would cook files that this same code reads back perfectly and that every
	// other machine reads as noise -- the worst possible failure mode, since the
	// producing machine's own round-trip test would pass. Refuse to build there
	// instead. Every platform this engine targets (x86-64, arm64) is
	// little-endian; if that ever stops being true, byte swapping belongs in the
	// reader, keyed off a byte-order mark in the header.
	static_assert(std::endian::native == std::endian::little,
	              "the .x3mesh format is a raw little-endian image; port the reader before building here");
	static_assert(std::numeric_limits<float>::is_iec559,
	              "the .x3mesh format stores raw IEEE-754 floats");

	namespace {

		// -----------------------------------------------------------------
		// Sections are addressed by ID through a table, never by position.
		// Values are permanent: an id, once used, is never reused for
		// something else, because "reader skips unknown ids" is only safe if
		// an id it DOES know cannot have changed meaning.
		// -----------------------------------------------------------------
		enum SectionId : uint32_t {
			SECTION_VERTICES       = 1,
			SECTION_TRI_REFS       = 2,
			SECTION_TRI_POSITIONS  = 3,
			SECTION_BVH_NODES      = 4,
			SECTION_BVH_PRIM_INDEX = 5,
			SECTION_SUBMESHES      = 6,
			SECTION_STRINGS        = 7,
			SECTION_MATERIALS      = 8,
			// Added after version 1 shipped, WITHOUT a version bump, which is the
			// mechanism this format was designed around (see EXTENDING in the
			// header). Deliberately NOT in kRequiredSections: a file cooked before
			// this existed is still a valid file, it just cannot prove it is fresh.
			SECTION_SOURCE_STAMP   = 9,
		};

		// Every section payload starts at a multiple of this, with the gap
		// zero-filled.
		//
		// 16 because that is the alignment a glm::vec4 array wants. Nothing in
		// the current reader needs it -- it memcpys, which has no alignment
		// requirement at all -- but a future loader that mmaps a cooked file and
		// casts straight into it does, and retrofitting alignment onto a format
		// means a version bump for zero functional gain. The zero fill (rather
		// than whatever the allocator left) is what makes two cooks of identical
		// input produce identical bytes, so a build system can compare cooked
		// files and a `git diff` of a checked-in fixture is meaningful.
		constexpr uint64_t kSectionAlign = 16;

		// Sanity bound on the section table so a corrupt header cannot make the
		// reader try to allocate a table of four billion entries before any of
		// the real validation runs.
		constexpr uint32_t kMaxSections = 64;

		// -----------------------------------------------------------------
		// 64 B file header.
		//
		// The layoutXxx fields are GUARD 2: the recorded sizeof of every struct
		// dumped raw into this file, verified against the running binary's
		// sizeof on read. This is what turns "old file, new engine" from silent
		// garbage into a refusal with a message.
		// -----------------------------------------------------------------
		struct Header {
			char     magic[8];            //  0  "X3MESH\0\0"
			uint32_t version;             //  8
			uint32_t headerBytes;         // 12  == sizeof(Header); framing self-check
			uint32_t layoutVertex;        // 16  \.
			uint32_t layoutTriRef;        // 20   |
			uint32_t layoutTriPositions;  // 24   |  layout fingerprint
			uint32_t layoutBvhNode;       // 28   |
			uint32_t layoutSubmesh;       // 32   |
			uint32_t layoutMaterial;      // 36  /
			uint32_t sectionCount;        // 40
			uint32_t reserved0;           // 44  always 0
			uint64_t payloadHash;         // 48  FNV-1a 64 over section payloads
			uint64_t reserved1;           // 56  always 0
		};
		static_assert(sizeof(Header) == 64);
		static_assert(std::is_trivially_copyable_v<Header>);

		// 32 B. byteSize is redundant with elementSize * elementCount and is
		// stored anyway: the reader validates the two against each other, which
		// is a free consistency check on a header that may have been corrupted
		// in a way the payload hash cannot help with (the hash is only
		// verifiable once the offsets are already trusted enough to read).
		struct SectionEntry {
			uint32_t id;
			uint32_t elementSize;
			uint64_t elementCount;
			uint64_t byteOffset;    // from the start of the file
			uint64_t byteSize;
		};
		static_assert(sizeof(SectionEntry) == 32);
		static_assert(std::is_trivially_copyable_v<SectionEntry>);

		// 24 B. SubmeshInfo carries a std::string, which obviously cannot be
		// dumped raw, so the names live in one blob (SECTION_STRINGS) and the
		// record keeps a (offset, length) pair into it. Length-prefixed rather
		// than NUL-terminated because an aiMesh name is arbitrary bytes and a
		// NUL scan would silently truncate one containing an embedded zero;
		// fixed-size records are also what keep this section a plain array the
		// reader can bounds-check in one multiply.
		struct SubmeshRecord {
			uint32_t firstTriIdx;
			uint32_t triCount;
			uint32_t materialSlot;
			uint32_t nameOffset;   // into SECTION_STRINGS
			uint32_t nameLength;
			uint32_t reserved;     // always 0; keeps the record 8-byte aligned
		};
		static_assert(sizeof(SubmeshRecord) == 24);
		static_assert(std::is_trivially_copyable_v<SubmeshRecord>);

		// 24 B. The staleness key, in a section that holds either zero or one of
		// these -- zero meaning "cooked without a source", which is a different
		// statement from "cooked from a source of size 0 at time 0" and must not
		// be encoded the same way.
		//
		// The name lives in SECTION_STRINGS alongside the submesh names rather
		// than in a fixed char array here: a fixed array either truncates a long
		// filename (and two long filenames sharing a prefix then compare equal,
		// which defeats the check) or wastes 256 bytes in every file. Same
		// (offset, length) discipline as SubmeshRecord, and bounds-checked
		// against the same blob.
		struct SourceStampRecord {
			uint64_t sourceSize;
			int64_t  sourceMTimeNs;
			uint32_t nameOffset;    // into SECTION_STRINGS
			uint32_t nameLength;
		};
		static_assert(sizeof(SourceStampRecord) == 24);
		static_assert(std::is_trivially_copyable_v<SourceStampRecord>);

		// At most one stamp per file: a .x3mesh is one mesh cooked from one
		// source. A file claiming several is malformed, and picking one of them
		// would be picking arbitrarily which source this file is keyed to.
		constexpr uint64_t kMaxStampRecords = 1;

		// The source's mtime as nanoseconds since the SYSTEM CLOCK epoch.
		//
		// std::filesystem::file_time_type's epoch is implementation-defined --
		// MSVC counts from 1601, libstdc++ from 1970 -- so storing its raw ticks
		// would make every file cooked on CI look stale to an editor on the other
		// platform. That failure is silent: the cook cache simply never hits, and
		// the only symptom is that project opens stay slow. clock_cast is the
		// portable conversion; where the standard library does not offer it the
		// raw ticks are stored, which is still correct within one platform and is
		// the same "compares unequal -> fall back to the importer" outcome
		// everywhere else.
		int64_t FileMTimeNs(const std::filesystem::path& p, std::error_code& ec) {
			const std::filesystem::file_time_type ft = std::filesystem::last_write_time(p, ec);
			if (ec) return 0;
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
			const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
			return std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count();
#else
			return std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch()).count();
#endif
		}

		// 112 B. An EXPLICIT MIRROR of MaterialDesc rather than a memcpy of it,
		// for two reasons that both matter:
		//
		//  1. MaterialDesc holds LR_GUID, which has a user-provided copy
		//     constructor and is therefore not trivially copyable -- memcpying
		//     a MaterialDesc is undefined behaviour, not just unwise.
		//  2. MaterialDesc has no layout asserts and is expected to keep
		//     growing (the Phase 6 lobes arrived on it, more will). A raw dump
		//     would silently reinterpret every field after the insertion point.
		//     Converting field by field means adding a field to MaterialDesc
		//     leaves cooked files readable and merely drops the new field, and
		//     the compiler points at this struct when someone wants it carried.
		struct MaterialRecord {
			float    emission[4];
			float    color[4];
			float    metallic, roughness, ao, normalScale;
			uint64_t baseColorTex, normalTex, metalRoughTex, emissiveTex;
			float    specularLevel, clearcoat, clearcoatRough, sheenRoughness;
			float    sheenColor[3];
			float    anisotropy;
		};
		static_assert(sizeof(MaterialRecord) == 112);
		static_assert(std::is_trivially_copyable_v<MaterialRecord>);

		MaterialRecord ToRecord(const MaterialDesc& m) {
			MaterialRecord r{};
			r.emission[0] = m.emission.x; r.emission[1] = m.emission.y;
			r.emission[2] = m.emission.z; r.emission[3] = m.emission.w;
			r.color[0] = m.color.x; r.color[1] = m.color.y;
			r.color[2] = m.color.z; r.color[3] = m.color.w;
			r.metallic      = m.metallic;
			r.roughness     = m.roughness;
			r.ao            = m.ao;
			r.normalScale   = m.normalScale;
			r.baseColorTex  = static_cast<uint64_t>(m.baseColorTex);
			r.normalTex     = static_cast<uint64_t>(m.normalTex);
			r.metalRoughTex = static_cast<uint64_t>(m.metalRoughTex);
			r.emissiveTex   = static_cast<uint64_t>(m.emissiveTex);
			r.specularLevel  = m.specularLevel;
			r.clearcoat      = m.clearcoat;
			r.clearcoatRough = m.clearcoatRough;
			r.sheenRoughness = m.sheenRoughness;
			r.sheenColor[0] = m.sheenColor.r;
			r.sheenColor[1] = m.sheenColor.g;
			r.sheenColor[2] = m.sheenColor.b;
			r.anisotropy    = m.anisotropy;
			return r;
		}

		MaterialDesc FromRecord(const MaterialRecord& r) {
			MaterialDesc m;
			m.emission = { r.emission[0], r.emission[1], r.emission[2], r.emission[3] };
			m.color    = { r.color[0],    r.color[1],    r.color[2],    r.color[3] };
			m.metallic      = r.metallic;
			m.roughness     = r.roughness;
			m.ao            = r.ao;
			m.normalScale   = r.normalScale;
			m.baseColorTex  = LR_GUID(r.baseColorTex);
			m.normalTex     = LR_GUID(r.normalTex);
			m.metalRoughTex = LR_GUID(r.metalRoughTex);
			m.emissiveTex   = LR_GUID(r.emissiveTex);
			m.specularLevel  = r.specularLevel;
			m.clearcoat      = r.clearcoat;
			m.clearcoatRough = r.clearcoatRough;
			m.sheenRoughness = r.sheenRoughness;
			m.sheenColor = { r.sheenColor[0], r.sheenColor[1], r.sheenColor[2] };
			m.anisotropy = r.anisotropy;
			return m;
		}

		// FNV-1a 64. Not a cryptographic hash and not trying to be -- it exists
		// to catch a truncated or partially-written file, which is the failure
		// this format is actually exposed to. A CRC32 would be the other obvious
		// choice; FNV needs no table and no dependency.
		constexpr uint64_t kFnvOffset = 1469598103934665603ull;
		constexpr uint64_t kFnvPrime  = 1099511628211ull;

		uint64_t HashBytes(uint64_t seed, const void* data, size_t bytes) {
			const unsigned char* p = static_cast<const unsigned char*>(data);
			uint64_t h = seed;
			for (size_t i = 0; i < bytes; ++i) {
				h ^= static_cast<uint64_t>(p[i]);
				h *= kFnvPrime;
			}
			return h;
		}

		void AppendBytes(std::vector<unsigned char>& out, const void* data, size_t bytes) {
			if (bytes == 0) return;
			const unsigned char* p = static_cast<const unsigned char*>(data);
			out.insert(out.end(), p, p + bytes);
		}

		void PadToAlignment(std::vector<unsigned char>& out) {
			while (out.size() % kSectionAlign != 0) out.push_back(0u);
		}

		// What the reader expects each known section's elementSize to be. A
		// section whose stride disagrees is rejected rather than read at the
		// stride the file claims: trusting the file's stride would let a
		// corrupt header walk the reader through the payload at the wrong
		// step and produce a mesh instead of an error.
		uint32_t ExpectedElementSize(uint32_t id) {
			switch (id) {
				case SECTION_VERTICES:       return sizeof(Gpu::Vertex);
				case SECTION_TRI_REFS:       return sizeof(Gpu::TriRef);
				case SECTION_TRI_POSITIONS:  return sizeof(Gpu::TrianglePositions);
				case SECTION_BVH_NODES:      return sizeof(BVHAccel::Node);
				case SECTION_BVH_PRIM_INDEX: return sizeof(uint32_t);
				case SECTION_SUBMESHES:      return sizeof(SubmeshRecord);
				case SECTION_STRINGS:        return 1u;
				case SECTION_MATERIALS:      return sizeof(MaterialRecord);
				case SECTION_SOURCE_STAMP:   return sizeof(SourceStampRecord);
				default:                     return 0u;   // unknown -> skipped
			}
		}

		const char* SectionName(uint32_t id) {
			switch (id) {
				case SECTION_VERTICES:       return "vertices";
				case SECTION_TRI_REFS:       return "triRefs";
				case SECTION_TRI_POSITIONS:  return "triPositions";
				case SECTION_BVH_NODES:      return "bvhNodes";
				case SECTION_BVH_PRIM_INDEX: return "bvhPrimIndex";
				case SECTION_SUBMESHES:      return "submeshes";
				case SECTION_STRINGS:        return "strings";
				case SECTION_MATERIALS:      return "materials";
				case SECTION_SOURCE_STAMP:   return "sourceStamp";
				default:                     return "<unknown>";
			}
		}

		// The eight sections a version-1 file must contain. A file missing one
		// is rejected even though the section might legitimately be empty,
		// because the writer ALWAYS emits all eight (empty ones with count 0).
		// A missing section therefore means damage, not an older writer -- the
		// version field is what would have caught that.
		constexpr uint32_t kRequiredSections[] = {
			SECTION_VERTICES, SECTION_TRI_REFS, SECTION_TRI_POSITIONS,
			SECTION_BVH_NODES, SECTION_BVH_PRIM_INDEX,
			SECTION_SUBMESHES, SECTION_STRINGS, SECTION_MATERIALS,
		};

		// Materialize a typed array out of the file image. memcpy rather than a
		// reinterpret_cast over the buffer: the cast would be UB for any element
		// with an alignment requirement unless the buffer's base address happens
		// to be aligned too, and "happens to be" is not something to build a
		// file format on. The copy costs one pass over memory the vector's
		// allocation was going to fault in regardless.
		template <typename T>
		std::vector<T> ReadArray(const std::vector<unsigned char>& file, const SectionEntry& e) {
			std::vector<T> out(static_cast<size_t>(e.elementCount));
			if (e.elementCount != 0) {
				std::memcpy(out.data(), file.data() + e.byteOffset, static_cast<size_t>(e.byteSize));
			}
			return out;
		}

		template <typename T>
		bool ArrayBytesEqual(const std::vector<T>& a, const std::vector<T>& b) {
			if (a.size() != b.size()) return false;
			if (a.empty()) return true;   // memcmp(nullptr, nullptr, 0) is UB
			return std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
		}

	} // namespace


	// =========================================================================
	// WRITE
	// =========================================================================
	bool WriteCookedMesh(const std::filesystem::path& path, const CookedMesh& mesh,
	                     const CookedSourceStamp* stamp) {
		if (mesh.triPositions.size() != mesh.triRefs.size()) {
			LOG_ENGINE_CRITICAL("WriteCookedMesh: triPositions ({0}) and triRefs ({1}) must be in "
			                    "lockstep -- refusing to cook {2}",
				mesh.triPositions.size(), mesh.triRefs.size(), path.string());
			return false;
		}
		// The BVH permutation has exactly one entry per triangle. A mismatch
		// means the cook was handed a mesh whose BVH was built over a different
		// triangle set, which would traverse into whatever is at those indices.
		if (!mesh.triPositions.empty() && mesh.primIndex.size() != mesh.triPositions.size()) {
			LOG_ENGINE_CRITICAL("WriteCookedMesh: primIndex ({0}) does not cover triPositions ({1}) "
			                    "-- refusing to cook {2}",
				mesh.primIndex.size(), mesh.triPositions.size(), path.string());
			return false;
		}

		// --- flatten the two sections that are not already flat arrays -------
		std::vector<SubmeshRecord> submeshRecords;
		std::vector<unsigned char> strings;
		submeshRecords.reserve(mesh.submeshes.size());
		for (const SubmeshInfo& s : mesh.submeshes) {
			SubmeshRecord r{};
			r.firstTriIdx  = s.firstTriIdx;
			r.triCount     = s.triCount;
			r.materialSlot = s.materialSlot;
			r.nameOffset   = static_cast<uint32_t>(strings.size());
			r.nameLength   = static_cast<uint32_t>(s.name.size());
			r.reserved     = 0u;
			AppendBytes(strings, s.name.data(), s.name.size());
			submeshRecords.push_back(r);
		}

		std::vector<MaterialRecord> materialRecords;
		materialRecords.reserve(mesh.materials.size());
		for (const MaterialDesc& m : mesh.materials) materialRecords.push_back(ToRecord(m));

		// The stamp's name goes into the SAME string blob, appended AFTER every
		// submesh name. Order matters only in that the submesh offsets were
		// already assigned above and must not move; appending is the one edit
		// that cannot disturb them.
		std::vector<SourceStampRecord> stampRecords;
		if (stamp) {
			SourceStampRecord s{};
			s.sourceSize    = stamp->sourceSize;
			s.sourceMTimeNs = stamp->sourceMTimeNs;
			s.nameOffset    = static_cast<uint32_t>(strings.size());
			s.nameLength    = static_cast<uint32_t>(stamp->sourceName.size());
			AppendBytes(strings, stamp->sourceName.data(), stamp->sourceName.size());
			stampRecords.push_back(s);
		}

		struct Pending {
			uint32_t    id;
			uint32_t    elementSize;
			uint64_t    elementCount;
			const void* data;
		};
		const Pending pending[] = {
			{ SECTION_VERTICES,       sizeof(Gpu::Vertex),            mesh.vertices.size(),     mesh.vertices.data() },
			{ SECTION_TRI_REFS,       sizeof(Gpu::TriRef),            mesh.triRefs.size(),      mesh.triRefs.data() },
			{ SECTION_TRI_POSITIONS,  sizeof(Gpu::TrianglePositions), mesh.triPositions.size(), mesh.triPositions.data() },
			{ SECTION_BVH_NODES,      sizeof(BVHAccel::Node),         mesh.nodes.size(),        mesh.nodes.data() },
			{ SECTION_BVH_PRIM_INDEX, sizeof(uint32_t),               mesh.primIndex.size(),    mesh.primIndex.data() },
			{ SECTION_SUBMESHES,      sizeof(SubmeshRecord),          submeshRecords.size(),    submeshRecords.data() },
			{ SECTION_STRINGS,        1u,                             strings.size(),           strings.data() },
			{ SECTION_MATERIALS,      sizeof(MaterialRecord),         materialRecords.size(),   materialRecords.data() },
			// Always emitted, holding zero records when there is no stamp. A
			// section that is present-but-empty rather than absent keeps the
			// writer's output one fixed shape, which is what makes the
			// determinism check in the self-test meaningful -- otherwise two
			// cooks of the same mesh differ in their section COUNT depending on
			// whether a source could be stat'd.
			{ SECTION_SOURCE_STAMP,   sizeof(SourceStampRecord),      stampRecords.size(),      stampRecords.data() },
		};
		// sizeof rather than std::size: `pending` is not constexpr (it holds data
		// pointers), and sizeof is unevaluated so it needs nothing to be.
		constexpr uint32_t sectionCount = static_cast<uint32_t>(sizeof(pending) / sizeof(pending[0]));
		static_assert(sizeof(pending) / sizeof(pending[0]) >= std::size(kRequiredSections),
		              "the writer must emit at least the sections the reader requires");
		static_assert(sizeof(pending) / sizeof(pending[0]) <= kMaxSections);

		// --- assemble the whole file in memory -------------------------------
		// One buffer rather than a sequence of stream writes: the section table
		// carries offsets that are only known after the payloads are laid out,
		// and patching a stream means seeking backwards over a file that may be
		// on a network mount. This is an offline tool; the transient copy of the
		// mesh is an acceptable price for a writer with no seek in it.
		std::vector<unsigned char> file;
		file.resize(sizeof(Header) + sectionCount * sizeof(SectionEntry), 0u);

		std::vector<SectionEntry> table(sectionCount);
		uint64_t hash = kFnvOffset;
		for (uint32_t i = 0; i < sectionCount; ++i) {
			PadToAlignment(file);
			const uint64_t byteSize = pending[i].elementCount * pending[i].elementSize;

			table[i].id           = pending[i].id;
			table[i].elementSize  = pending[i].elementSize;
			table[i].elementCount = pending[i].elementCount;
			table[i].byteOffset   = file.size();
			table[i].byteSize     = byteSize;

			AppendBytes(file, pending[i].data, static_cast<size_t>(byteSize));
			// Payload bytes only -- not the header, not the table, not the
			// alignment padding. The hash is then a statement about CONTENT, so
			// a future change to the framing does not change the hash of
			// identical geometry.
			hash = HashBytes(hash, pending[i].data, static_cast<size_t>(byteSize));
		}

		Header header{};
		std::memcpy(header.magic, COOKED_MESH_MAGIC, sizeof(header.magic));
		header.version            = COOKED_MESH_VERSION;
		header.headerBytes        = sizeof(Header);
		header.layoutVertex       = sizeof(Gpu::Vertex);
		header.layoutTriRef       = sizeof(Gpu::TriRef);
		header.layoutTriPositions = sizeof(Gpu::TrianglePositions);
		header.layoutBvhNode      = sizeof(BVHAccel::Node);
		header.layoutSubmesh      = sizeof(SubmeshRecord);
		header.layoutMaterial     = sizeof(MaterialRecord);
		header.sectionCount       = sectionCount;
		header.reserved0          = 0u;
		header.payloadHash        = hash;
		header.reserved1          = 0ull;

		std::memcpy(file.data(), &header, sizeof(Header));
		std::memcpy(file.data() + sizeof(Header), table.data(), table.size() * sizeof(SectionEntry));

		// --- write via a temp file, then rename ------------------------------
		// A cook killed halfway (disk full, Ctrl-C, CI timeout) otherwise leaves
		// a truncated .x3mesh with a newer mtime than its source, which every
		// subsequent build treats as up to date. The rename is atomic within a
		// filesystem, so the destination is only ever the previous file or the
		// complete new one.
		std::error_code ec;
		if (path.has_parent_path()) {
			std::filesystem::create_directories(path.parent_path(), ec);
			// Not fatal on its own -- the directory may already exist, which
			// create_directories reports as ec-free but false. The open below is
			// the real test.
		}

		std::filesystem::path tempPath = path;
		tempPath += ".tmp";
		{
			std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
			if (!out.is_open()) {
				LOG_ENGINE_CRITICAL("WriteCookedMesh: could not open {0} for writing", tempPath.string());
				return false;
			}
			out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
			out.flush();
			if (!out.good()) {
				LOG_ENGINE_CRITICAL("WriteCookedMesh: write failed for {0} ({1} bytes)",
					tempPath.string(), file.size());
				out.close();
				std::filesystem::remove(tempPath, ec);
				return false;
			}
		}

		std::filesystem::rename(tempPath, path, ec);
		if (ec) {
			LOG_ENGINE_CRITICAL("WriteCookedMesh: could not move {0} into place at {1}: {2}",
				tempPath.string(), path.string(), ec.message());
			std::filesystem::remove(tempPath, ec);
			return false;
		}

		LOG_ENGINE_INFO("WriteCookedMesh: wrote {0} ({1} bytes, {2} tris / {3} verts / {4} nodes / "
		                "{5} submeshes / {6} materials), source stamp: {7}",
			path.string(), file.size(), mesh.triPositions.size(), mesh.vertices.size(),
			mesh.nodes.size(), mesh.submeshes.size(), mesh.materials.size(),
			// Named in the log because a file cooked without one loads fine and
			// then silently never wins a freshness check -- a symptom with no
			// other trace, since the fallback path works perfectly.
			stamp ? stamp->sourceName : std::string("<none -- this file can never be proven fresh>"));
		return true;
	}


	// =========================================================================
	// THE STALENESS KEY
	// =========================================================================
	std::optional<CookedSourceStamp> MakeCookedSourceStamp(const std::filesystem::path& source) {
		std::error_code ec;
		const uintmax_t size = std::filesystem::file_size(source, ec);
		if (ec) {
			LOG_ENGINE_WARN("MakeCookedSourceStamp: could not size {0}: {1} -- cooking without a "
			                "freshness key, this file will always be treated as stale",
				source.string(), ec.message());
			return std::nullopt;
		}
		const int64_t mtime = FileMTimeNs(source, ec);
		if (ec) {
			LOG_ENGINE_WARN("MakeCookedSourceStamp: could not read the modification time of {0}: {1} "
			                "-- cooking without a freshness key",
				source.string(), ec.message());
			return std::nullopt;
		}

		CookedSourceStamp stamp;
		stamp.sourceSize    = static_cast<uint64_t>(size);
		stamp.sourceMTimeNs = mtime;
		stamp.sourceName    = source.filename().string();
		return stamp;
	}


	// Reads the header, the section table and the stamp -- and nothing else. See
	// the declaration for why this deliberately skips the payload hash.
	//
	// EVERY OFFSET IS CHECKED AGAINST THE FILE'S SIZE ON DISK rather than against
	// a buffer, because unlike ReadCookedMesh this never has the whole file in
	// memory. That is the trade: a bounded read costs the discipline of doing the
	// bounds arithmetic by hand at each seek.
	std::optional<CookedSourceStamp> ReadCookedSourceStamp(const std::filesystem::path& path) {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;

		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in.is_open()) {
			LOG_ENGINE_WARN("ReadCookedSourceStamp: could not open {0}", path.string());
			return std::nullopt;
		}
		const std::streamoff fileSize = in.tellg();
		if (fileSize < static_cast<std::streamoff>(sizeof(Header))) return std::nullopt;

		const auto readAt = [&](uint64_t offset, void* dst, uint64_t bytes) {
			if (offset > static_cast<uint64_t>(fileSize) ||
			    bytes > static_cast<uint64_t>(fileSize) - offset) return false;
			in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
			in.read(static_cast<char*>(dst), static_cast<std::streamsize>(bytes));
			return in.good() && in.gcount() == static_cast<std::streamsize>(bytes);
		};

		Header header{};
		if (!readAt(0, &header, sizeof(Header))) return std::nullopt;

		// The same gate ReadCookedMesh applies, for the same reasons -- a file
		// this build cannot load must not be reported fresh, or the loader is
		// told to prefer something that is then rejected on the next line.
		if (std::memcmp(header.magic, COOKED_MESH_MAGIC, sizeof(header.magic)) != 0) return std::nullopt;
		if (header.version != COOKED_MESH_VERSION)  return std::nullopt;
		if (header.headerBytes != sizeof(Header))   return std::nullopt;
		if (header.layoutVertex       != sizeof(Gpu::Vertex))            return std::nullopt;
		if (header.layoutTriRef       != sizeof(Gpu::TriRef))            return std::nullopt;
		if (header.layoutTriPositions != sizeof(Gpu::TrianglePositions)) return std::nullopt;
		if (header.layoutBvhNode      != sizeof(BVHAccel::Node))         return std::nullopt;
		if (header.layoutSubmesh      != sizeof(SubmeshRecord))          return std::nullopt;
		if (header.layoutMaterial     != sizeof(MaterialRecord))         return std::nullopt;
		if (header.sectionCount == 0 || header.sectionCount > kMaxSections) return std::nullopt;

		std::vector<SectionEntry> table(header.sectionCount);
		if (!readAt(sizeof(Header), table.data(),
		            static_cast<uint64_t>(header.sectionCount) * sizeof(SectionEntry))) {
			return std::nullopt;
		}

		const SectionEntry* stampEntry   = nullptr;
		const SectionEntry* stringsEntry = nullptr;
		for (const SectionEntry& e : table) {
			// Overflow-safe extent validation, identical in shape to
			// ReadCookedMesh's. Duplicated rather than shared because the two
			// check against different notions of "the end" (a buffer there, a
			// file here) and merging them would mean one of the two doing its
			// bounds arithmetic in the other's terms.
			if (e.elementSize != 0 &&
			    e.elementCount > (std::numeric_limits<uint64_t>::max() / e.elementSize)) return std::nullopt;
			if (e.byteSize != e.elementCount * e.elementSize) return std::nullopt;
			if (e.byteOffset > static_cast<uint64_t>(fileSize) ||
			    e.byteSize > static_cast<uint64_t>(fileSize) - e.byteOffset) return std::nullopt;

			if (e.id == SECTION_SOURCE_STAMP)  stampEntry   = &e;
			if (e.id == SECTION_STRINGS)       stringsEntry = &e;
		}

		// No stamp section, or a stamp section with no records: a file cooked
		// before stamps existed, or one cooked from synthetic data. Not an error
		// and not a log line -- CheckCookedMeshFreshness reports it as NoStamp.
		if (!stampEntry || stampEntry->elementCount == 0) return std::nullopt;

		if (stampEntry->elementSize != sizeof(SourceStampRecord) ||
		    stampEntry->elementCount > kMaxStampRecords) {
			LOG_ENGINE_WARN("ReadCookedSourceStamp: {0} has a malformed source stamp section "
			                "({1} records of {2} bytes)",
				path.string(), stampEntry->elementCount, stampEntry->elementSize);
			return std::nullopt;
		}

		SourceStampRecord record{};
		if (!readAt(stampEntry->byteOffset, &record, sizeof(record))) return std::nullopt;

		CookedSourceStamp stamp;
		stamp.sourceSize    = record.sourceSize;
		stamp.sourceMTimeNs = record.sourceMTimeNs;

		if (record.nameLength != 0) {
			// The name span indexes a DIFFERENT section, so it is bounded by that
			// section's own length -- the same rule the submesh names follow, and
			// for the same reason: the file supplied both numbers.
			if (!stringsEntry ||
			    static_cast<uint64_t>(record.nameOffset) + record.nameLength > stringsEntry->byteSize) {
				LOG_ENGINE_WARN("ReadCookedSourceStamp: {0} has a source name span outside its string blob",
					path.string());
				return std::nullopt;
			}
			stamp.sourceName.resize(record.nameLength);
			if (!readAt(stringsEntry->byteOffset + record.nameOffset, stamp.sourceName.data(),
			            record.nameLength)) {
				return std::nullopt;
			}
		}
		return stamp;
	}


	const char* CookedFreshnessToString(CookedFreshness freshness) {
		switch (freshness) {
			case CookedFreshness::Fresh:           return "fresh";
			case CookedFreshness::NoCookedFile:    return "no cooked file";
			case CookedFreshness::OlderThanSource: return "older than its source";
			case CookedFreshness::NoStamp:         return "no source stamp";
			case CookedFreshness::Stale:           return "stale";
			case CookedFreshness::Unreadable:      return "unreadable";
		}
		return "<unknown>";
	}


	CookedFreshness CheckCookedMeshFreshness(const std::filesystem::path& cookedPath,
	                                         const std::filesystem::path& sourcePath,
	                                         std::string* why) {
		const auto reason = [&](CookedFreshness f, std::string text) {
			if (why) *why = std::move(text);
			return f;
		};

		std::error_code ec;
		if (!std::filesystem::is_regular_file(cookedPath, ec))
			return reason(CookedFreshness::NoCookedFile, "no " + cookedPath.filename().string());
		if (!std::filesystem::is_regular_file(sourcePath, ec))
			return reason(CookedFreshness::Unreadable, "the source " + sourcePath.string() + " is not a file");

		// --- the cheap pre-check ---------------------------------------------
		// Strictly weaker than the stamp comparison below -- anything this
		// catches, the stamp catches too -- and it is here only to answer the
		// common stale case without opening the file at all. Do not mistake it
		// for the key: an mtime on the cooked file is trivially newer after a
		// checkout, an unzip or an rsync that never looked at the source.
		// `>=` rather than `>` because a fast cook can finish inside one tick of
		// a filesystem's timestamp granularity, and a mesh that cooks in under a
		// second is exactly the mesh someone is iterating on.
		const int64_t cookedMTime = FileMTimeNs(cookedPath, ec);
		if (ec) return reason(CookedFreshness::Unreadable, "could not read the cooked file's mtime");
		const int64_t sourceMTime = FileMTimeNs(sourcePath, ec);
		if (ec) return reason(CookedFreshness::Unreadable, "could not read the source's mtime");
		if (cookedMTime < sourceMTime)
			return reason(CookedFreshness::OlderThanSource, "the source has been modified since the cook");

		// --- the key ----------------------------------------------------------
		const std::optional<CookedSourceStamp> stamp = ReadCookedSourceStamp(cookedPath);
		if (!stamp) {
			// Deliberately conflated with an unreadable file, because the ANSWER
			// is the same: this file cannot be proven fresh, so do not use it.
			// Splitting them would invite a caller to treat one as "probably
			// fine", which is the whole class of bug the stamp exists to remove.
			return reason(CookedFreshness::NoStamp,
			              "no usable source stamp -- re-cook it to make this file cacheable");
		}

		const uintmax_t sourceSize = std::filesystem::file_size(sourcePath, ec);
		if (ec) return reason(CookedFreshness::Unreadable, "could not size the source");

		if (stamp->sourceSize != static_cast<uint64_t>(sourceSize)) {
			return reason(CookedFreshness::Stale,
			              "the source is " + std::to_string(sourceSize) + " bytes, the cook saw " +
			              std::to_string(stamp->sourceSize));
		}
		if (stamp->sourceMTimeNs != sourceMTime) {
			return reason(CookedFreshness::Stale, "the source's modification time has changed");
		}
		if (!stamp->sourceName.empty() && stamp->sourceName != sourcePath.filename().string()) {
			return reason(CookedFreshness::Stale,
			              "cooked from '" + stamp->sourceName + "', asked about '" +
			              sourcePath.filename().string() + "'");
		}

		if (why) why->clear();
		return CookedFreshness::Fresh;
	}


	// =========================================================================
	// SEMANTIC VALIDATION -- see the declaration for what this adds over the
	// framing checks in ReadCookedMesh, and why none of it is an assert.
	// =========================================================================
	bool CookedMeshIsSelfConsistent(const CookedMesh& mesh, std::string* whyNot) {
		const auto fail = [&](std::string text) {
			if (whyNot) *whyNot = std::move(text);
			return false;
		};

		const uint64_t vertexCount   = mesh.vertices.size();
		const uint64_t triangleCount = mesh.triPositions.size();
		const uint64_t nodeCount     = mesh.nodes.size();
		const uint64_t materialCount = mesh.materials.size();

		// Restated here as well as in ReadCookedMesh, because this function is
		// also called on meshes that never went through a file (the cooker checks
		// what it is about to write) and the invariant is the mesh's, not the
		// format's.
		if (mesh.triRefs.size() != triangleCount)
			return fail("triRefs and triPositions are not in lockstep");
		if (triangleCount != 0 && mesh.primIndex.size() != triangleCount)
			return fail("the BVH primitive permutation does not cover every triangle");

		// --- 1. TRIANGLE -> VERTEX -------------------------------------------
		// THE ONE THAT MATTERS MOST. MergeMesh adds firstVertexIdx to each of
		// these and the shader dereferences the result with no bound of its own,
		// so an out-of-range index here is an out-of-bounds read of the pool's
		// VertexBuffer on the GPU -- garbage geometry at best, a device lost at
		// worst, and nothing anywhere else in the load path looks at it.
		for (uint64_t i = 0; i < mesh.triRefs.size(); ++i) {
			const Gpu::TriRef& t = mesh.triRefs[i];
			if (t.i0 >= vertexCount || t.i1 >= vertexCount || t.i2 >= vertexCount) {
				return fail("triangle " + std::to_string(i) + " indexes vertices (" +
				            std::to_string(t.i0) + ", " + std::to_string(t.i1) + ", " +
				            std::to_string(t.i2) + ") of " + std::to_string(vertexCount));
			}
			// materialSlot indexes importedMaterials in Renderer::Parse, which
			// becomes MeshMetadata::materialSlotCount == materials.size().
			if (t.materialSlot >= materialCount) {
				return fail("triangle " + std::to_string(i) + " uses material slot " +
				            std::to_string(t.materialSlot) + " of " + std::to_string(materialCount));
			}
		}

		// --- 2. SUBMESH RANGES ------------------------------------------------
		// Mesh-local by SubmeshInfo's contract, so they bound against this
		// mesh's own triangle count. Phase 7's rasterizer turns each of these
		// into a draw range directly.
		for (uint64_t i = 0; i < mesh.submeshes.size(); ++i) {
			const SubmeshInfo& s = mesh.submeshes[i];
			if (static_cast<uint64_t>(s.firstTriIdx) + s.triCount > triangleCount) {
				return fail("submesh " + std::to_string(i) + " covers triangles [" +
				            std::to_string(s.firstTriIdx) + ", +" + std::to_string(s.triCount) +
				            ") of " + std::to_string(triangleCount));
			}
			if (s.materialSlot >= materialCount && materialCount != 0) {
				return fail("submesh " + std::to_string(i) + " uses material slot " +
				            std::to_string(s.materialSlot) + " of " + std::to_string(materialCount));
			}
		}

		// --- 3. THE BVH -------------------------------------------------------
		// Trace.slang walks this with a fixed-size stack and no bounds checks --
		// it cannot afford them per node. An interior node whose child index is
		// out of range walks into whatever follows the node buffer; one that
		// points at itself or backwards is a loop the traversal never leaves,
		// which on a GPU is a hung queue and a device-lost, not an exception.
		for (uint64_t i = 0; i < mesh.nodes.size(); ++i) {
			const BVHAccel::Node& n = mesh.nodes[i];
			if (n.triCount == 0) {
				// Interior: leftChild_Or_FirstTri is a node index, and the right
				// child is always left+1 by BVHAccel's construction.
				const uint64_t left = n.leftChild_Or_FirstTri;
				if (left + 1 >= nodeCount) {
					return fail("BVH node " + std::to_string(i) + " has children " +
					            std::to_string(left) + "/" + std::to_string(left + 1) +
					            " of " + std::to_string(nodeCount));
				}
				if (left <= i) {
					// BVHAccel::SubDivide only ever allocates children AFTER the
					// parent, so a forward edge is not a style preference: it is
					// what makes the tree acyclic, and it is the only cheap check
					// that rules out a traversal that never terminates.
					return fail("BVH node " + std::to_string(i) + " has a non-forward child " +
					            std::to_string(left));
				}
			}
			else {
				// Leaf: leftChild_Or_FirstTri indexes this mesh's slice of the
				// primitive permutation.
				if (static_cast<uint64_t>(n.leftChild_Or_FirstTri) + n.triCount > triangleCount) {
					return fail("BVH leaf " + std::to_string(i) + " covers primitives [" +
					            std::to_string(n.leftChild_Or_FirstTri) + ", +" +
					            std::to_string(n.triCount) + ") of " + std::to_string(triangleCount));
				}
			}
		}

		// --- 4. THE PERMUTATION -----------------------------------------------
		// Its VALUES are triangle indices, mesh-local (Trace.slang adds
		// rootTriIdx). A value past the end resolves to another mesh's triangle,
		// which renders as geometry from an unrelated model appearing inside this
		// one -- a symptom nobody would think to blame on a cooked file.
		for (uint64_t i = 0; i < mesh.primIndex.size(); ++i) {
			if (mesh.primIndex[i] >= triangleCount) {
				return fail("BVH primitive " + std::to_string(i) + " refers to triangle " +
				            std::to_string(mesh.primIndex[i]) + " of " + std::to_string(triangleCount));
			}
		}

		if (whyNot) whyNot->clear();
		return true;
	}


	// =========================================================================
	// READ
	//
	// Every failure below is a `return std::nullopt` with a log line naming what
	// disagreed. None of them are asserts: a cooked file is data that arrived
	// from outside this process, and the whole reason to version and hash it is
	// to reject it politely rather than crash on it in a shipped runtime.
	// =========================================================================
	std::optional<CookedMesh> ReadCookedMesh(const std::filesystem::path& path) {
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			LOG_ENGINE_WARN("ReadCookedMesh: not a file: {0}", path.string());
			return std::nullopt;
		}

		// The entire file in one read. Every bounds check below is then against
		// one known size; the alternative -- seeking per section -- turns a
		// truncated file into a scatter of short reads that each have to be
		// checked separately, and one forgotten check is a buffer overrun.
		std::vector<unsigned char> file;
		{
			std::ifstream in(path, std::ios::binary | std::ios::ate);
			if (!in.is_open()) {
				LOG_ENGINE_WARN("ReadCookedMesh: could not open {0}", path.string());
				return std::nullopt;
			}
			const std::streamoff size = in.tellg();
			if (size < 0) {
				LOG_ENGINE_WARN("ReadCookedMesh: could not size {0}", path.string());
				return std::nullopt;
			}
			file.resize(static_cast<size_t>(size));
			in.seekg(0, std::ios::beg);
			if (!file.empty()) {
				in.read(reinterpret_cast<char*>(file.data()), size);
				if (in.gcount() != size) {
					LOG_ENGINE_WARN("ReadCookedMesh: short read on {0} ({1} of {2} bytes)",
						path.string(), static_cast<int64_t>(in.gcount()), static_cast<int64_t>(size));
					return std::nullopt;
				}
			}
		}

		if (file.size() < sizeof(Header)) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} is {1} bytes, smaller than a header",
				path.string(), file.size());
			return std::nullopt;
		}

		Header header{};
		std::memcpy(&header, file.data(), sizeof(Header));

		if (std::memcmp(header.magic, COOKED_MESH_MAGIC, sizeof(header.magic)) != 0) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} is not a cooked mesh (bad magic)", path.string());
			return std::nullopt;
		}
		if (header.version != COOKED_MESH_VERSION) {
			// No backward compatibility while the format is pre-1.0. A reader
			// that guesses at an older layout is exactly how garbage gets
			// loaded; re-cook instead.
			LOG_ENGINE_WARN("ReadCookedMesh: {0} is format version {1}, this build reads {2} -- re-cook it",
				path.string(), header.version, COOKED_MESH_VERSION);
			return std::nullopt;
		}
		if (header.headerBytes != sizeof(Header)) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} declares a {1}-byte header, this build has {2}",
				path.string(), header.headerBytes, sizeof(Header));
			return std::nullopt;
		}

		// GUARD 2. See the comment on Header. The version check above cannot
		// catch this case: an engine edit that changes a mirrored struct without
		// touching the version leaves files that claim to be readable and are
		// not.
		struct { const char* what; uint32_t inFile; uint32_t inBuild; } fingerprints[] = {
			{ "Gpu::Vertex",            header.layoutVertex,       sizeof(Gpu::Vertex) },
			{ "Gpu::TriRef",            header.layoutTriRef,       sizeof(Gpu::TriRef) },
			{ "Gpu::TrianglePositions", header.layoutTriPositions, sizeof(Gpu::TrianglePositions) },
			{ "BVHAccel::Node",         header.layoutBvhNode,      sizeof(BVHAccel::Node) },
			{ "SubmeshRecord",          header.layoutSubmesh,      sizeof(SubmeshRecord) },
			{ "MaterialRecord",         header.layoutMaterial,     sizeof(MaterialRecord) },
		};
		for (const auto& f : fingerprints) {
			if (f.inFile != f.inBuild) {
				LOG_ENGINE_WARN("ReadCookedMesh: {0} was cooked with sizeof({1}) == {2}, this build "
				                "has {3} -- the struct changed, re-cook it",
					path.string(), f.what, f.inFile, f.inBuild);
				return std::nullopt;
			}
		}

		if (header.sectionCount == 0 || header.sectionCount > kMaxSections) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} declares {1} sections, which is not plausible",
				path.string(), header.sectionCount);
			return std::nullopt;
		}
		const uint64_t tableBytes = static_cast<uint64_t>(header.sectionCount) * sizeof(SectionEntry);
		if (file.size() < sizeof(Header) + tableBytes) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} is truncated inside its section table", path.string());
			return std::nullopt;
		}

		std::vector<SectionEntry> table(header.sectionCount);
		std::memcpy(table.data(), file.data() + sizeof(Header), static_cast<size_t>(tableBytes));

		// --- validate every entry before touching any payload ----------------
		std::unordered_map<uint32_t, const SectionEntry*> byId;
		for (const SectionEntry& e : table) {
			if (byId.contains(e.id)) {
				// Duplicate ids would leave "which one wins" to whichever loop
				// ran last. There is no correct answer, so there is no answer.
				LOG_ENGINE_WARN("ReadCookedMesh: {0} has two sections with id {1} ({2})",
					path.string(), e.id, SectionName(e.id));
				return std::nullopt;
			}
			// Overflow-safe: elementCount and elementSize are both file-supplied
			// and their product must be checked before it is used as a length.
			if (e.elementSize != 0 && e.elementCount > (std::numeric_limits<uint64_t>::max() / e.elementSize)) {
				LOG_ENGINE_WARN("ReadCookedMesh: {0} section {1} has an overflowing extent", path.string(), e.id);
				return std::nullopt;
			}
			if (e.byteSize != e.elementCount * e.elementSize) {
				LOG_ENGINE_WARN("ReadCookedMesh: {0} section {1} ({2}) says {3} bytes but {4} x {5}",
					path.string(), e.id, SectionName(e.id), e.byteSize, e.elementCount, e.elementSize);
				return std::nullopt;
			}
			if (e.byteOffset > file.size() || e.byteSize > file.size() - e.byteOffset) {
				LOG_ENGINE_WARN("ReadCookedMesh: {0} section {1} ({2}) runs past the end of the file",
					path.string(), e.id, SectionName(e.id));
				return std::nullopt;
			}
			const uint32_t expected = ExpectedElementSize(e.id);
			// expected == 0 means an id this build does not know: skipped, which
			// is the forward-compatibility mechanism. A KNOWN id at an
			// unexpected stride is a hard error -- see ExpectedElementSize.
			if (expected != 0 && e.elementSize != expected) {
				LOG_ENGINE_WARN("ReadCookedMesh: {0} section {1} ({2}) has stride {3}, expected {4}",
					path.string(), e.id, SectionName(e.id), e.elementSize, expected);
				return std::nullopt;
			}
			byId[e.id] = &e;
		}

		for (uint32_t required : kRequiredSections) {
			if (!byId.contains(required)) {
				LOG_ENGINE_WARN("ReadCookedMesh: {0} is missing the {1} section",
					path.string(), SectionName(required));
				return std::nullopt;
			}
		}

		// --- payload hash, in table order (the order the writer hashed) ------
		uint64_t hash = kFnvOffset;
		for (const SectionEntry& e : table) {
			hash = HashBytes(hash, file.data() + e.byteOffset, static_cast<size_t>(e.byteSize));
		}
		if (hash != header.payloadHash) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} failed its payload hash (file {1}, computed {2}) -- "
			                "the file is corrupt or was truncated mid-write",
				path.string(), header.payloadHash, hash);
			return std::nullopt;
		}

		// --- materialize ------------------------------------------------------
		CookedMesh mesh;
		mesh.vertices     = ReadArray<Gpu::Vertex>           (file, *byId[SECTION_VERTICES]);
		mesh.triRefs      = ReadArray<Gpu::TriRef>           (file, *byId[SECTION_TRI_REFS]);
		mesh.triPositions = ReadArray<Gpu::TrianglePositions>(file, *byId[SECTION_TRI_POSITIONS]);
		mesh.nodes        = ReadArray<BVHAccel::Node>        (file, *byId[SECTION_BVH_NODES]);
		mesh.primIndex    = ReadArray<uint32_t>              (file, *byId[SECTION_BVH_PRIM_INDEX]);

		const SectionEntry& stringsEntry = *byId[SECTION_STRINGS];
		const std::vector<SubmeshRecord> submeshRecords = ReadArray<SubmeshRecord>(file, *byId[SECTION_SUBMESHES]);
		mesh.submeshes.reserve(submeshRecords.size());
		for (const SubmeshRecord& r : submeshRecords) {
			// The name span is file-supplied and indexes a different section, so
			// it is checked against that section's own length rather than
			// trusted. Same overflow discipline as the section extents above.
			if (static_cast<uint64_t>(r.nameOffset) + r.nameLength > stringsEntry.byteSize) {
				LOG_ENGINE_WARN("ReadCookedMesh: {0} has a submesh name span [{1}, +{2}) outside its "
				                "{3}-byte string blob",
					path.string(), r.nameOffset, r.nameLength, stringsEntry.byteSize);
				return std::nullopt;
			}
			SubmeshInfo s;
			s.firstTriIdx  = r.firstTriIdx;
			s.triCount     = r.triCount;
			s.materialSlot = r.materialSlot;
			s.name.assign(reinterpret_cast<const char*>(file.data() + stringsEntry.byteOffset + r.nameOffset),
			              r.nameLength);
			mesh.submeshes.push_back(std::move(s));
		}

		const std::vector<MaterialRecord> materialRecords = ReadArray<MaterialRecord>(file, *byId[SECTION_MATERIALS]);
		mesh.materials.reserve(materialRecords.size());
		for (const MaterialRecord& r : materialRecords) mesh.materials.push_back(FromRecord(r));

		// A file can be internally consistent and still describe an impossible
		// mesh. These two invariants are the ones the renderer assumes without
		// checking, so they are checked here, once, at the boundary.
		if (mesh.triPositions.size() != mesh.triRefs.size()) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} has {1} triPositions and {2} triRefs; they must be in lockstep",
				path.string(), mesh.triPositions.size(), mesh.triRefs.size());
			return std::nullopt;
		}
		if (!mesh.triPositions.empty() && mesh.primIndex.size() != mesh.triPositions.size()) {
			LOG_ENGINE_WARN("ReadCookedMesh: {0} has {1} BVH prim indices for {2} triangles",
				path.string(), mesh.primIndex.size(), mesh.triPositions.size());
			return std::nullopt;
		}

		LOG_ENGINE_INFO("ReadCookedMesh: read {0} ({1} tris / {2} verts / {3} nodes / {4} submeshes / "
		                "{5} materials)",
			path.string(), mesh.triPositions.size(), mesh.vertices.size(), mesh.nodes.size(),
			mesh.submeshes.size(), mesh.materials.size());
		return mesh;
	}


	// =========================================================================
	// EXTRACT -- pool slice to cookable mesh.
	// =========================================================================
	std::optional<CookedMesh> ExtractCookedMesh(const AssetPool& pool, LR_GUID guid) {
		const std::shared_ptr<MeshMetadata> meta = pool.find<MeshMetadata>(guid);
		if (!meta) {
			LOG_ENGINE_CRITICAL("ExtractCookedMesh: GUID {0} is not a mesh in this pool", (uint64_t)guid);
			return std::nullopt;
		}

		// The pool's own cross-buffer invariants, restated as checks. Every
		// range below is metadata-supplied, and a slice taken outside a buffer
		// is a silently wrong cooked file rather than a crash -- the arrays
		// would simply come from somewhere else. Bounds-check every one.
		const auto inRange = [](size_t first, size_t count, size_t size, const char* what) {
			if (first > size || count > size - first) {
				LOG_ENGINE_CRITICAL("ExtractCookedMesh: {0} range [{1}, +{2}) is outside the pool's "
				                    "{3}-element buffer", what, first, count, size);
				return false;
			}
			return true;
		};

		const size_t firstTri  = meta->firstTriIdx;
		const size_t triCount  = meta->TriCount;
		const size_t firstVert = meta->firstVertexIdx;
		const size_t vertCount = meta->vertexCount;
		const size_t firstNode = meta->firstNodeIdx;
		const size_t nodeCount = meta->nodeCount;

		if (!inRange(firstTri,  triCount,  pool.TriPositionBuffer.size(),  "triPositions")) return std::nullopt;
		if (!inRange(firstTri,  triCount,  pool.TriRefBuffer.size(),       "triRefs"))      return std::nullopt;
		if (!inRange(firstVert, vertCount, pool.VertexBuffer.size(),       "vertices"))     return std::nullopt;
		if (!inRange(firstNode, nodeCount, pool.NodeBuffer.size(),         "bvhNodes"))     return std::nullopt;
		// BvhPrimIndexBuffer is appended in lockstep with TriPositionBuffer --
		// one entry per triangle, every mesh -- which is what makes firstTriIdx
		// index BOTH. Nothing in AssetPool documents that as an invariant, so it
		// is verified here rather than assumed.
		if (!inRange(firstTri, triCount, pool.BvhPrimIndexBuffer.size(), "bvhPrimIndex")) return std::nullopt;

		CookedMesh mesh;
		mesh.vertices.assign(pool.VertexBuffer.begin() + firstVert,
		                     pool.VertexBuffer.begin() + firstVert + vertCount);
		mesh.triPositions.assign(pool.TriPositionBuffer.begin() + firstTri,
		                         pool.TriPositionBuffer.begin() + firstTri + triCount);
		mesh.nodes.assign(pool.NodeBuffer.begin() + firstNode,
		                  pool.NodeBuffer.begin() + firstNode + nodeCount);
		mesh.primIndex.assign(pool.BvhPrimIndexBuffer.begin() + firstTri,
		                      pool.BvhPrimIndexBuffer.begin() + firstTri + triCount);

		// THE ONE UN-REBASE, the exact inverse of MergeMesh's one rebase. The
		// pool holds GLOBAL vertex indices (firstVertexIdx already added, which
		// is what saves the shader an add per hit); a cooked file must be
		// loadable into a pool at ANY offset, so it stores them mesh-local and
		// whatever loads it adds its own base back. Skipping this step produces
		// a file that only works when loaded first, into an empty pool -- which
		// is exactly the configuration a round-trip test runs in, so the test
		// would not catch it.
		mesh.triRefs.reserve(triCount);
		for (size_t i = 0; i < triCount; ++i) {
			const Gpu::TriRef& t = pool.TriRefBuffer[firstTri + i];
			if (t.i0 < firstVert || t.i1 < firstVert || t.i2 < firstVert ||
			    t.i0 >= firstVert + vertCount || t.i1 >= firstVert + vertCount || t.i2 >= firstVert + vertCount) {
				LOG_ENGINE_CRITICAL("ExtractCookedMesh: triangle {0} indexes vertices ({1}, {2}, {3}) "
				                    "outside this mesh's range [{4}, +{5})",
					i, t.i0, t.i1, t.i2, firstVert, vertCount);
				return std::nullopt;
			}
			mesh.triRefs.push_back(Gpu::TriRef{
				t.i0 - static_cast<uint32_t>(firstVert),
				t.i1 - static_cast<uint32_t>(firstVert),
				t.i2 - static_cast<uint32_t>(firstVert),
				t.materialSlot });
		}

		// submeshes' firstTriIdx is already mesh-local (SubmeshInfo's contract),
		// so these copy unchanged.
		mesh.submeshes = meta->submeshes;
		mesh.materials = meta->importedMaterials;
		return mesh;
	}


	// =========================================================================
	// COMPARE
	// =========================================================================
	bool CookedMeshBytesEqual(const CookedMesh& a, const CookedMesh& b, std::string* whatDiffers) {
		const auto fail = [&](const char* what) {
			if (whatDiffers) *whatDiffers = what;
			return false;
		};

		if (!ArrayBytesEqual(a.vertices,     b.vertices))     return fail("vertices");
		if (!ArrayBytesEqual(a.triRefs,      b.triRefs))      return fail("triRefs");
		if (!ArrayBytesEqual(a.triPositions, b.triPositions)) return fail("triPositions");
		if (!ArrayBytesEqual(a.nodes,        b.nodes))        return fail("bvhNodes");
		if (!ArrayBytesEqual(a.primIndex,    b.primIndex))    return fail("bvhPrimIndex");

		if (a.submeshes.size() != b.submeshes.size()) return fail("submesh count");
		for (size_t i = 0; i < a.submeshes.size(); ++i) {
			const SubmeshInfo& x = a.submeshes[i];
			const SubmeshInfo& y = b.submeshes[i];
			if (x.firstTriIdx != y.firstTriIdx || x.triCount != y.triCount ||
			    x.materialSlot != y.materialSlot || x.name != y.name) {
				return fail("submesh entry");
			}
		}

		// Materials are compared in their STORED form. Comparing MaterialDesc
		// field by field would compare floats by value, and the arrays above are
		// compared by bytes -- one gate cannot use two different definitions of
		// "identical" and still mean anything. Converting both sides here also
		// makes the comparison cover exactly what the file carries, so a field
		// this format silently drops shows up as a failure instead of passing.
		if (a.materials.size() != b.materials.size()) return fail("material count");
		for (size_t i = 0; i < a.materials.size(); ++i) {
			const MaterialRecord x = ToRecord(a.materials[i]);
			const MaterialRecord y = ToRecord(b.materials[i]);
			if (std::memcmp(&x, &y, sizeof(MaterialRecord)) != 0) return fail("material entry");
		}

		return true;
	}

}
