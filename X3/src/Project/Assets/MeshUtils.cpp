#include "Project/Assets/MeshUtils.h"

#include <glm/gtc/constants.hpp>

namespace X3
{

	namespace {
		// The UVs live in the .w lanes of the first two vec4s -- see Gpu::Vertex.
		inline glm::vec2 uvOf(const Gpu::Vertex& v) {
			return { v.positionU.w, v.normalV.w };
		}
	}

	void ComputeTangents(std::vector<Gpu::Vertex>& vertices,
	                     const std::vector<Gpu::TriRef>& tris,
	                     uint32_t firstTri, uint32_t triCount)
	{
		if (triCount == 0) return;

		// Accumulate the per-face tangent and bitangent onto every incident
		// vertex. The bitangent accumulator is not stored in the vertex -- it is
		// only needed to recover the handedness sign at the end.
		std::vector<glm::vec3> tanAccum(vertices.size(), glm::vec3(0.0f));
		std::vector<glm::vec3> bitAccum(vertices.size(), glm::vec3(0.0f));

		for (uint32_t t = 0; t < triCount; ++t) {
			const Gpu::TriRef& tri = tris[firstTri + t];
			if (tri.i0 >= vertices.size() || tri.i1 >= vertices.size() || tri.i2 >= vertices.size())
				continue;

			const Gpu::Vertex& a = vertices[tri.i0];
			const Gpu::Vertex& b = vertices[tri.i1];
			const Gpu::Vertex& c = vertices[tri.i2];

			const glm::vec3 e1 = glm::vec3(b.positionU) - glm::vec3(a.positionU);
			const glm::vec3 e2 = glm::vec3(c.positionU) - glm::vec3(a.positionU);

			const glm::vec2 duv1 = uvOf(b) - uvOf(a);
			const glm::vec2 duv2 = uvOf(c) - uvOf(a);

			// Zero UV area: the parameterisation is degenerate over this face and
			// there is no tangent to derive. Contribute nothing rather than
			// dividing by ~0 and poisoning every vertex of the triangle.
			const float det = duv1.x * duv2.y - duv2.x * duv1.y;
			if (glm::abs(det) < 1e-12f)
				continue;

			const float r = 1.0f / det;
			const glm::vec3 tangent   = (e1 * duv2.y - e2 * duv1.y) * r;
			const glm::vec3 bitangent = (e2 * duv1.x - e1 * duv2.x) * r;

			for (uint32_t idx : { tri.i0, tri.i1, tri.i2 }) {
				tanAccum[idx] += tangent;
				bitAccum[idx] += bitangent;
			}
		}

		// Only touch the vertices this range actually referenced. A vertex the
		// range never mentions keeps whatever it already had, which matters when
		// this is called per-submesh over a shared vertex array.
		for (uint32_t t = 0; t < triCount; ++t) {
			const Gpu::TriRef& tri = tris[firstTri + t];
			for (uint32_t idx : { tri.i0, tri.i1, tri.i2 }) {
				if (idx >= vertices.size()) continue;

				Gpu::Vertex& v = vertices[idx];
				const glm::vec3 n = glm::vec3(v.normalV);
				const glm::vec3 acc = tanAccum[idx];

				// Gram-Schmidt against the vertex normal. If the accumulated
				// tangent is parallel to the normal (or cancelled to nothing) the
				// projection leaves ~0 and there is no usable tangent frame.
				const glm::vec3 ortho = acc - n * glm::dot(n, acc);
				const float len = glm::length(ortho);
				if (len < 1e-8f) {
					v.tangent = glm::vec4(0.0f);   // w == 0: the sentinel
					continue;
				}

				const glm::vec3 tangent = ortho / len;
				// Handedness: which way the bitangent actually points relative to
				// the one cross(N,T) would produce.
				const float handedness =
					(glm::dot(glm::cross(n, tangent), bitAccum[idx]) < 0.0f) ? -1.0f : 1.0f;

				v.tangent = glm::vec4(tangent, handedness);
			}
		}
	}

	void ComputeSmoothNormals(std::vector<Gpu::Vertex>& vertices,
	                          const std::vector<Gpu::TriRef>& tris,
	                          uint32_t firstTri, uint32_t triCount)
	{
		if (triCount == 0) return;

		std::vector<glm::vec3> accum(vertices.size(), glm::vec3(0.0f));

		for (uint32_t t = 0; t < triCount; ++t) {
			const Gpu::TriRef& tri = tris[firstTri + t];
			if (tri.i0 >= vertices.size() || tri.i1 >= vertices.size() || tri.i2 >= vertices.size())
				continue;

			const glm::vec3 p0 = glm::vec3(vertices[tri.i0].positionU);
			const glm::vec3 p1 = glm::vec3(vertices[tri.i1].positionU);
			const glm::vec3 p2 = glm::vec3(vertices[tri.i2].positionU);

			// Unnormalised cross product: its magnitude is twice the face area,
			// which is exactly the area weighting a smooth normal wants.
			const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);

			accum[tri.i0] += faceNormal;
			accum[tri.i1] += faceNormal;
			accum[tri.i2] += faceNormal;
		}

		for (uint32_t t = 0; t < triCount; ++t) {
			const Gpu::TriRef& tri = tris[firstTri + t];
			for (uint32_t idx : { tri.i0, tri.i1, tri.i2 }) {
				if (idx >= vertices.size()) continue;

				const glm::vec3 acc = accum[idx];
				const float len = glm::length(acc);
				// A vertex with no usable normal gets +Y rather than a NaN. It is
				// wrong, but it is wrong in a way that renders instead of
				// producing black pixels that look like a different bug.
				const glm::vec3 n = (len > 1e-8f) ? (acc / len) : glm::vec3(0.0f, 1.0f, 0.0f);

				vertices[idx].normalV = glm::vec4(n, vertices[idx].normalV.w);
			}
		}
	}

}
