#pragma once

// =============================================================================
// AssetCook.h -- the .x3mesh binary format: serialized geometry + prebuilt BVH.
//
// WHAT THIS IS FOR. Today every asset is re-imported from its original source
// path on every project open (`.lrmeta` sidecars store `sourcePath`), which
// means an assimp parse plus a full SAH BVH build per mesh, per open. The BVH
// build dominates that cost and is entirely deterministic -- it is the same
// tree every time from the same triangles. Phase 9 exists to stop paying for it
// twice, and this file is the first step: the geometry and the tree it produced
// written out once, in the exact in-memory layout the GPU buffers already use.
//
// SCOPE, DELIBERATELY NARROW. This is geometry, BVH and the submesh/material
// tables. It is NOT the rest of Phase 9: no BC7 texture compression, no
// meshoptimizer re-layout, no binary scene format. Those each need an external
// dependency and each change what a cooked mesh CONTAINS, whereas this defines
// how a cooked mesh is FRAMED. A section table plus a version field is what lets
// them land later without a rewrite (see "EXTENDING" below).
//
// THE ENGINE NOW READS THESE FILES. When this file was written nothing but
// X3AssetCook could parse a .x3mesh, and the paragraph here said so. That was
// the gap the plan called out, and AssetManager::DecodeCookedMesh closes it: it
// turns a CookedMesh into exactly what AssetManager::DecodeMesh produces, so
// MergeMesh consumes the two identically and there is no second merge path to
// keep in sync. Two consequences worth stating where the format is defined:
//   - A COOKED FILE IS UNTRUSTED DATA once a shipped runtime reads it. Every
//     check in the reader below returns nullopt and logs; none of them assert.
//     The caller's contract is "fall back to the importer", never "crash".
//   - The framing checks here are not sufficient on their own. A file can pass
//     magic, version, fingerprint, extents and hash and still describe a
//     triangle indexing vertex 4 billion. See CookedMeshIsSelfConsistent.
//
// THE LAYOUT CONTRACT. The arrays are dumped raw. Gpu::Vertex, Gpu::TriRef,
// Gpu::TrianglePositions and BVHAccel::Node are GPU-mirrored structs that carry
// sizeof/offsetof static_asserts in GpuTypes.h and BVHAccel.h; the file is a
// straight image of them, so a cooked buffer can be uploaded with no
// per-element conversion whatsoever. That is the whole point of the format and
// it is also its one sharp edge: if any of those structs is edited, every
// previously cooked file becomes garbage that still parses.
//
// Two independent guards, because one is not enough:
//   1. static_asserts in AssetCook.cpp repeat the size asserts from GpuTypes.h.
//      A layout edit then breaks the BUILD of the cooker. Loud, immediate, and
//      it names this file as something that must be considered.
//   2. The header records the sizeof of every mirrored struct as a layout
//      FINGERPRINT, checked on read. That catches the other half: an old file
//      meeting a new binary. Without it, a Vertex that grew a field would load
//      as a mesh whose positions are progressively more wrong the further into
//      the buffer you look -- which reads as a broken importer, not a stale
//      cache.
// Guard 1 catches the edit; guard 2 catches the file. Neither substitutes.
//
// EXTENDING. Sections are addressed by id through a table, not by position, so:
//   - adding a section (lightmap UVs, meshlets, BC7 mips) needs NO version bump
//     -- an older reader skips ids it does not know;
//   - changing an EXISTING section's element layout REQUIRES a version bump,
//     because the fingerprint only covers structs whose sizeof happens to
//     change. Reordering two floats inside Gpu::Vertex does not change its size
//     and no automatic check can see it. Bump the version.
// SECTION_SOURCE_STAMP was the first section added on that promise and it held:
// version 1 files and version 1 readers still interoperate in both directions.
// The one thing to check before believing it a second time is that the payload
// hash covers sections IN TABLE ORDER on both sides, which is what lets an added
// section participate in the hash without invalidating anything -- it does.
//
// THE CACHE KEY, WHICH THIS FILE USED TO SAY IT DID NOT HAVE. The original
// version of this comment argued that staleness belonged in ProjectExporter,
// because "a mesh loaded from a .x3mesh has no source". That reasoning does not
// survive the editor's "load cooked" mode: there the cooked file is chosen
// INSTEAD OF a source that is still sitting next to it, and if it is older than
// that source the editor silently shows yesterday's geometry -- the exact
// cook-only bug the mode exists to reproduce, now caused by the mode itself.
//
// So the key lives in the file, as SECTION_SOURCE_STAMP: the source's size and
// modification time, plus its filename. Recorded by the writer, compared by
// CheckCookedMeshFreshness against the source on disk.
//
//   - IN THE FILE, not in a sidecar, because a sidecar can be lost, copied
//     without its file, or left behind by a `rm *.x3mesh`, and every one of
//     those makes a stale file look fresh.
//   - SIZE AND MTIME, not a content hash. Hashing a 200 MB source on every
//     import to decide whether to skip a 40 ms load is a cache that costs more
//     than it saves. The pair is what every build system in existence uses and
//     it fails in the safe direction: an edit that preserves both is rare, and
//     the alternative is not "hash" but "no check at all".
//   - AS A SECTION, which is why no version bump was needed -- see EXTENDING
//     above. A build predating this section reads a file that has one and skips
//     it; a build with this section reads a file that has none and reports
//     NoStamp, which the loader treats as "not provably fresh" and falls back.
//   - OPTIONAL. A file cooked from synthetic data (the self-test) or from a
//     source that could not be stat'd carries no stamp rather than a zeroed one
//     -- a zeroed stamp compares unequal to every real source, which is the same
//     outcome by a route that looks like corruption in the log.
// =============================================================================

