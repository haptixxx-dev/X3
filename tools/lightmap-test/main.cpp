// =============================================================================
// X3LightmapTest -- assertions over the lightmap unwrapper, the atlas packer and
// the dilation pass.
//
// LIKE X3MathTest, NEEDS NO GPU AND NO DISPLAY, and for the same reason: none of
// this code touches Vulkan. Every mesh it unwraps is built in this file, so
// there is no fixture that can go stale while the assertions keep passing
// against the wrong geometry.
//
// Every check here is a PROPERTY that must hold for any mesh, not a value
// recorded from a run. That distinction matters more here than almost anywhere
// else in the engine: a lightmap atlas is a picture, and a picture of a broken
// atlas looks exactly as plausible as a picture of a correct one. Two charts
// overlapping by one texel is invisible in the atlas and unmistakable on the
// wall it lights.
//
// Exit code 0 only if every check passed.
// =============================================================================

#include "Project/Assets/LightmapUV.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

	int g_Failures = 0;
	int g_Checks   = 0;

	void check(bool ok, const std::string& what) {
		++g_Checks;
		if (!ok) {
			++g_Failures;
			std::printf("  \033[31mFAIL\033[0m  %s\n", what.c_str());
		}
	}

	void checkClose(float a, float b, float tol, const std::string& what) {
		++g_Checks;
		if (!(std::abs(a - b) <= tol)) {
			++g_Failures;
			std::printf("  \033[31mFAIL\033[0m  %s  (%.6f vs %.6f, tol %.6f)\n",
			            what.c_str(), a, b, tol);
		}
	}

	// -------------------------------------------------------------------------
	// Test meshes. Built by hand rather than loaded, so the expected chart
	// topology is visible right here beside the assertion about it.
	// -------------------------------------------------------------------------

	struct Mesh {
		std::vector<X3::Gpu::Vertex>  verts;
		std::vector<X3::Gpu::TriRef>  tris;
	};

	// EVERY VERTEX NORMAL IS THE SAME DELIBERATELY WRONG DIRECTION. The unwrapper
	// is documented to chart on geometric face normals, never on Gpu::Vertex's
	// shading normals; if it ever reads them, the cube collapses to one chart and
	// TestCubeSplitsIntoCharts fails. This is the assertion, expressed as data.
	X3::Gpu::Vertex V(float x, float y, float z) {
		X3::Gpu::Vertex v{};
		v.positionU = { x, y, z, 0.0f };
		v.normalV   = { 0.0f, 1.0f, 0.0f, 0.0f };
		return v;
	}

	void AddTri(Mesh& m, uint32_t a, uint32_t b, uint32_t c) {
		X3::Gpu::TriRef t{};
		t.i0 = a; t.i1 = b; t.i2 = c;
		m.tris.push_back(t);
	}

	// Two coplanar triangles sharing an edge: the minimal one-chart mesh.
	Mesh MakeQuad() {
		Mesh m;
		m.verts = { V(0,0,0), V(2,0,0), V(2,3,0), V(0,3,0) };
		AddTri(m, 0, 1, 2);
		AddTri(m, 0, 2, 3);
		return m;
	}

	// Eight shared corners, twelve triangles, six face normals 90 degrees apart.
	// Shared vertices are the point: adjacency reaches across every cube edge, so
	// only the normal cone can stop a chart from wrapping around the solid.
	Mesh MakeCube() {
		Mesh m;
		m.verts = {
			V(-1,-1,-1), V( 1,-1,-1), V( 1, 1,-1), V(-1, 1,-1),
			V(-1,-1, 1), V( 1,-1, 1), V( 1, 1, 1), V(-1, 1, 1),
		};
		AddTri(m, 4,5,6); AddTri(m, 4,6,7);   // +z
		AddTri(m, 1,0,3); AddTri(m, 1,3,2);   // -z
		AddTri(m, 5,1,2); AddTri(m, 5,2,6);   // +x
		AddTri(m, 0,4,7); AddTri(m, 0,7,3);   // -x
		AddTri(m, 3,7,6); AddTri(m, 3,6,2);   // +y
		AddTri(m, 0,1,5); AddTri(m, 0,5,4);   // -y
		return m;
	}

	// An open cylinder wall. THE CASE THE SEED-CONE RULE EXISTS FOR: every
	// triangle is within a few degrees of its neighbour, so a neighbour-relative
	// rule would grow one chart the whole way round and project the far side onto
	// the near side.
	Mesh MakeCylinder(uint32_t sides, float radius, float halfHeight) {
		Mesh m;
		for (uint32_t i = 0; i < sides; ++i) {
			const float a = 6.2831853f * float(i) / float(sides);
			m.verts.push_back(V(radius * std::cos(a), -halfHeight, radius * std::sin(a)));
			m.verts.push_back(V(radius * std::cos(a),  halfHeight, radius * std::sin(a)));
		}
		for (uint32_t i = 0; i < sides; ++i) {
			const uint32_t b0 = 2 * i;
			const uint32_t b1 = 2 * ((i + 1) % sides);
			AddTri(m, b0, b1, b1 + 1);
			AddTri(m, b0, b1 + 1, b0 + 1);
		}
		return m;
	}

	// THE ADVERSARIAL CASE FOR THE 44-DEGREE CONE CLAMP. Three triangles hinged on
	// one shared edge: a tiny one facing straight up (the seed, since seeds are
	// taken in triangle order), a tiny one rolled 85 degrees one way, and a HUGE
	// one rolled 85 degrees the other.
	//
	// If the cone were wide enough to admit both wings into the seed's chart, the
	// area-weighted mean normal would be dragged onto the huge wing -- 170 degrees
	// from the small one -- and the small wing would project with NEGATIVE area:
	// folded over, its texels landing on the huge wing's. That is precisely the
	// failure the 2*cone < 90 clamp exists to make impossible, and this mesh is
	// what turns the clamp from a comment into a tested property.
	Mesh MakeHinge() {
		Mesh m;
		m.verts = { V(0,0,0), V(1,0,0) };
		// A triangle on the shared edge with apex direction (sin p, -cos p) in the
		// YZ plane has face normal (0, cos p, sin p) -- see the cross product with
		// the hinge direction (1,0,0).
		auto apex = [&m](float degrees, float len) {
			const float p = degrees * 3.14159265f / 180.0f;
			m.verts.push_back(V(0.5f, std::sin(p) * len, -std::cos(p) * len));
			return uint32_t(m.verts.size() - 1);
		};
		AddTri(m, 0, 1, apex(0.0f,    0.05f));   // seed: triangle 0
		AddTri(m, 0, 1, apex(-85.0f,  0.05f));   // small wing
		AddTri(m, 0, 1, apex(85.0f,  10.0f));    // heavy wing
		return m;
	}

	// A quad plus a triangle with two identical corners: zero area, no normal.
	// The importer produces these from badly authored models and the bake must
	// survive one without emitting a NaN into an atlas.
	Mesh MakeQuadWithDegenerateTri() {
		Mesh m = MakeQuad();
		m.verts.push_back(V(5, 5, 0));
		AddTri(m, 4, 4, 0);
		return m;
	}

	// -------------------------------------------------------------------------
	// Shared helpers over a result.
	// -------------------------------------------------------------------------

	glm::vec3 FaceNormal(const Mesh& m, uint32_t t, float& areaOut) {
		const auto& tri = m.tris[t];
		const glm::vec3 p0(m.verts[tri.i0].positionU);
		const glm::vec3 p1(m.verts[tri.i1].positionU);
		const glm::vec3 p2(m.verts[tri.i2].positionU);
		const glm::vec3 c = glm::cross(p1 - p0, p2 - p0);
		const float len = glm::length(c);
		areaOut = 0.5f * len;
		return (len > 1e-20f) ? c / len : glm::vec3(0.0f);
	}

	// Signed area in UV space. SIGNED, not absolute: a chart projected along an
	// axis more than 90 degrees from a face folds that face over, which shows up
	// as a NEGATIVE area while the absolute value stays perfectly healthy. The
	// folded face's texels then belong to whatever else landed on them.
	float SignedUVArea(const X3::LightmapUV& uv, uint32_t t) {
		const glm::vec2 a = uv.cornerUV[3 * t + 0];
		const glm::vec2 b = uv.cornerUV[3 * t + 1];
		const glm::vec2 c = uv.cornerUV[3 * t + 2];
		return 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
	}

	// -------------------------------------------------------------------------
	// THE RANGE PROPERTY. A UV outside [0,1] is not a slightly wrong UV: the
	// sampler wraps it, so the texel it reads belongs to a chart on the other
	// side of the atlas and that surface is lit by a completely different part of
	// the scene.
	// -------------------------------------------------------------------------
	void TestUVsInsideUnitSquare(const Mesh& m, const std::string& name,
	                             const X3::LightmapUVSettings& s) {
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		check(uv.ok, name + ": unwrap succeeded");
		check(uv.cornerUV.size() == m.tris.size() * 3, name + ": three UVs per triangle");

		bool inRange = true, finite = true;
		for (const glm::vec2& p : uv.cornerUV) {
			if (!std::isfinite(p.x) || !std::isfinite(p.y)) finite = false;
			if (p.x < 0.0f || p.x > 1.0f || p.y < 0.0f || p.y > 1.0f) inRange = false;
		}
		check(finite,  name + ": every UV is finite");
		check(inRange, name + ": every UV lies inside [0,1]");

		bool assigned = true;
		for (uint32_t c : uv.triangleChart)
			if (c == X3::LIGHTMAP_NO_CHART || c >= uv.charts.size()) assigned = false;
		check(assigned, name + ": every triangle was assigned a valid chart");
	}

	// -------------------------------------------------------------------------
	// THE PACKER'S WHOLE JOB. Two charts sharing a texel means one surface's baked
	// irradiance appears on another surface -- a bright patch of the wrong light,
	// in a place that has no relationship to the geometry causing it. Nothing
	// downstream can detect or repair it.
	//
	// Checked on the PADDED rectangles, which is the stronger statement: disjoint
	// padded rectangles imply the chart CONTENT is separated by gutter on both
	// sides of the boundary.
	// -------------------------------------------------------------------------
	void TestChartRectsDoNotOverlap(const Mesh& m, const std::string& name,
	                                const X3::LightmapUVSettings& s) {
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		if (!uv.ok) { check(false, name + ": unwrap succeeded (overlap test)"); return; }

		const uint32_t res = uv.atlasResolution;
		const uint32_t g   = uv.gutterTexels;

		bool inBounds = true, nonEmpty = true;
		for (const auto& c : uv.charts) {
			if (c.x + c.width > res || c.y + c.height > res) inBounds = false;
			if (c.width <= 2 * g || c.height <= 2 * g) nonEmpty = false;
		}
		check(inBounds, name + ": every padded chart rectangle lies inside the atlas");
		check(nonEmpty, name + ": every chart has at least one content texel inside its gutter");

		bool disjoint = true;
		bool gutterKept = true;
		for (size_t i = 0; i < uv.charts.size(); ++i) {
			for (size_t j = i + 1; j < uv.charts.size(); ++j) {
				const auto& a = uv.charts[i];
				const auto& b = uv.charts[j];
				const bool sepX = (a.x + a.width <= b.x) || (b.x + b.width <= a.x);
				const bool sepY = (a.y + a.height <= b.y) || (b.y + b.height <= a.y);
				if (!sepX && !sepY) disjoint = false;

				// THE GUTTER PROPERTY, stated on the content rectangles: separated
				// padded rectangles put at least 2*gutter empty texels between two
				// charts' content on whichever axis separates them. That gap is
				// what stops a bilinear tap at one chart's edge from reaching the
				// other chart's texels and drawing a bright line along every
				// boundary in the scene.
				const uint32_t ax0 = a.innerX(g), ax1 = ax0 + a.innerWidth(g);
				const uint32_t ay0 = a.innerY(g), ay1 = ay0 + a.innerHeight(g);
				const uint32_t bx0 = b.innerX(g), bx1 = bx0 + b.innerWidth(g);
				const uint32_t by0 = b.innerY(g), by1 = by0 + b.innerHeight(g);
				const bool gapX = (bx0 >= ax1 + 2 * g) || (ax0 >= bx1 + 2 * g);
				const bool gapY = (by0 >= ay1 + 2 * g) || (ay0 >= by1 + 2 * g);
				if (g > 0 && !gapX && !gapY) gutterKept = false;
			}
		}
		check(disjoint, name + ": no two padded chart rectangles overlap");
		check(gutterKept, name + ": every pair of charts is separated by at least 2*gutter texels");
	}

	// Every corner must land inside ITS OWN chart's content rectangle. Disjoint
	// rectangles are worthless if the UVs do not respect them -- the overlap would
	// simply move from the rectangles to the texels.
	void TestUVsStayInsideTheirChart(const Mesh& m, const std::string& name,
	                                 const X3::LightmapUVSettings& s) {
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		if (!uv.ok) { check(false, name + ": unwrap succeeded (containment test)"); return; }

		const float res = float(uv.atlasResolution);
		const uint32_t g = uv.gutterTexels;
		// A thousandth of a texel: slack for the multiply and divide by the
		// resolution, not slack for a chart that nearly fits.
		const float tol = 1e-3f;

		bool inside = true;
		for (uint32_t t = 0; t < m.tris.size(); ++t) {
			const auto& c = uv.charts[uv.triangleChart[t]];
			const float x0 = float(c.innerX(g)), x1 = x0 + float(c.innerWidth(g));
			const float y0 = float(c.innerY(g)), y1 = y0 + float(c.innerHeight(g));
			for (uint32_t k = 0; k < 3; ++k) {
				const glm::vec2 texel = uv.cornerUV[3 * t + k] * res;
				if (texel.x < x0 - tol || texel.x > x1 + tol ||
				    texel.y < y0 - tol || texel.y > y1 + tol) inside = false;
			}
		}
		check(inside, name + ": every corner UV lies inside its own chart's content rectangle");
	}

	// -------------------------------------------------------------------------
	// THE AREA PROPERTY. A triangle with zero UV area occupies no texels, so it
	// gets no baked light at all and renders black -- and it does so silently,
	// because nothing else in the pipeline knows the triangle was supposed to have
	// texels. A NEGATIVE area means the chart projection folded the triangle over,
	// which puts its texels on top of another triangle's.
	//
	// This is the property the 44-degree cone clamp exists to guarantee: the
	// projection axis is at most 2*cone from any member face, so the projected
	// area is at least the true area times cos(2*cone), which is positive.
	// -------------------------------------------------------------------------
	void TestUVAreaIsPositive(const Mesh& m, const std::string& name,
	                          const X3::LightmapUVSettings& s) {
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		if (!uv.ok) { check(false, name + ": unwrap succeeded (area test)"); return; }

		bool positive = true;
		bool degenerateHandled = true;
		for (uint32_t t = 0; t < m.tris.size(); ++t) {
			float area3d = 0.0f;
			FaceNormal(m, t, area3d);
			const float uvArea = SignedUVArea(uv, t);
			if (!std::isfinite(uvArea)) { degenerateHandled = false; continue; }
			// Only non-degenerate input triangles are claimed to have texels. A
			// zero-area triangle in the source has no surface to light.
			if (area3d > 0.0f && !(uvArea > 0.0f)) positive = false;
		}
		check(positive, name + ": every non-degenerate triangle has positive signed UV area");
		check(degenerateHandled, name + ": no triangle produced a non-finite UV area");
	}

	// The cone bound, asserted directly on the output. If this holds, the area
	// property above cannot fail; asserting both means a regression names itself
	// as either a charting bug or a projection bug rather than just "black
	// triangles".
	void TestChartsRespectTheNormalCone(const Mesh& m, const std::string& name,
	                                    const X3::LightmapUVSettings& s) {
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		if (!uv.ok) { check(false, name + ": unwrap succeeded (cone test)"); return; }

		const float limit = std::cos(glm::radians(2.0f * uv.maxChartAngleDegrees));
		bool withinCone = true;
		for (uint32_t t = 0; t < m.tris.size(); ++t) {
			float area = 0.0f;
			const glm::vec3 n = FaceNormal(m, t, area);
			if (!(area > 0.0f)) continue;
			const auto& c = uv.charts[uv.triangleChart[t]];
			if (glm::dot(n, c.normal) < limit - 1e-4f) withinCone = false;
		}
		check(withinCone, name + ": every face is within 2*maxChartAngle of its chart's projection axis");
	}

	// -------------------------------------------------------------------------
	// CHART TOPOLOGY. Two statements about what charting is FOR: a flat surface
	// must not be cut (every cut is a seam, and every seam costs a gutter and
	// risks a visible line), and a closed solid must be cut (one chart around a
	// cube projects three of its faces onto the other three).
	// -------------------------------------------------------------------------
	void TestFlatQuadIsOneChart() {
		const Mesh m = MakeQuad();
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), {});
		check(uv.ok, "quad: unwrap succeeded");
		check(uv.charts.size() == 1, "a flat quad produces exactly one chart");
	}

	void TestCubeSplitsIntoCharts() {
		const Mesh m = MakeCube();
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), {});
		check(uv.ok, "cube: unwrap succeeded");
		check(uv.charts.size() > 1, "a cube produces more than one chart");
		// Stronger, and still a property rather than a recorded number: a cube has
		// six planar faces and twelve triangles, so a charter that groups by
		// planarity cannot need more charts than there are triangles, and cannot
		// use fewer than there are distinct face planes.
		check(uv.charts.size() >= 6, "a cube needs at least one chart per face plane");
		check(uv.charts.size() <= m.tris.size(), "no chart is empty");
	}

	// A curved surface must be cut into a bounded number of charts. The upper
	// bound is what fails if the seed-cone rule is ever relaxed to a
	// neighbour-relative one: the whole cylinder would come back as one chart,
	// with the far side projected on top of the near side.
	void TestCylinderChartsAreBounded() {
		const Mesh m = MakeCylinder(64, 1.0f, 1.0f);
		X3::LightmapUVSettings s;
		s.maxChartAngleDegrees = 40.0f;
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		check(uv.ok, "cylinder: unwrap succeeded");
		// 360 degrees of turning, charts spanning at most 2*40 degrees each (the
		// seed cone reaches maxChartAngle either side of the seed).
		const size_t minCharts = size_t(std::ceil(360.0f / (2.0f * s.maxChartAngleDegrees)));
		check(uv.charts.size() >= minCharts,
		      "a cylinder is cut into at least 360/(2*cone) charts");
		check(uv.charts.size() <= m.tris.size(), "cylinder: chart count never exceeds triangle count");
	}

	// -------------------------------------------------------------------------
	// Determinism. The cook must be reproducible: a bake that re-charts
	// differently on the second run invalidates every lightmap in the project for
	// no reason, and makes any diff of cooked output meaningless.
	// -------------------------------------------------------------------------
	void TestDeterminism() {
		const Mesh m = MakeCylinder(37, 2.0f, 0.5f);
		const auto a = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), {});
		const auto b = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), {});

		bool same = a.charts.size() == b.charts.size() && a.cornerUV.size() == b.cornerUV.size();
		if (same)
			for (size_t i = 0; i < a.cornerUV.size(); ++i)
				if (a.cornerUV[i] != b.cornerUV[i]) same = false;
		if (same)
			for (size_t i = 0; i < a.charts.size(); ++i)
				if (a.charts[i].x != b.charts[i].x || a.charts[i].y != b.charts[i].y ||
				    a.charts[i].width != b.charts[i].width || a.charts[i].height != b.charts[i].height)
					same = false;
		check(same, "two unwraps of the same mesh are byte-identical");
	}

	// A gutter wider than the atlas leaves no room for content. It must FAIL
	// rather than quietly drop the gutter -- an atlas packed with no gutter looks
	// correct until it is sampled.
	void TestImpossibleGutterFails() {
		const Mesh m = MakeQuad();
		X3::LightmapUVSettings s;
		s.atlasResolution = 8;
		s.gutterTexels    = 8;
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		check(!uv.ok, "a gutter wider than the atlas is reported as a failure, not silently dropped");
	}

	// The auto-fit must coarsen the density until the charts fit, rather than
	// emitting UVs outside the atlas. A 2-degree cone cuts this cylinder into one
	// chart per side quad; 64 padded charts into a 64x64 atlas is tight enough
	// that the first density estimate cannot pack, which is the retry loop's
	// whole reason to exist -- and exactly the case where a packer that gave up
	// quietly would start writing out-of-range UVs.
	void TestAutoFitShrinksToFit() {
		const Mesh m = MakeCylinder(64, 4.0f, 4.0f);
		X3::LightmapUVSettings s;
		s.atlasResolution      = 64;
		s.gutterTexels         = 1;
		s.maxChartAngleDegrees = 2.0f;
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		check(uv.ok, "a crowded atlas still packs after the auto-fit coarsens the density");
		check(uv.worldUnitsPerTexel > 0.0f, "the fitted texel density is reported and positive");
		TestUVsInsideUnitSquare(m, "crowded atlas", s);
		TestChartRectsDoNotOverlap(m, "crowded atlas", s);
	}

	// A requested density that cannot fit must be coarsened, not honoured into an
	// overflowing atlas. Asserted as a property of the OUTPUT: whatever density
	// came back, the charts fit inside the atlas at it.
	void TestExplicitDensityIsCoarsenedWhenItCannotFit() {
		const Mesh m = MakeCube();
		X3::LightmapUVSettings s;
		s.atlasResolution   = 32;
		s.gutterTexels      = 2;
		s.worldUnitsPerTexel = 0.001f;   // 2000 texels per cube face, into 32
		const auto uv = X3::GenerateLightmapUVs(m.verts, m.tris, 0, uint32_t(m.tris.size()), s);
		check(uv.ok, "an impossible explicit density is coarsened rather than refused");
		check(uv.worldUnitsPerTexel > s.worldUnitsPerTexel,
		      "the reported density is the coarsened one, not the requested one");
		TestChartRectsDoNotOverlap(m, "explicit density", s);
	}

	// -------------------------------------------------------------------------
	// DILATION. Three properties, and the two negative ones matter most: baked
	// values are the expensive product of the whole phase, and a dilation that
	// touches them destroys work rather than merely failing to help.
	// -------------------------------------------------------------------------

	struct Atlas {
		std::vector<glm::vec4> texels;
		std::vector<uint8_t>   coverage;
		uint32_t w = 0, h = 0;
	};

	Atlas MakeAtlas(uint32_t w, uint32_t h) {
		Atlas a;
		a.w = w; a.h = h;
		a.texels.assign(size_t(w) * h, glm::vec4(0.0f));
		a.coverage.assign(size_t(w) * h, 0u);
		return a;
	}

	uint32_t CoveredCount(const Atlas& a) {
		uint32_t n = 0;
		for (uint8_t c : a.coverage) if (c) ++n;
		return n;
	}

	void TestDilationGrowsAndPreserves() {
		Atlas a = MakeAtlas(16, 16);

		// A 4x4 covered block, every texel a DIFFERENT value. Distinct values are
		// the point: a dilation that averaged over a covered texel would change it
		// undetectably if every covered texel held the same number.
		for (uint32_t y = 6; y < 10; ++y)
			for (uint32_t x = 6; x < 10; ++x) {
				const size_t idx = size_t(y) * a.w + x;
				a.texels[idx]   = glm::vec4(float(idx), float(idx) * 2.0f, 1.0f, 1.0f);
				a.coverage[idx] = 1u;
			}

		const std::vector<glm::vec4> before = a.texels;
		const std::vector<uint8_t>   coveredBefore = a.coverage;
		const uint32_t countBefore = CoveredCount(a);

		const uint32_t filled = X3::DilateLightmap(a.texels, a.coverage, a.w, a.h, 1);
		const uint32_t countAfter = CoveredCount(a);

		check(countAfter > countBefore, "dilation strictly grows the covered set");
		check(filled == countAfter - countBefore, "the returned fill count matches the growth");

		// NEVER OVERWRITE. Bitwise equality, not approximate: a baked texel must
		// come out of dilation as the exact same bits it went in with.
		bool preserved = true;
		for (size_t i = 0; i < before.size(); ++i)
			if (coveredBefore[i] && a.texels[i] != before[i]) preserved = false;
		check(preserved, "dilation never overwrites an already-covered texel");

		// Covered texels stay covered -- dilation is monotone, so a second pass can
		// never un-fill what a first pass filled.
		bool monotone = true;
		for (size_t i = 0; i < coveredBefore.size(); ++i)
			if (coveredBefore[i] && !a.coverage[i]) monotone = false;
		check(monotone, "dilation never un-covers a texel");

		// ONE RING PER PASS, exactly. With an 8-neighbourhood the covered set grows
		// by the Chebyshev ball of radius one, so a 4x4 block becomes 6x6. More
		// than that means a texel filled during the pass seeded further growth
		// inside the same pass -- the scan-order bug the coverage snapshot exists
		// to prevent, which fills the gutter further in +x/+y than in -x/-y.
		check(countAfter == 6 * 6, "one pass grows the covered set by exactly one ring");

		X3::DilateLightmap(a.texels, a.coverage, a.w, a.h, 1);
		check(CoveredCount(a) == 8 * 8, "a second pass grows by exactly one more ring");
	}

	void TestDilationTerminates() {
		// Nothing covered: there is no value to spread, and inventing one would
		// mean writing black over the whole atlas and calling it lit.
		Atlas empty = MakeAtlas(8, 8);
		const uint32_t none = X3::DilateLightmap(empty.texels, empty.coverage, empty.w, empty.h, 4);
		check(none == 0, "dilating an empty atlas fills nothing");
		check(CoveredCount(empty) == 0, "dilating an empty atlas covers nothing");

		// Fully covered: there is nowhere to grow, so the pass must do nothing at
		// all rather than re-averaging every texel with its neighbours -- which
		// would blur the entire lightmap once per requested pass.
		Atlas full = MakeAtlas(8, 8);
		for (size_t i = 0; i < full.texels.size(); ++i) {
			full.texels[i]   = glm::vec4(float(i), 0.0f, 0.0f, 1.0f);
			full.coverage[i] = 1u;
		}
		const std::vector<glm::vec4> before = full.texels;
		const uint32_t filled = X3::DilateLightmap(full.texels, full.coverage, full.w, full.h, 4);
		check(filled == 0, "dilating a fully covered atlas fills nothing");
		bool unchanged = true;
		for (size_t i = 0; i < before.size(); ++i) if (full.texels[i] != before[i]) unchanged = false;
		check(unchanged, "dilating a fully covered atlas changes nothing");
	}

	// The dilated value must be a blend of real baked neighbours, never zero and
	// never NaN: a NaN in a lightmap propagates through the shading of everything
	// that samples it.
	void TestDilatedValuesComeFromNeighbours() {
		Atlas a = MakeAtlas(8, 8);
		const glm::vec4 lit(0.25f, 0.5f, 0.75f, 1.0f);
		for (uint32_t y = 3; y < 5; ++y)
			for (uint32_t x = 3; x < 5; ++x) {
				a.coverage[size_t(y) * a.w + x] = 1u;
				a.texels[size_t(y) * a.w + x]   = lit;
			}

		const std::vector<uint8_t> coveredBefore = a.coverage;
		X3::DilateLightmap(a.texels, a.coverage, a.w, a.h, 2);

		bool sane = true;
		for (size_t i = 0; i < a.texels.size(); ++i) {
			if (!a.coverage[i] || coveredBefore[i]) continue;
			// Every source texel holds the same value, so any average of them must
			// reproduce it exactly.
			if (a.texels[i] != lit) sane = false;
		}
		check(sane, "a dilated texel is the average of its covered neighbours");
	}

	// -------------------------------------------------------------------------

	void RunMeshSuite(const Mesh& m, const std::string& name, const X3::LightmapUVSettings& s) {
		TestUVsInsideUnitSquare(m, name, s);
		TestChartRectsDoNotOverlap(m, name, s);
		TestUVsStayInsideTheirChart(m, name, s);
		TestUVAreaIsPositive(m, name, s);
		TestChartsRespectTheNormalCone(m, name, s);
	}

} // namespace

