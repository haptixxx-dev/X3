#pragma once

// =============================================================================
// LightmapUV -- the second UV set a lightmap bake writes into, and the two
// seam fixes without which a bake looks broken no matter how good the tracer is.
//
// ENGINE_PLAN.md Phase 10a says "integrate xatlas". THIS IS NOT XATLAS, and it
// is not trying to be. xatlas is not vendored in X3/libs and cannot be fetched
// here, so this is a deliberately simple unwrapper that exists so the rest of
// 10a -- ray generation from texels, atlas packing, dilation, the job-system
// parallel bake -- can be built and tested against something real. The interface
// is the part meant to survive; the parameterisation is the part meant to be
// replaced. See "QUALITY LIMITS VERSUS XATLAS" below before shipping a bake with
// it.
//
// WHY UNWRAPPING IS NEEDED AT ALL. A lightmap stores incoming light per surface
// point, so every surface point in the scene needs its OWN texel. The mesh's
// authored UV set does not provide that: artists tile, mirror and overlap UVs
// freely because a base-colour texture is happy to repeat. Bake irradiance into
// a tiled UV set and the far wall's light lands on the near wall. Hence a
// second, injective UV set, generated rather than authored.
//
// -----------------------------------------------------------------------------
// WHY THE OUTPUT IS PER-TRIANGLE-CORNER AND NOT PER-VERTEX
// -----------------------------------------------------------------------------
// Charting cuts the surface. A vertex on a chart boundary belongs to two charts
// that land in completely different places in the atlas, so it needs two
// different UVs -- and the cube is the smallest example: its 8 corner vertices
// are each shared by 3 faces that become 3 charts. There is no per-vertex array
// that can express that without first splitting the mesh's vertices, which would
// mean rewriting Gpu::Vertex/Gpu::TriRef, invalidating every index in the BVH
// and every firstVertexIdx in every MeshEntityHandle. This module refuses to do
// that: it returns 3 UVs per triangle plus the triangle->chart mapping, and
// leaves the decision of whether to split vertices (Phase 9's cook step, where
// meshoptimizer already re-lays-out the buffers) to the code that owns them.
//
//   cornerUV[3 * localTri + 0] belongs to tris[firstTri + localTri].i0
//   cornerUV[3 * localTri + 1] belongs to ... .i1
//   cornerUV[3 * localTri + 2] belongs to ... .i2
//
// -----------------------------------------------------------------------------
// QUALITY LIMITS VERSUS XATLAS -- read before trusting a bake
// -----------------------------------------------------------------------------
//   * PLANAR PROJECTION, NO PARAMETERISATION. Each chart is projected flat onto
//     its mean normal. xatlas runs LSCM/ABF and gets a low-distortion
//     parameterisation of a curved chart; here, distortion is bounded only by
//     how tight the chart normal cone is, so texel density varies within a chart
//     by up to 1/cos(2*maxChartAngle). Tightening the cone fixes the distortion
//     and multiplies the chart count.
//   * NO OVERLAP TEST INSIDE A CHART. The normal cone makes each individual
//     triangle project with positive area, but it does not stop a folded or
//     spiralling region from projecting on top of itself -- and two surface
//     points sharing a texel is exactly the failure the whole second UV set
//     exists to prevent. The cone makes it unlikely, not impossible. xatlas
//     detects it and splits.
//   * BOUNDING BOXES ARE PACKED, NOT CHART OUTLINES. A triangle fan that fills
//     half its bounding box wastes the other half. xatlas rasterises the chart
//     and packs the actual footprint, which is most of why its atlases are so
//     much denser. Expect roughly half the usable resolution here for the same
//     atlas size.
//   * NO CHART ROTATION, NO CHART MERGING, NO MULTI-RESOLUTION SEARCH. Charts go
//     in at their natural orientation, and the atlas is a fixed square that the
//     texel density is shrunk to fit.
//   * SEAM PLACEMENT IS WHEREVER THE NORMAL CONE HAPPENS TO BREAK, not where a
//     seam would be least visible.
//
// Consequence: more charts, more seams, a bigger atlas for the same world-space
// texel density. All of it is contained behind GenerateLightmapUVs; nothing
// downstream needs to change when xatlas replaces the body.
// =============================================================================

#include "lrpch.h"
#include "Renderer/GpuTypes.h"

#include <vector>

namespace X3
{

	/// Written into LightmapUV::triangleChart for a triangle that received no
	/// chart. Only possible when generation failed outright (ok == false); a
	/// successful run assigns every triangle, including degenerate ones.
	inline constexpr uint32_t LIGHTMAP_NO_CHART = 0xFFFFFFFFu;