#include "lrpch.h"
#include "Core/GUID.h"
#include "Project/Assets/AssetManager.h"
#include "Project/Assets/AssetTypes.h"
#include "Project/Assets/BVHAccel.h"
#include "Project/Assets/MaterialDesc.h"
#include "Renderer/GpuTypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace X3
{

	/// The cooked-mesh file extension. A constant rather than a macro; the
	/// #define next to ASSET_META_FILE_EXTENSION is legacy, do not copy it.
	inline constexpr const char* COOKED_MESH_FILE_EXTENSION = ".x3mesh";

	/// Magic and version as written into the file header. Exposed so tools can
	/// print them and so a test can assert against a literal rather than against
	/// whatever the writer happened to emit.
	inline constexpr char     COOKED_MESH_MAGIC[8] = { 'X','3','M','E','S','H','\0','\0' };
	inline constexpr uint32_t COOKED_MESH_VERSION  = 1;

	// -------------------------------------------------------------------------
	// One mesh asset in the form the file round-trips.
	//
	// This deliberately mirrors the geometry half of
	// AssetManager::MeshImportResult, which is a PRIVATE nested type and
	// therefore unreachable from here. Duplicating the shape is the cost of not
	// touching AssetManager; the alternative -- promoting MeshImportResult to
	// public -- widens the AssetManager surface for the benefit of one tool, and
	// the split between decode and merge is the thing that keeps the pool
	// single-writer. Do not "fix" this by exposing it.
	//
	// EVERY INDEX IS MESH-LOCAL, exactly as DecodeMesh produces them:
	//   - triRefs' i0/i1/i2 index `vertices` from 0, NOT the pool's global
	//     VertexBuffer. The pool holds them already rebased by firstVertexIdx
	//     (MergeMesh's one rebase); ExtractCookedMesh undoes that. A cooked file
	//     must be loadable into a pool at any offset, so it cannot bake in the
	//     offset it happened to be imported at.
	//   - submeshes' firstTriIdx is relative to triPositions[0], matching
	//     SubmeshInfo's own contract.
	//   - BVH nodes and primIndex are already mesh-relative by construction and
	//     need nothing done to them; that is why a merge is a plain append.
	// -------------------------------------------------------------------------
	struct CookedMesh {
		std::vector<Gpu::Vertex>            vertices;
		std::vector<Gpu::TriRef>            triRefs;      // lockstep with triPositions
		std::vector<Gpu::TrianglePositions> triPositions;
		std::vector<BVHAccel::Node>         nodes;
		std::vector<uint32_t>               primIndex;    // BVH primitive permutation

		std::vector<SubmeshInfo>  submeshes;

		// The model file's materials, one per dense slot.
		//
		// HONEST LIMITATION: the texture GUIDs inside these are references to
		// texture assets that this file does NOT contain -- cooking textures is
		// the BC7 half of Phase 9 and is not implemented. A .x3mesh loaded
		// standalone therefore shades from the scalar factors alone. They are
		// carried anyway rather than dropped, because MeshMetadata holds them
		// and a format that silently loses authored data is a format people
		// stop trusting.
		std::vector<MaterialDesc> materials;
	};

	// -------------------------------------------------------------------------
	// THE STALENESS KEY. See "THE CACHE KEY" at the top of this file for why it
	// is size+mtime rather than a hash, and why it is a section rather than a
	// sidecar.
	// -------------------------------------------------------------------------
	struct CookedSourceStamp {
		/// The source's size in bytes at cook time.
		uint64_t sourceSize = 0;

		/// The source's last-write time, in nanoseconds since the SYSTEM CLOCK
		/// epoch -- not since std::filesystem::file_time_type's own epoch, which
		/// is implementation-defined and differs between MSVC and libstdc++. A
		/// file cooked on the CI Linux box and consumed by a Windows editor would
		/// otherwise compare unequal against an unchanged source, quietly turning
		/// the cook cache off for everyone on that platform. Where the standard
		/// library cannot make that conversion, the raw file-clock ticks are
		/// stored instead and the comparison stays correct within one platform;
		/// see FileMTimeNs.
		int64_t sourceMTimeNs = 0;

		/// The source's FILENAME (not its path). Paths move -- a project is
		/// checked out somewhere else, an asset folder is renamed -- and a stamp
		/// keyed on an absolute path would call every cooked file in a moved
		/// checkout stale. The filename is what catches the case size+mtime
		/// cannot: `a.glb.x3mesh` copied over `b.glb.x3mesh`, where two unrelated
		/// meshes can plausibly share a size and a timestamp.
		std::string sourceName;
	};

	/// Stat `source` into a stamp. Returns std::nullopt and logs if it cannot be
	/// stat'd -- the caller then cooks WITHOUT a stamp rather than with a zeroed
	/// one (a zeroed stamp reads as corruption, absence reads as absence).
	std::optional<CookedSourceStamp> MakeCookedSourceStamp(const std::filesystem::path& source);

	/// Serialize `mesh` to `path` (conventionally *.x3mesh).
	///
	/// Writes to a sibling temp file and renames on success, so an interrupted
	/// cook leaves either the previous file or nothing -- never a truncated one
	/// that a later build would happily treat as valid. Returns false and logs
	/// on any I/O failure; never throws.
	///
	/// `stamp` is the freshness key. Passing nullptr writes a file that no
	/// staleness check can ever pass -- correct for synthetic data, and a
	/// deliberate opt-out rather than a default, because "cooked without a
	/// stamp" is indistinguishable at read time from "cooked by a build that
	/// predates stamps" and both are treated as not-provably-fresh.
	bool WriteCookedMesh(const std::filesystem::path& path, const CookedMesh& mesh,
	                     const CookedSourceStamp* stamp = nullptr);

	/// Read a cooked mesh back. Returns std::nullopt and logs if the file is
	/// missing, truncated, corrupt (payload hash mismatch), written by a
	/// different format version, or built against a different struct layout.
	std::optional<CookedMesh> ReadCookedMesh(const std::filesystem::path& path);

	/// Read ONLY the stamp out of a cooked file. Returns std::nullopt when the
	/// file is unreadable/malformed OR when it simply carries no stamp; the two
	/// are distinguished by CheckCookedMeshFreshness, which is what callers
	/// should use.
	///
	/// DOES NOT VERIFY THE PAYLOAD HASH, and reads only the header, the section
	/// table and the stamp itself rather than the whole file. This runs on every
	/// mesh import in cooked mode, and hashing hundreds of megabytes to decide
	/// whether it is worth reading hundreds of megabytes would make the fast
	/// path slower than the importer it replaces. The hash still runs, in
	/// ReadCookedMesh, before any of those bytes reach a buffer -- so a corrupt
	/// stamp costs a wasted freshness check, never a bad load.
	std::optional<CookedSourceStamp> ReadCookedSourceStamp(const std::filesystem::path& path);

	/// The answer to "may I load this cooked file instead of importing its
	/// source". Only Fresh means yes; everything else is a logged fallback.
	enum class CookedFreshness {
		Fresh,          ///< stamp present and matches the source on disk
		NoCookedFile,   ///< no .x3mesh there at all -- the ordinary case, not an error
		OlderThanSource,///< the cooked file's own mtime predates the source's
		NoStamp,        ///< readable, but cooked without a stamp or by a build that had none
		Stale,          ///< stamp present and disagrees with the source
		Unreadable,     ///< there is a file and it is not a cooked mesh this build can parse
	};
	const char* CookedFreshnessToString(CookedFreshness freshness);

	/// Compare `cookedPath` against `sourcePath`. `why` (if non-null) receives a
	/// human-readable reason for any answer other than Fresh, for the log line
	/// the caller writes when it falls back.
	///
	/// NEVER THROWS AND NEVER ASSERTS: every filesystem call goes through an
	/// error_code, and a missing source is Unreadable rather than a question
	/// this function is entitled to answer.
	CookedFreshness CheckCookedMeshFreshness(const std::filesystem::path& cookedPath,
	                                         const std::filesystem::path& sourcePath,
	                                         std::string* why = nullptr);

	/// The SEMANTIC half of validating an untrusted cooked file, as opposed to
	/// the framing half ReadCookedMesh does.
	///
	/// ReadCookedMesh proves the file is intact: it is the right format, the
	/// right struct layout, its sections fit inside it, and its payload hashes
	/// to what the header claims. None of that says the mesh MEANS anything. A
	/// perfectly intact file can hold a triangle indexing vertex 0xFFFFFFFF, a
	/// BVH interior node whose child is its own index, or a submesh range
	/// running off the end of the triangle list -- and every one of those is
	/// uploaded verbatim to the GPU, where the first reads unmapped memory, the
	/// second is an infinite loop in the traversal shader and the third is a
	/// draw call over someone else's triangles.
	///
	/// So: every index a cooked file contains is checked against the array it
	/// indexes, once, here, at the boundary. Returns false and fills `whyNot`
	/// rather than asserting -- this is the load path of a shipped runtime
	/// reading a file it did not write.
	bool CookedMeshIsSelfConsistent(const CookedMesh& mesh, std::string* whyNot = nullptr);

	/// Pull one mesh asset out of a loaded AssetPool into cookable form.
	///
	/// Uses the pool's public surface only: the caller imports through
	/// AssetManager::ImportAsset (the existing decode path, unmodified) and
	/// hands the resulting pool and GUID here. Returns std::nullopt if the GUID
	/// is not a mesh or if any of its recorded ranges falls outside the pool's
	/// buffers -- the latter is a corrupt-pool assertion, not a user error, and
	/// is checked because silently slicing the wrong window would produce a
	/// cooked file that is wrong rather than absent.
	std::optional<CookedMesh> ExtractCookedMesh(const AssetPool& pool, LR_GUID guid);

	/// Byte-exact comparison of two CookedMeshes, as the round-trip gate.
	///
	/// The array comparisons are memcmp, NOT element-wise value comparison. That
	/// is deliberate: the arrays are full of floats, and `NaN != NaN`, so a
	/// value compare would report a difference for a degenerate mesh that round
	/// tripped perfectly -- and, worse, would report EQUALITY for +0.0 vs -0.0,
	/// which are different bytes. The claim this function makes is "the bytes
	/// came back", which is the only claim a serializer round trip can honestly
	/// make.
	///
	/// On mismatch, `whatDiffers` (if non-null) receives the first difference
	/// found, named by section.
	bool CookedMeshBytesEqual(const CookedMesh& a, const CookedMesh& b,
	                          std::string* whatDiffers = nullptr);

}
