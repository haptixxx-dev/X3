#include "Project/Assets/LightmapUV.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace X3
{

	namespace {

		// A face whose cross product is shorter than this is treated as having no
		// normal at all. It is an ABSOLUTE floor, not a relative tolerance,
		// because its only job is to keep the normalise below from producing
		// infinities -- a degenerate face's normal is not "imprecise", it does not
		// exist, and any answer normalise() invents would then be fed into the
		// chart cone test as if it meant something.
		constexpr float kDegenerateCross = 1e-20f;

		struct Face {
			glm::vec3 normal{ 0.0f };
			float     area = 0.0f;       // 0 exactly for a degenerate face
			bool      degenerate = true;
		};

		// -------------------------------------------------------------------------
		// SKYLINE (bottom-left) BIN PACKER over a fixed square bin.
		//
		// The "shelf/skyline" family is the right complexity point here. A pure
		// shelf packer rounds every rectangle up to its shelf's height and wastes
		// the difference, which for lightmap charts -- wildly varying aspect
		// ratios -- is most of the atlas. A skyline packer tracks the actual
		// upper contour and drops each rectangle into the lowest place it fits,
		// so a short chart can sit beside a tall one instead of under it, for
		// about thirty lines more code and no data structure beyond a sorted
		// vector.
		//
		// NON-OVERLAP IS STRUCTURAL, not checked. Every rectangle is placed with
		// its bottom edge ON the contour and the contour is then raised across
		// exactly the span it occupies, so no later rectangle can be placed below
		// it within that span. That property is the packer's entire reason to
		// exist -- two charts sharing texels means one surface's baked light
		// appears on another -- so X3LightmapTest asserts it independently rather
		// than trusting this comment.
		// -------------------------------------------------------------------------
		struct SkylineNode { uint32_t x, y, width; };

		class SkylinePacker
		{
		public:
			explicit SkylinePacker(uint32_t binSize) : m_Bin(binSize) {
				m_Nodes.push_back({ 0u, 0u, binSize });
			}

			// Lowest-then-leftmost placement. Lowest first because it keeps the
			// contour flat, and a flat contour is what lets the next rectangle
			// span several nodes without being lifted by the tallest of them.
			bool Place(uint32_t w, uint32_t h, uint32_t& outX, uint32_t& outY) {
				size_t bestIdx = SIZE_MAX;
				uint32_t bestX = 0, bestY = std::numeric_limits<uint32_t>::max();

				for (size_t i = 0; i < m_Nodes.size(); ++i) {
					const uint32_t x = m_Nodes[i].x;
					if (x + w > m_Bin) continue;

					// The rectangle rests on the HIGHEST node it spans; resting on
					// anything lower would overlap that node's rectangle.
					uint32_t y = m_Nodes[i].y;
					uint32_t remaining = w;
					size_t j = i;
					bool fits = true;
					while (remaining > 0) {
						if (j >= m_Nodes.size()) { fits = false; break; }
						y = std::max(y, m_Nodes[j].y);
						if (y + h > m_Bin) { fits = false; break; }
						remaining = (remaining > m_Nodes[j].width) ? remaining - m_Nodes[j].width : 0u;
						++j;
					}
					if (!fits) continue;

					if (y < bestY || (y == bestY && x < bestX)) {
						bestY = y; bestX = x; bestIdx = i;
					}
				}

				if (bestIdx == SIZE_MAX) return false;

				AddLevel(bestIdx, bestX, bestY, w, h);
				outX = bestX;
				outY = bestY;
				return true;
			}

		private:
			void AddLevel(size_t idx, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
				m_Nodes.insert(m_Nodes.begin() + static_cast<ptrdiff_t>(idx),
				               SkylineNode{ x, y + h, w });

				// Clip whatever the new node now covers. Only the nodes
				// immediately after it can be affected, and the loop stops at the
				// first one that is clear of it.
				for (size_t i = idx + 1; i < m_Nodes.size();) {
					const uint32_t prevRight = m_Nodes[i - 1].x + m_Nodes[i - 1].width;
					if (m_Nodes[i].x >= prevRight) break;

					const uint32_t shrink = prevRight - m_Nodes[i].x;
					if (m_Nodes[i].width <= shrink) {
						m_Nodes.erase(m_Nodes.begin() + static_cast<ptrdiff_t>(i));
						continue;   // fully swallowed; the next node may be too
					}
					m_Nodes[i].x += shrink;
					m_Nodes[i].width -= shrink;
					break;
				}

				// Merge equal-height neighbours. Without this the contour
				// fragments into thousands of one-texel nodes and Place() -- which
				// is linear in node count per candidate -- goes quadratic on the
				// chart count.
				for (size_t i = 0; i + 1 < m_Nodes.size();) {
					if (m_Nodes[i].y == m_Nodes[i + 1].y) {
						m_Nodes[i].width += m_Nodes[i + 1].width;
						m_Nodes.erase(m_Nodes.begin() + static_cast<ptrdiff_t>(i + 1));
					} else {
						++i;
					}
				}
			}

			uint32_t m_Bin;
			std::vector<SkylineNode> m_Nodes;
		};

		// Deterministic orthonormal basis for a plane. The reference axis is
		// chosen away from the normal so the cross product is never near-zero;
		// picking a fixed reference instead would make the tangent direction --
		// and therefore every UV in the chart -- explode for normals that happen
		// to be parallel to it.
		void BuildBasis(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
			const glm::vec3 ref = (std::abs(n.z) < 0.9f) ? glm::vec3(0.0f, 0.0f, 1.0f)
			                                             : glm::vec3(1.0f, 0.0f, 0.0f);
			t = glm::normalize(glm::cross(ref, n));
			// (t, b, n) right-handed: cross(t, b) == n. That is what makes a
			// triangle's projected winding match its world winding, and therefore
			// its signed UV area positive whenever its face normal is within 90
			// degrees of n.
			b = glm::cross(n, t);
		}

	} // namespace

	LightmapUV GenerateLightmapUVs(const std::vector<Gpu::Vertex>& vertices,
	                               const std::vector<Gpu::TriRef>& tris,
	                               uint32_t firstTri, uint32_t triCount,
	                               const LightmapUVSettings& settings)
	{
		LightmapUV out;
		out.atlasResolution = std::max(1u, settings.atlasResolution);
		out.gutterTexels    = settings.gutterTexels;
		// See LightmapUVSettings::maxChartAngleDegrees for why 44 is the ceiling:
		// above it the projection axis can be 90 degrees or more from a member
		// face and that face's texels stop existing.
		out.maxChartAngleDegrees = glm::clamp(settings.maxChartAngleDegrees, 1.0f, 44.0f);

		out.cornerUV.assign(size_t(triCount) * 3u, glm::vec2(0.0f));
		out.triangleChart.assign(triCount, LIGHTMAP_NO_CHART);

		if (triCount == 0) { out.ok = true; return out; }

		// A chart needs at least one content texel plus a gutter on both sides.
		// Failing here rather than clamping the gutter is deliberate: silently
		// shrinking the gutter would produce an atlas that looks fine in the
		// editor and grows a bright seam along every chart boundary once it is
		// sampled bilinearly, which is a far more expensive thing to discover.
		if (2u * out.gutterTexels + 1u > out.atlasResolution) return out;   // ok stays false

		if (firstTri > tris.size() || triCount > tris.size() - firstTri) return out;

		// ---------------------------------------------------------------------
		// 1. Face geometry. GEOMETRIC normals only -- see the header.
		// ---------------------------------------------------------------------
		std::vector<glm::vec3> corner(size_t(triCount) * 3u, glm::vec3(0.0f));
		std::vector<Face> faces(triCount);

		for (uint32_t t = 0; t < triCount; ++t) {
			const Gpu::TriRef& tri = tris[firstTri + t];
			// An out-of-range index is treated as a degenerate face rather than
			// as a crash. This runs inside the cook over whatever the importer
			// produced, and one bad triangle must not take the whole bake down.
			if (tri.i0 >= vertices.size() || tri.i1 >= vertices.size() || tri.i2 >= vertices.size())
				continue;

			const glm::vec3 p0 = glm::vec3(vertices[tri.i0].positionU);
			const glm::vec3 p1 = glm::vec3(vertices[tri.i1].positionU);
			const glm::vec3 p2 = glm::vec3(vertices[tri.i2].positionU);
			corner[size_t(t) * 3u + 0] = p0;
			corner[size_t(t) * 3u + 1] = p1;
			corner[size_t(t) * 3u + 2] = p2;

			const glm::vec3 c = glm::cross(p1 - p0, p2 - p0);
			const float len = glm::length(c);
			// Written as !(len > eps) so a NaN position -- which produces a NaN
			// length, and NaN > eps is false -- lands in the degenerate branch
			// instead of poisoning a chart normal.
			if (!(len > kDegenerateCross)) continue;

			faces[t].normal     = c / len;
			faces[t].area       = 0.5f * len;
			faces[t].degenerate = false;
		}

		// ---------------------------------------------------------------------
		// 2. Edge adjacency, keyed on the pair of GLOBAL vertex indices.
		//
		// Two triangles are neighbours only if they share an index, not merely a
		// position. That is conservative in exactly the safe direction: a mesh
		// whose vertices were already split (a UV seam, a hard normal crease)
		// yields more, smaller charts -- more seams to dilate, never a chart that
		// folds across a crease it could not see.
		// ---------------------------------------------------------------------
		std::unordered_map<uint64_t, std::vector<uint32_t>> edges;
		edges.reserve(size_t(triCount) * 3u);

		auto edgeKey = [](uint32_t a, uint32_t b) -> uint64_t {
			const uint32_t lo = std::min(a, b), hi = std::max(a, b);
			return (uint64_t(lo) << 32) | uint64_t(hi);
		};

		for (uint32_t t = 0; t < triCount; ++t) {
			const Gpu::TriRef& tri = tris[firstTri + t];
			if (tri.i0 >= vertices.size() || tri.i1 >= vertices.size() || tri.i2 >= vertices.size())
				continue;
			edges[edgeKey(tri.i0, tri.i1)].push_back(t);
			edges[edgeKey(tri.i1, tri.i2)].push_back(t);
			edges[edgeKey(tri.i2, tri.i0)].push_back(t);
		}

		// ---------------------------------------------------------------------
		// 3. Region grow. Breadth-first from the lowest unassigned triangle.
		//
		// EVERY CANDIDATE IS COMPARED TO THE SEED, NOT TO ITS NEIGHBOUR. Comparing
		// against the neighbour would let the normal drift by the threshold at
		// every step, so a cylinder's charts would wrap the whole way round and
		// project onto themselves -- the two halves of the cylinder landing on the
		// same texels, so the lit side's irradiance is baked onto the dark side.
		// Seeding the cone bounds the chart's total curvature instead of its local
		// curvature, which is the property the flat projection actually needs.
		//
		// Breadth-first rather than depth-first because BFS grows a chart
		// radially. DFS snakes along the surface and produces long thin charts
		// whose bounding boxes -- which is what gets packed -- are almost entirely
		// empty.
		// ---------------------------------------------------------------------
		const float cosCone = std::cos(glm::radians(out.maxChartAngleDegrees));

		std::vector<uint32_t> queue;
		queue.reserve(triCount);

		for (uint32_t seed = 0; seed < triCount; ++seed) {
			if (out.triangleChart[seed] != LIGHTMAP_NO_CHART) continue;

			const uint32_t chartIdx = uint32_t(out.charts.size());
			out.charts.emplace_back();

			// A degenerate seed has no normal. It gets an arbitrary one and, being
			// zero-area, contributes nothing to the mean; the cone test below only
			// ever admits other degenerate faces to it, which is harmless because
			// none of them own any texels.
			const glm::vec3 seedNormal = faces[seed].degenerate ? glm::vec3(0.0f, 0.0f, 1.0f)
			                                                    : faces[seed].normal;

			glm::vec3 normalSum(0.0f);
			uint32_t  triangles = 0;

			queue.clear();
			queue.push_back(seed);
			out.triangleChart[seed] = chartIdx;

			for (size_t head = 0; head < queue.size(); ++head) {
				const uint32_t t = queue[head];
				++triangles;
				normalSum += faces[t].normal * faces[t].area;   // zero for degenerates

				const Gpu::TriRef& tri = tris[firstTri + t];
				if (tri.i0 >= vertices.size() || tri.i1 >= vertices.size() || tri.i2 >= vertices.size())
					continue;

				const uint64_t keys[3] = {
					edgeKey(tri.i0, tri.i1), edgeKey(tri.i1, tri.i2), edgeKey(tri.i2, tri.i0)
				};

				for (const uint64_t key : keys) {
					auto it = edges.find(key);
					if (it == edges.end()) continue;
					// Insertion order is triangle order and lookups never depend on
					// the hash map's own ordering, so the growth order -- and every
					// UV that follows from it -- is deterministic.
					for (const uint32_t n : it->second) {
						if (out.triangleChart[n] != LIGHTMAP_NO_CHART) continue;
						// Degenerate faces are absorbed unconditionally: they have
						// no normal to test and no area to distort the chart with,
						// and leaving them as singleton charts would spend a whole
						// padded atlas rectangle on a triangle with no texels.
						if (!faces[n].degenerate &&
						    glm::dot(faces[n].normal, seedNormal) < cosCone) continue;

						out.triangleChart[n] = chartIdx;
						queue.push_back(n);
					}
				}
			}

			LightmapChart& chart = out.charts[chartIdx];
			chart.triangleCount = triangles;

			// AREA-WEIGHTED MEAN, not the seed: projecting along the mean halves
			// the worst-case angle between the axis and a member face, and the
			// area weight stops a swarm of slivers from dragging the axis away
			// from the surface the chart is mostly made of.
			chart.normal = (glm::length(normalSum) > kDegenerateCross)
			             ? glm::normalize(normalSum)
			             : seedNormal;
			BuildBasis(chart.normal, chart.tangent, chart.bitangent);
			chart.origin = corner[size_t(seed) * 3u + 0];
		}

		// ---------------------------------------------------------------------
		// 4. Project every corner into its chart's plane and take the bounds.
		//
		// Cached rather than recomputed at emit time so the UVs written out are
		// derived from exactly the numbers the bounds were taken from. Recomputing
		// would give the same answer today and would silently stop doing so the
		// moment anything here is reordered or vectorised.
		// ---------------------------------------------------------------------
		std::vector<glm::vec2> projected(size_t(triCount) * 3u, glm::vec2(0.0f));

		// Seeded to the inverted-infinite box. Every chart is created by a seed
		// triangle and therefore has at least one triangle to bound it, so no
		// chart can escape this loop still holding the sentinel.
		for (auto& c : out.charts) {
			c.projMin = glm::vec2(std::numeric_limits<float>::max());
			c.projMax = glm::vec2(std::numeric_limits<float>::lowest());
		}

		for (uint32_t t = 0; t < triCount; ++t) {
			const uint32_t ci = out.triangleChart[t];
			LightmapChart& c = out.charts[ci];
			for (uint32_t k = 0; k < 3; ++k) {
				const glm::vec3 d = corner[size_t(t) * 3u + k] - c.origin;
				const glm::vec2 p(glm::dot(d, c.tangent), glm::dot(d, c.bitangent));
				projected[size_t(t) * 3u + k] = p;
				c.projMin = glm::min(c.projMin, p);
				c.projMax = glm::max(c.projMax, p);
			}
		}

		// ---------------------------------------------------------------------
		// 5. Fit a texel density and pack.
		//
		// The first density is an area estimate and the loop coarsens it until the
		// charts fit. There is no closed form for how much a bounding-box skyline
		// pack wastes -- it depends on the aspect ratios and the order they arrive
		// in -- so the only honest answer is to try, and to bound the trying.
		// ---------------------------------------------------------------------
		double contentArea = 0.0;
		for (const auto& c : out.charts) {
			const glm::vec2 ext = glm::max(c.projMax - c.projMin, glm::vec2(0.0f));
			contentArea += double(ext.x) * double(ext.y);
		}

		const float usage = glm::clamp(settings.targetAtlasUsage, 0.05f, 1.0f);
		double wpt = (settings.worldUnitsPerTexel > 0.0f)
		           ? double(settings.worldUnitsPerTexel)
		           : ((contentArea > 0.0)
		              ? std::sqrt(contentArea / (double(usage) * double(out.atlasResolution) * double(out.atlasResolution)))
		              : 1.0);
		if (!(wpt > 0.0) || !std::isfinite(wpt)) wpt = 1.0;

		// An explicit worldUnitsPerTexel is a request, not a contract: if the
		// charts do not fit at that density the alternative to coarsening is
		// emitting UVs outside [0,1], which wraps and bakes one chart's light on
		// top of another. Coarsening is a resolution loss; wrapping is corruption.
		const uint32_t attempts = std::max(1u, settings.maxFitAttempts);
		const uint32_t pad = 2u * out.gutterTexels;

		struct PackItem { uint32_t chart, w, h; };
		std::vector<PackItem> items;
		items.reserve(out.charts.size());

		for (uint32_t attempt = 0; attempt < attempts; ++attempt, wpt *= 1.15) {
			items.clear();
			bool sizesValid = true;

			for (uint32_t ci = 0; ci < out.charts.size(); ++ci) {
				const glm::vec2 ext = glm::max(out.charts[ci].projMax - out.charts[ci].projMin, glm::vec2(0.0f));
				// Computed in double and range-checked BEFORE the cast: a chart
				// far wider than the atlas would otherwise wrap around uint32 and
				// come out as a rectangle that "fits".
				const double dw = std::ceil(double(ext.x) / wpt);
				const double dh = std::ceil(double(ext.y) / wpt);
				if (!std::isfinite(dw) || !std::isfinite(dh) ||
				    dw > double(out.atlasResolution) || dh > double(out.atlasResolution)) {
					sizesValid = false;
					break;
				}
				// At least one content texel. A chart of exactly zero extent (one
				// degenerate triangle) still needs somewhere to point its UVs.
				const uint32_t w = std::max(1u, uint32_t(dw)) + pad;
				const uint32_t h = std::max(1u, uint32_t(dh)) + pad;
				if (w > out.atlasResolution || h > out.atlasResolution) { sizesValid = false; break; }
				items.push_back({ ci, w, h });
			}
			if (!sizesValid) continue;

			// Tallest first. Skyline packing is order-sensitive and height-descending
			// is the standard heuristic: placing a tall rectangle late means finding
			// a tall gap late, and there usually is not one. Ties broken by width
			// then by chart index so the pack is a pure function of the input.
			std::stable_sort(items.begin(), items.end(), [](const PackItem& a, const PackItem& b) {
				if (a.h != b.h) return a.h > b.h;
				if (a.w != b.w) return a.w > b.w;
				return a.chart < b.chart;
			});

			SkylinePacker packer(out.atlasResolution);
			bool packed = true;
			for (const PackItem& it : items) {
				uint32_t px = 0, py = 0;
				if (!packer.Place(it.w, it.h, px, py)) { packed = false; break; }
				LightmapChart& c = out.charts[it.chart];
				c.x = px; c.y = py; c.width = it.w; c.height = it.h;
			}
			if (!packed) continue;

			out.worldUnitsPerTexel = float(wpt);
			out.ok = true;
			break;
		}

		if (!out.ok) return out;

		// ---------------------------------------------------------------------
		// 6. Emit UVs.
		//
		// Continuous texel coordinates, normalised by the resolution: texel k
		// covers [k/res, (k+1)/res], which is the convention a bake that iterates
		// texels and a shader that samples them both assume. Offsetting by half a
		// texel here would shift every baked value half a texel against the
		// geometry it belongs to.
		// ---------------------------------------------------------------------
		const float invRes = 1.0f / float(out.atlasResolution);
		const float invWpt = float(1.0 / wpt);

		for (uint32_t t = 0; t < triCount; ++t) {
			const LightmapChart& c = out.charts[out.triangleChart[t]];
			const float baseX = float(c.innerX(out.gutterTexels));
			const float baseY = float(c.innerY(out.gutterTexels));

			for (uint32_t k = 0; k < 3; ++k) {
				const glm::vec2 rel = projected[size_t(t) * 3u + k] - c.projMin;
				glm::vec2 uv((baseX + rel.x * invWpt) * invRes,
				             (baseY + rel.y * invWpt) * invRes);
				// A FLOAT-EPSILON GUARD, not a fitting mechanism. The construction
				// above already places every corner inside the chart's content
				// rectangle; this only stops a corner that lands exactly on the
				// atlas edge from rounding to 1.0000001 and wrapping to the far
				// side of the atlas, which would read a completely unrelated
				// chart. If a UV is ever meaningfully outside, that is a packing
				// bug and clamping it here would hide it -- which is why
				// X3LightmapTest asserts the range with the clamp's own tolerance
				// rather than trusting the clamp.
				out.cornerUV[size_t(t) * 3u + k] = glm::clamp(uv, glm::vec2(0.0f), glm::vec2(1.0f));
			}
		}

		return out;
	}

	uint32_t DilateLightmap(std::vector<glm::vec4>& texels,
	                        std::vector<uint8_t>& coverage,
	                        uint32_t width, uint32_t height,
	                        uint32_t passes)
	{
		const size_t count = size_t(width) * size_t(height);
		if (count == 0 || texels.size() < count || coverage.size() < count) return 0;

		uint32_t filled = 0;

		// COVERAGE IS NOT TOUCHED UNTIL THE PASS IS OVER. Newly filled texels are
		// collected here and marked at the end, which is what makes a pass a true
		// one-ring dilation. Marking them as they are found would let a texel
		// filled earlier in this pass immediately seed the next one, so the flood
		// would travel arbitrarily far to the right and down and exactly one texel
		// up and left -- the gutter would fill asymmetrically, and by how much
		// would depend on where in the atlas the chart happened to be packed.
		//
		// Deferring the marks is also why the loop below can read `coverage`
		// directly instead of a copy: within a pass it is a constant.
		std::vector<uint32_t> pending;
		pending.reserve(count / 4 + 1);

		for (uint32_t pass = 0; pass < passes; ++pass) {
			pending.clear();

			for (uint32_t y = 0; y < height; ++y) {
				for (uint32_t x = 0; x < width; ++x) {
					const size_t idx = size_t(y) * width + x;
					// NEVER overwrite a covered texel. A baked value is the
					// expensive thing in this whole phase; dilation exists to fill
					// the space around it, not to average it away.
					if (coverage[idx]) continue;

					glm::vec4 accum(0.0f);
					uint32_t n = 0;

					// 8-neighbourhood: a 4-neighbourhood cannot reach a texel that
					// touches the covered set only at a corner, so every convex
					// corner of every chart keeps a black pixel that bilinear
					// filtering then smears into the shading.
					for (int dy = -1; dy <= 1; ++dy) {
						const int ny = int(y) + dy;
						if (ny < 0 || ny >= int(height)) continue;
						for (int dx = -1; dx <= 1; ++dx) {
							if (dx == 0 && dy == 0) continue;
							const int nx = int(x) + dx;
							if (nx < 0 || nx >= int(width)) continue;

							const size_t nidx = size_t(ny) * width + nx;
							if (!coverage[nidx]) continue;
							accum += texels[nidx];
							++n;
						}
					}

					if (n == 0) continue;
					// Averaged rather than copied from the nearest neighbour: the
					// extension then continues the chart's own gradient across the
					// edge, so the bilinear ramp out of the chart is smooth instead
					// of stepping to whichever neighbour the scan happened to see
					// first.
					texels[idx] = accum / float(n);
					pending.push_back(uint32_t(idx));
				}
			}

			// THE DEFERRED MARK. See the top of the function: this is the single
			// line that makes a pass one ring rather than a scan-order flood.
			if (pending.empty()) break;   // nothing left to reach; further passes cannot help
			for (const uint32_t idx : pending) coverage[idx] = 1u;
			filled += uint32_t(pending.size());
		}

		return filled;
	}

}
