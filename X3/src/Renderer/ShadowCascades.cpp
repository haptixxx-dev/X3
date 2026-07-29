#include "Renderer/ShadowCascades.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace X3
{
	namespace {

		/// The eight world-space corners of the camera frustum between two view
		/// depths.
		///
		/// Derived from focalLength directly rather than by unprojecting through
		/// an inverse projection matrix. The half-extent at view depth z is
		/// z/proj[0][0] horizontally and z/proj[1][1] vertically, and this
		/// engine's projection has proj[0][0] == focalLength and
		/// proj[1][1] == aspect * focalLength -- see the fovY derivation in
		/// Renderer::SetupGPUResources, which had to be corrected once because
		/// focalLength is a HORIZONTAL parameter and glm takes a vertical fov.
		///
		/// Doing it this way means the cascade fit cannot disagree with the
		/// projection through a stale or transposed inverse.
		std::array<glm::vec3, 8> FrustumSliceCorners(const glm::mat4& cameraTransform,
		                                             float focalLength, float aspect,
		                                             float zNear, float zFar) {
			const float invX = 1.0f / focalLength;
			const float invY = 1.0f / (aspect * focalLength);

			std::array<glm::vec3, 8> corners{};
			int i = 0;
			for (float z : { zNear, zFar }) {
				const float hx = z * invX;
				const float hy = z * invY;
				for (float sy : { -1.0f, 1.0f }) {
					for (float sx : { -1.0f, 1.0f }) {
						// +Z IS FORWARD in this engine's camera space. A
						// right-handed -Z-forward convention here would place
						// every cascade behind the camera, and the shadow map
						// would be empty in a way that looks like the pass never
						// ran.
						const glm::vec4 viewPos(sx * hx, sy * hy, z, 1.0f);
						corners[i++] = glm::vec3(cameraTransform * viewPos);
					}
				}
			}
			return corners;
		}

	} // namespace

	ShadowCascades ComputeShadowCascades(const glm::mat4& cameraTransform,
	                                     float focalLength, float aspect,
	                                     float nearPlane, float shadowFarPlane,
	                                     const glm::vec3& lightDirection,
	                                     uint32_t shadowMapResolution,
	                                     float lambda) {
		ShadowCascades out{};

		const float n = glm::max(nearPlane, 1e-4f);
		const float f = glm::max(shadowFarPlane, n * 1.001f);
		const float ratio = f / n;
		const float res = float(glm::max(shadowMapResolution, 1u));

		// THE SHADOW FAR PLANE IS NOT THE CAMERA'S. The camera's is 1000 units;
		// fitting four cascades across that would put the near cascade's texels
		// metres apart and every contact shadow would be a blur. The caller
		// passes a much closer distance and geometry beyond it falls back to the
		// traced path, which has no such limit.

		const glm::vec3 L = glm::normalize(lightDirection);

		// A light pointing straight down makes the obvious up vector degenerate,
		// and lookAt then produces a matrix full of NaNs -- which propagates into
		// every cascade matrix and blanks the shadow map with no error anywhere.
		const glm::vec3 up = (std::abs(L.y) > 0.999f) ? glm::vec3(0.0f, 0.0f, 1.0f)
		                                              : glm::vec3(0.0f, 1.0f, 0.0f);

		float sliceNear = n;
		for (uint32_t c = 0; c < SHADOW_CASCADE_COUNT; ++c) {
			// The practical split scheme: a blend of the uniform and logarithmic
			// distributions. Uniform alone gives the near cascade almost no depth
			// range where the eye spends all its time; logarithmic alone crushes
			// the far cascades into nothing.
			const float p = float(c + 1) / float(SHADOW_CASCADE_COUNT);
			const float logSplit = n * std::pow(ratio, p);
			const float uniSplit = n + (f - n) * p;
			const float sliceFar = lambda * logSplit + (1.0f - lambda) * uniSplit;

			const std::array<glm::vec3, 8> corners =
				FrustumSliceCorners(cameraTransform, focalLength, aspect, sliceNear, sliceFar);

			// BOUNDING SPHERE, not a bounding box. Its radius does not change
			// when the camera rotates, which is half of what stops shadow edges
			// crawling. See the header.
			glm::vec3 center(0.0f);
			for (const glm::vec3& p3 : corners) center += p3;
			center /= float(corners.size());

			float radius = 0.0f;
			for (const glm::vec3& p3 : corners)
				radius = glm::max(radius, glm::length(p3 - center));
			// Rounded up, so a radius that wobbles in the last few bits of float
			// does not resize the cascade and undo the snapping below.
			radius = std::ceil(radius * 16.0f) / 16.0f;

			const float texelWorldSize = (2.0f * radius) / res;

			// SNAP THE CENTRE TO THE TEXEL GRID, in light space. Without this the
			// projection slides by a fraction of a texel as the camera moves and
			// the sampled depth flickers between neighbouring texels -- the
			// classic shimmering shadow edge. Quantising here means a moving
			// camera shifts the map by whole texels only.
			//
			// The snap must happen in the LIGHT'S basis -- rounding the
			// world-space centre would quantise along the wrong axes and do
			// nothing useful.
			//
			// THE BASIS MUST BE ORIGIN-FIXED, and that is the whole subtlety. The
			// obvious spelling, lookAtLH(center, center + L, up), places `center`
			// AT ITS OWN ORIGIN: the light-space coordinate is then (0, 0, 0) by
			// construction, snapping it is a no-op, and the shimmer the snap
			// exists to remove is still there. Nothing about that failure is
			// visible in a still frame -- it only shows up as crawling edges when
			// the camera moves -- which is exactly why X3MathTest asserts the
			// whole-texel property instead of trusting the code to be right.
			//
			// A rotation about the world origin gives a basis that does not move
			// with the cascade, so the quantisation grid stays put between frames.
			// The final lightView below is built from the same L and up, so its
			// x/y axes match this one and the snap survives into the projection.
			const glm::mat4 lightBasis = glm::lookAtLH(glm::vec3(0.0f), L, up);
			glm::vec3 centerLS = glm::vec3(lightBasis * glm::vec4(center, 1.0f));
			centerLS.x = std::floor(centerLS.x / texelWorldSize) * texelWorldSize;
			centerLS.y = std::floor(centerLS.y / texelWorldSize) * texelWorldSize;
			center = glm::vec3(glm::inverse(lightBasis) * glm::vec4(centerLS, 1.0f));

			// PULL THE EYE BACK BY MORE THAN THE RADIUS so that casters standing
			// between the light and the cascade are still inside the near plane.
			// A shadow map whose near plane clips its own casters produces
			// objects that light the ground through themselves.
			const float backOff = radius * 3.0f;
			const glm::mat4 lightView = glm::lookAtLH(center - L * backOff, center, up);

			// orthoLH_ZO with near and far SWAPPED, which is what makes it
			// reverse-Z -- the same trick and the same reason as the perspective
			// projection: one set of pipeline defaults (GREATER, clear to 0) then
			// serves every depth target in the engine.
			const glm::mat4 lightProj = glm::orthoLH_ZO(
				-radius, radius, -radius, radius,
				backOff + radius,      // far, passed as near
				0.0f);                 // near, passed as far

			out.viewProj[c]       = lightProj * lightView;
			out.splitDepth[c]     = sliceFar;
			out.texelWorldSize[c] = texelWorldSize;

			sliceNear = sliceFar;
		}

		return out;
	}

}