int main() {
	std::printf("X3LightmapTest\n");

	X3::LightmapUVSettings defaults;

	X3::LightmapUVSettings tightCone;
	tightCone.maxChartAngleDegrees = 5.0f;    // many small charts: stresses the packer

	X3::LightmapUVSettings wideCone;
	wideCone.maxChartAngleDegrees = 90.0f;    // must be clamped to 44, or the area property dies

	X3::LightmapUVSettings noGutter;
	noGutter.gutterTexels = 0;                // the gutter must be optional without breaking packing

	X3::LightmapUVSettings fatGutter;
	fatGutter.gutterTexels = 6;

	const Mesh quad     = MakeQuad();
	const Mesh cube     = MakeCube();
	const Mesh cylinder = MakeCylinder(24, 1.5f, 2.0f);
	const Mesh degen    = MakeQuadWithDegenerateTri();
	const Mesh hinge    = MakeHinge();

	RunMeshSuite(quad,     "quad",              defaults);
	RunMeshSuite(cube,     "cube",              defaults);
	RunMeshSuite(cylinder, "cylinder",          defaults);
	RunMeshSuite(degen,    "quad+degenerate",   defaults);
	RunMeshSuite(cylinder, "cylinder/tight",    tightCone);
	RunMeshSuite(cylinder, "cylinder/wide",     wideCone);
	RunMeshSuite(cube,     "cube/no-gutter",    noGutter);
	RunMeshSuite(cube,     "cube/fat-gutter",   fatGutter);
	RunMeshSuite(hinge,    "hinge",             defaults);
	RunMeshSuite(hinge,    "hinge/wide",        wideCone);

	// The hinge's three faces are 85 degrees apart, so the clamped cone must
	// refuse to put any two of them in one chart no matter what was requested.
	// One chart per face is what keeps the small wing's texels its own.
	{
		const auto uv = X3::GenerateLightmapUVs(hinge.verts, hinge.tris, 0,
		                                        uint32_t(hinge.tris.size()), wideCone);
		check(uv.charts.size() == 3, "faces 85 degrees apart are never merged into one chart");
	}

	TestFlatQuadIsOneChart();
	TestCubeSplitsIntoCharts();
	TestCylinderChartsAreBounded();
	TestDeterminism();
	TestImpossibleGutterFails();
	TestAutoFitShrinksToFit();
	TestExplicitDensityIsCoarsenedWhenItCannotFit();

	// The clamp is load-bearing for the area property, so assert it directly
	// rather than only through its consequences.
	{
		const auto uv = X3::GenerateLightmapUVs(cube.verts, cube.tris, 0,
		                                        uint32_t(cube.tris.size()), wideCone);
		checkClose(uv.maxChartAngleDegrees, 44.0f, 1e-4f,
		           "a chart angle of 90 degrees is clamped to 44");
	}

	// An empty range is not an error. The cook calls this per submesh and some
	// submeshes are empty.
	{
		const auto uv = X3::GenerateLightmapUVs(quad.verts, quad.tris, 0, 0, defaults);
		check(uv.ok, "an empty triangle range succeeds");
		check(uv.charts.empty(), "an empty triangle range produces no charts");
	}

	// Unwrapping a SUB-RANGE must behave exactly like unwrapping that range on its
	// own -- the (firstTri, triCount) contract this shares with
	// MeshUtils::ComputeTangents.
	{
		const auto uv = X3::GenerateLightmapUVs(cube.verts, cube.tris, 2, 2, defaults);
		check(uv.ok, "a sub-range unwraps");
		check(uv.cornerUV.size() == 6, "a sub-range emits UVs for its own triangles only");
		check(uv.charts.size() == 1, "the two triangles of one cube face form one chart");
	}

	TestDilationGrowsAndPreserves();
	TestDilationTerminates();
	TestDilatedValuesComeFromNeighbours();

	if (g_Failures == 0) {
		std::printf("  \033[32m%d checks passed\033[0m\n", g_Checks);
		return 0;
	}
	std::printf("  \033[31m%d of %d checks failed\033[0m\n", g_Failures, g_Checks);
	return 1;
}
