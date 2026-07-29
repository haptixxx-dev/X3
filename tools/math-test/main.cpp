// =============================================================================
// X3MathTest -- assertions over the renderer's pure arithmetic.
//
// THE FIRST GATE IN THIS REPO THAT NEEDS NO GPU AND NO DISPLAY. verify.sh runs
// binaries and watches for validation messages; X3RenderTest compares images and
// needs a display. Neither can say whether a matrix is right, only whether the
// picture looks like the one from last time -- and a shadow cascade that is
// subtly wrong produces a picture that looks entirely plausible.
//
// Every check here is a property that must hold for ANY camera and ANY light,
// not a value recorded from a run. That distinction is the point: a golden image
// records what the code did, an assertion records what it is supposed to do.
//
// Exit code 0 only if every check passed.
// =============================================================================

#include "Renderer/ShadowCascades.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <string>

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

	// The engine's camera convention: +Z forward, built the same way a
	// TransformComponent would build it.
	glm::mat4 CameraAt(const glm::vec3& position, float yawDegrees, float pitchDegrees) {
		glm::mat4 m(1.0f);
		m = glm::translate(m, position);
		m = glm::rotate(m, glm::radians(yawDegrees),   glm::vec3(0, 1, 0));
		m = glm::rotate(m, glm::radians(pitchDegrees), glm::vec3(1, 0, 0));
		return m;
	}

	constexpr float kFocal  = 1.9209821f;   // 55 degrees horizontal
	constexpr float kAspect = 16.0f / 9.0f;
	constexpr float kNear   = 0.05f;
	constexpr float kFar    = 60.0f;
	constexpr uint32_t kRes = 1024;

	const glm::vec3 kLight = glm::normalize(glm::vec3(-0.4f, -0.8f, 0.45f));

	// -------------------------------------------------------------------------

	void TestSplitsAscend() {
		const auto c = X3::ComputeShadowCascades(CameraAt({0, 2, -5}, 0, 0),
			kFocal, kAspect, kNear, kFar, kLight, kRes);

		for (uint32_t i = 0; i < X3::SHADOW_CASCADE_COUNT; ++i)
			check(c.splitDepth[i] > (i == 0 ? kNear : c.splitDepth[i - 1]),
			      "cascade split " + std::to_string(i) + " is beyond the previous one");

		// The last split must reach the shadow far plane exactly, or geometry
		// between the last cascade and the far plane falls into no cascade at all
		// and is silently unshadowed.
		checkClose(c.splitDepth[X3::SHADOW_CASCADE_COUNT - 1], kFar, 1e-3f,
		           "the last cascade ends exactly at the shadow far plane");
	}

	// THE COVERAGE PROPERTY, and the one that actually matters: every corner of a
	// frustum slice must land inside its own cascade's clip volume. A cascade
	// that does not contain its slice leaves a wedge of the view unshadowed, and
	// it is invisible until something happens to stand in that wedge.
	void TestSlicesFitTheirCascades() {
		for (float yaw : { 0.0f, 37.0f, 180.0f, -95.0f }) {
			const glm::mat4 cam = CameraAt({1.5f, 2.0f, -4.0f}, yaw, -15.0f);
			const auto c = X3::ComputeShadowCascades(cam, kFocal, kAspect, kNear, kFar,
			                                         kLight, kRes);

			float sliceNear = kNear;
			for (uint32_t i = 0; i < X3::SHADOW_CASCADE_COUNT; ++i) {
				const float sliceFar = c.splitDepth[i];
				const float invX = 1.0f / kFocal;
				const float invY = 1.0f / (kAspect * kFocal);

				bool allInside = true;
				for (float z : { sliceNear, sliceFar })
					for (float sy : { -1.0f, 1.0f })
						for (float sx : { -1.0f, 1.0f }) {
							const glm::vec4 ws = cam * glm::vec4(sx * z * invX, sy * z * invY, z, 1.0f);
							const glm::vec4 clip = c.viewProj[i] * ws;
							// Orthographic, so w is 1 and there is no divide.
							if (std::abs(clip.x) > 1.0f + 1e-3f ||
							    std::abs(clip.y) > 1.0f + 1e-3f ||
							    clip.z < -1e-3f || clip.z > 1.0f + 1e-3f)
								allInside = false;
						}

				check(allInside, "yaw " + std::to_string(int(yaw)) + ": frustum slice " +
				                 std::to_string(i) + " lies inside its cascade");
				sliceNear = sliceFar;
			}
		}
	}

	// ANTI-SHIMMER, PART ONE. Rotating the camera must not resize a cascade. This
	// is the entire reason the fit uses a bounding sphere rather than a tight box
	// -- and if someone "optimises" it back to a box, this is what fails.
	void TestRotationDoesNotResize() {
		const glm::vec3 eye{ 0.0f, 2.0f, 0.0f };
		const auto ref = X3::ComputeShadowCascades(CameraAt(eye, 0, 0),
			kFocal, kAspect, kNear, kFar, kLight, kRes);

		for (float yaw : { 13.0f, 90.0f, 211.0f, -46.0f }) {
			const auto c = X3::ComputeShadowCascades(CameraAt(eye, yaw, 0),
				kFocal, kAspect, kNear, kFar, kLight, kRes);
			for (uint32_t i = 0; i < X3::SHADOW_CASCADE_COUNT; ++i)
				checkClose(c.texelWorldSize[i], ref.texelWorldSize[i], 1e-6f,
				           "yaw " + std::to_string(int(yaw)) + ": cascade " +
				           std::to_string(i) + " keeps its texel size under rotation");
		}
	}

	// ANTI-SHIMMER, PART TWO. Translating the camera must move the shadow map by
	// a WHOLE NUMBER OF TEXELS, so a fixed world point keeps landing on the same
	// place within a texel. Without the snap it slides by fractions and the
	// sampled depth flickers between neighbours, which is the crawling edge
	// everyone recognises and nobody can locate.
	//
	// Checked on the projection of a fixed world point, because that is the thing
	// that actually has to stay put -- not on the matrix, which can change freely
	// so long as this holds.
	void TestTranslationSnapsToTexels() {
		const glm::vec3 probe{ 3.0f, 0.0f, 7.0f };

		const auto a = X3::ComputeShadowCascades(CameraAt({0, 2, 0}, 25.0f, -10.0f),
			kFocal, kAspect, kNear, kFar, kLight, kRes);

		// Deliberately not a whole number of texels, and not axis-aligned to the
		// light: the snap has to work for an arbitrary movement or it works for
		// nothing.
		const auto b = X3::ComputeShadowCascades(CameraAt({0.137f, 2.0f, 0.041f}, 25.0f, -10.0f),
			kFocal, kAspect, kNear, kFar, kLight, kRes);

		for (uint32_t i = 0; i < X3::SHADOW_CASCADE_COUNT; ++i) {
			const glm::vec4 pa = a.viewProj[i] * glm::vec4(probe, 1.0f);
			const glm::vec4 pb = b.viewProj[i] * glm::vec4(probe, 1.0f);

			// Clip [-1,1] to texels.
			const float half = 0.5f * float(kRes);
			const float dx = (pb.x - pa.x) * half;
			const float dy = (pb.y - pa.y) * half;

			const float ex = std::abs(dx - std::round(dx));
			const float ey = std::abs(dy - std::round(dy));

			checkClose(ex, 0.0f, 1e-2f, "cascade " + std::to_string(i) +
			           " shifts by a whole number of texels in x under camera translation");
			checkClose(ey, 0.0f, 1e-2f, "cascade " + std::to_string(i) +
			           " shifts by a whole number of texels in y under camera translation");
		}
	}

	// A light pointing straight down makes the obvious up vector degenerate and
	// lookAt returns NaNs -- which propagate into every cascade matrix and blank
	// the shadow map with no error reported anywhere.
	void TestDegenerateLightDirection() {
		for (const glm::vec3& L : { glm::vec3(0, -1, 0), glm::vec3(0, 1, 0) }) {
			const auto c = X3::ComputeShadowCascades(CameraAt({0, 3, -6}, 20.0f, -20.0f),
				kFocal, kAspect, kNear, kFar, L, kRes);
			bool finite = true;
			for (uint32_t i = 0; i < X3::SHADOW_CASCADE_COUNT; ++i)
				for (int col = 0; col < 4; ++col)
					for (int row = 0; row < 4; ++row)
						if (!std::isfinite(c.viewProj[i][col][row])) finite = false;
			check(finite, "a straight-" + std::string(L.y < 0 ? "down" : "up") +
			              " light produces finite cascade matrices");
		}
	}

} // namespace

int main() {
	std::printf("X3MathTest\n");

	TestSplitsAscend();
	TestSlicesFitTheirCascades();
	TestRotationDoesNotResize();
	TestTranslationSnapsToTexels();
	TestDegenerateLightDirection();

	if (g_Failures == 0) {
		std::printf("  \033[32m%d checks passed\033[0m\n", g_Checks);
		return 0;
	}
	std::printf("  \033[31m%d of %d checks failed\033[0m\n", g_Failures, g_Checks);
	return 1;
}
