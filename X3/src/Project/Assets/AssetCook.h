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
// them land later without a rewrite (see "EXTENDING" below). A half-integrated
// cook step is worse than none, so nothing here is wired into the engine's load
// path yet -- X3AssetCook writes files, and nothing reads them but X3AssetCook.
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
//
// NOT A CACHE KEY. Nothing here records a hash of the source model, so nothing
// here can tell you a cooked file is stale with respect to its input. That
// belongs in ProjectExporter's cook step where the source path is known and
// meaningful; a mesh loaded from a .x3mesh has no source.
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

	/// Serialize `mesh` to `path` (conventionally *.x3mesh).
	///
	/// Writes to a sibling temp file and renames on success, so an interrupted
	/// cook leaves either the previous file or nothing -- never a truncated one
	/// that a later build would happily treat as valid. Returns false and logs
	/// on any I/O failure; never throws.
	bool WriteCookedMesh(const std::filesystem::path& path, const CookedMesh& mesh);

	/// Read a cooked mesh back. Returns std::nullopt and logs if the file is
	/// missing, truncated, corrupt (payload hash mismatch), written by a
	/// different format version, or built against a different struct layout.
	std::optional<CookedMesh> ReadCookedMesh(const std::filesystem::path& path);

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