	struct LightmapUVSettings
	{
		/// Side length of the square atlas in texels. Square because the packer
		/// is a skyline packer over a fixed bin, and a non-square bin gives it a
		/// preferred axis for no benefit.
		uint32_t atlasResolution = 512;

		/// THE GUTTER, in texels, applied on all four sides of every chart when
		/// packing.
		///
		/// WHAT HAPPENS AT ZERO -- this is the single most visible lightmap
		/// artefact and the reason this parameter exists. The GPU samples a
		/// lightmap bilinearly, so shading a point near a chart's edge blends
		/// that chart's edge texel with the texel NEXT to it in the atlas. With
		/// no gutter that neighbour belongs to a different chart -- a different
		/// piece of surface, lit completely differently. The result is a hard
		/// bright or dark line traced along every single chart boundary in the
		/// scene: a glowing wireframe of the charting, drawn over the geometry.
		/// It is unmistakable, it is everywhere, and it does not go away with
		/// more bake samples because it is not noise.
		///
		/// 2 is the minimum that survives bilinear filtering at mip 0. Each mip
		/// level halves the gutter in texel terms, so a lightmap sampled with N
		/// mips needs gutterTexels >= 2 << N to stay clean; lightmaps are
		/// usually sampled without mips for exactly this reason.
		///
		/// The gutter reserves the space. DilateLightmap fills it -- both halves
		/// are required, and neither works alone.
		uint32_t gutterTexels = 2;

		/// Maximum angle between a chart's seed face normal and any face in it.
		///
		/// CLAMPED TO [1, 44] DEGREES, and 44 is not arbitrary. The chart is
		/// projected along its AREA-WEIGHTED MEAN normal, which lies within this
		/// angle of the seed (a normalised weighted sum of vectors inside a
		/// convex cone stays inside it), so any face can be up to 2x this angle
		/// away from the projection axis. At 2x >= 90 degrees a face projects to
		/// zero or NEGATIVE area -- it collapses to a line, or folds over its own
		/// chart with reversed winding -- and its texels either do not exist or
		/// belong to something else. Staying under 45 is what makes "every
		/// non-degenerate triangle has positive UV area" a guarantee rather than
		/// a hope.
		float maxChartAngleDegrees = 40.0f;

		/// World units per texel: the bake's spatial resolution. 0 means auto --
		/// pick the finest density whose charts still fit the atlas.
		///
		/// ONE SCALE FOR EVERY CHART, deliberately. Scaling charts individually
		/// to fill the atlas better would give neighbouring surfaces different
		/// lightmap resolutions, and the resolution change is visible as a
		/// sharpness discontinuity running along the seam between them.
		float worldUnitsPerTexel = 0.0f;

		/// Fraction of the atlas the auto-fit aims to fill with chart CONTENT on
		/// its first attempt, before gutters and packing waste. Below 1 because
		/// bounding-box packing wastes an input-dependent amount that has no
		/// closed form -- the first estimate is a starting point for the retry
		/// loop, not a prediction.
		float targetAtlasUsage = 0.55f;

		/// How many times the auto-fit may coarsen the texel density and re-pack
		/// before giving up. Each attempt multiplies worldUnitsPerTexel by 1.15,
		/// so 64 attempts span a factor of ~7000 in density -- far more than any
		/// real mesh needs, and a bound so a pathological input cannot hang the
		/// cook.
		uint32_t maxFitAttempts = 64;
	};

	/// One connected, near-planar group of triangles and the atlas rectangle it
	/// was packed into.
	struct LightmapChart
	{
		/// Area-weighted mean of the member face normals: the projection axis.
		glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
		/// Orthonormal basis of the projection plane. (tangent, bitangent,
		/// normal) is right-handed, which is what makes projected winding -- and
		/// therefore signed UV area -- match world winding.
		glm::vec3 tangent{ 1.0f, 0.0f, 0.0f };
		glm::vec3 bitangent{ 0.0f, 1.0f, 0.0f };

		/// World-space point the projection is measured from -- the first corner
		/// of the chart's first triangle, not the world origin. Projecting
		/// absolute world coordinates would spend all of a float's precision on
		/// the offset for a mesh placed a few kilometres out, and the chart's own
		/// extent is what the packer needs to resolve.
		glm::vec3 origin{ 0.0f };

		/// Chart bounds in the projection plane, world units, relative to
		/// `origin`.
		glm::vec2 projMin{ 0.0f };
		glm::vec2 projMax{ 0.0f };

		/// PADDED rectangle in the atlas, texels: the chart's content grown by
		/// gutterTexels on every side. These are the rectangles the packer keeps
		/// disjoint, and keeping the PADDED ones disjoint is what guarantees two
		/// charts' content is separated by at least 2 * gutterTexels.
		uint32_t x = 0, y = 0, width = 0, height = 0;

