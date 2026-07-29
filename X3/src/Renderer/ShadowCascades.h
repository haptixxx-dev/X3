#pragma once

// =============================================================================
// ShadowCascades -- where the four cascade matrices come from.
//
// Split out of Renderer.cpp because it is pure arithmetic over a camera and a
// light direction: no Vulkan, no frame, nothing to mock. That makes it the one
// part of the shadow path that can be checked by asserting on numbers rather
// than by looking at an image, which matters because almost every cascaded
// shadow map bug looks like "the shadows are slightly wrong somewhere".
//
// STABILITY IS THE WHOLE DESIGN, not a polish pass. A cascade fitted tightly to
// the camera frustum changes size and orientation as the camera turns, so every
// shadow texel lands on a different world position each frame and every shadow
// edge crawls. ENGINE_PLAN.md names that -- "stabilized against shimmer" -- as a
// requirement rather than a nicety. Two things fix it and both are here:
//
//   * FIT A SPHERE, NOT THE FRUSTUM CORNERS. The bounding sphere of a frustum
//     slice is invariant under camera rotation, so the cascade's extent stops
//     changing when the camera merely looks around. A tight box would be smaller
//     -- better texel density -- and would resize every frame, which is the
//     trade this makes deliberately.
//   * SNAP THE ORIGIN TO WHOLE TEXELS. Even with a fixed extent, translating the
//     camera slides the projection by a fraction of a texel and the sampled
//     depth flickers between neighbours. Quantising the light-space centre to
//     the texel grid means a moving camera shifts the map by whole texels only.
//
// The two together are what make a shadow edge sit still. Neither alone does.
// =============================================================================

#include "lrpch.h"

namespace X3
{

	/// FOUR, per ENGINE_PLAN.md. The count is fixed rather than configurable
	/// because it sizes the atlas, the matrix array in the camera UBO and the
	/// split array beside it, and a runtime-variable count would make all three
	/// dynamic to buy very little.
	inline constexpr uint32_t SHADOW_CASCADE_COUNT = 4;

	struct ShadowCascades
	{
		/// World -> light clip, one per cascade. Reverse-Z, matching every other
		/// projection in this engine, so the depth test is GREATER and the map
		/// clears to 0. An orthographic projection gains no precision from
		/// reverse-Z -- the depth distribution is already linear -- but matching
		/// is what keeps one set of pipeline defaults correct everywhere.
		std::array<glm::mat4, SHADOW_CASCADE_COUNT> viewProj{};

		/// VIEW-SPACE far distance of each cascade, ascending. The shading pass
		/// picks a cascade by comparing the fragment's view depth against these.
		std::array<float, SHADOW_CASCADE_COUNT> splitDepth{};

		/// Light-space texel world-size per cascade, for scaling normal-offset
		/// bias. A fixed bias cannot serve four cascades whose texels differ in
		/// world size by two orders of magnitude: enough for the far cascade
		/// detaches contact shadows in the near one.
		std::array<float, SHADOW_CASCADE_COUNT> texelWorldSize{};
	};

	/// Builds the cascades for one directional light.
	///
	/// `cameraTransform` is camera->world (this engine's +Z-forward convention),
	/// `lightDirection` points FROM the light -- the direction light travels,
	/// matching Gpu::LightData::direction and what SampleLightDirect negates.
	///
	/// `shadowMapResolution` is the side length of ONE cascade's square region in
	/// the atlas, not the atlas width, because it is the texel-snapping quantum.
	///
	/// `lambda` blends the two standard split schemes: 0 is uniform, 1 is
	/// logarithmic. Logarithmic matches perspective foreshortening and gives the
	/// near cascade far more of the depth range, which is where the eye is; pure
	/// logarithmic wastes the far cascades on almost nothing. 0.5-0.8 is the
	/// usual practical range and 0.75 is the default here.
	ShadowCascades ComputeShadowCascades(const glm::mat4& cameraTransform,
	                                     float focalLength, float aspect,
	                                     float nearPlane, float shadowFarPlane,
	                                     const glm::vec3& lightDirection,
	                                     uint32_t shadowMapResolution,
	                                     float lambda = 0.75f);

}