		uint32_t triangleCount = 0;

		/// The content rectangle: where UVs actually land.
		uint32_t innerX(uint32_t gutter) const { return x + gutter; }
		uint32_t innerY(uint32_t gutter) const { return y + gutter; }
		uint32_t innerWidth(uint32_t gutter) const { return width - 2u * gutter; }
		uint32_t innerHeight(uint32_t gutter) const { return height - 2u * gutter; }
	};

	struct LightmapUV
	{
		/// 3 per triangle, in i0/i1/i2 order. See the per-corner rationale at the
		/// top of this file.
		std::vector<glm::vec2> cornerUV;
		/// 1 per triangle: index into `charts`, or LIGHTMAP_NO_CHART.
		std::vector<uint32_t> triangleChart;
		std::vector<LightmapChart> charts;

		uint32_t atlasResolution = 0;
		uint32_t gutterTexels = 0;
		/// The density actually used -- the auto-fit's answer, and what the bake
		/// needs in order to know what a texel is worth in world terms.
		float worldUnitsPerTexel = 0.0f;
		/// maxChartAngleDegrees AFTER clamping. The guarantee that every
		/// non-degenerate triangle has positive UV area is stated in terms of
		/// this value, not the requested one.
		float maxChartAngleDegrees = 0.0f;

		/// False only if packing failed (gutter wider than the atlas, or the
		/// retry budget exhausted). An empty triangle range succeeds trivially.
		bool ok = false;
	};

	/// Unwraps [firstTri, firstTri + triCount) of `tris` into a lightmap UV set.
	///
	/// `tris` indexes `vertices` GLOBALLY, matching Gpu::TriRef's contract, so
	/// this takes the same (buffer, range) shape as MeshUtils::ComputeTangents
	/// and can be called on one submesh or a whole mesh.
	///
	/// GEOMETRIC FACE NORMALS ARE USED, never Gpu::Vertex::normalV. Shading
	/// normals lie on purpose -- a smooth-shaded cube's vertex normals point into
	/// the corners so the lighting looks round -- and charting on them would grow
	/// one chart around the entire cube and then project it flat. Charting is a
	/// question about the surface, not about how it is shaded.
	///
	/// Pure and deterministic: same input, byte-identical output. No logging, no
	/// allocation of engine state, no GPU. That is what lets X3LightmapTest assert
	/// on it with no display and no device.
	LightmapUV GenerateLightmapUVs(const std::vector<Gpu::Vertex>& vertices,
	                               const std::vector<Gpu::TriRef>& tris,
	                               uint32_t firstTri, uint32_t triCount,
	                               const LightmapUVSettings& settings = {});

	/// THE OTHER HALF OF THE SEAM FIX. Floods lit texels outward into empty ones,
	/// one ring per pass, and returns how many texels were newly filled.
	///
	/// WHY A GUTTER IS NOT ENOUGH ON ITS OWN. The gutter reserves empty space
	/// around a chart; it does not put anything in it. Two things then sample
	/// that empty space. First, bilinear filtering at the chart's own edge blends
	/// the edge texel with the reserved empty texel outside it, so an unlit
	/// gutter darkens every chart border into a black outline -- the same
	/// artefact the gutter was added to prevent, just a different neighbour.
	/// Second, and worse, a texel whose CENTRE falls outside the triangle but
	/// whose area is partially covered gets no bake sample at all, yet
	/// interpolated UVs still land on it; every chart edge therefore has a fringe
	/// of black texels INSIDE the content rectangle. Dilating outward from the
	/// covered set fills both.
	///
	/// `texels` and `coverage` are both width*height, row-major. A covered texel
	/// is coverage != 0. Newly filled texels take the average of their COVERED
	/// 8-neighbours and are marked covered afterwards.
	///
	///   * 8-neighbours, not 4: with a 4-neighbourhood a texel touching the
	///     covered set only diagonally never fills, leaving a black pixel at
	///     every convex corner of every chart -- and corners are where the eye
	///     goes.
	///   * The covered set is SNAPSHOTTED per pass, so a texel filled during pass
	///     N cannot seed further growth until pass N+1. Without the snapshot the
	///     flood would run further in the scan direction than against it, and the
	///     gutter would be filled asymmetrically depending on where the chart sat
	///     in the atlas.
	///   * Already-covered texels are NEVER written. Dilation only ever adds; it
	///     cannot corrupt a baked value.
	///
	/// `passes` should be at least the gutter width -- filling less than the
	/// gutter leaves the outer part of it black, which is the artefact above.
	uint32_t DilateLightmap(std::vector<glm::vec4>& texels,
	                        std::vector<uint8_t>& coverage,
	                        uint32_t width, uint32_t height,
	                        uint32_t passes);

}
