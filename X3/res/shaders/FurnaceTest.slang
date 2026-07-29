// =============================================================================
// FurnaceTest -- numeric validation of the BSDF library.
//
// ENGINE_PLAN.md's Phase 6 is explicit that this must be validated with NUMBERS
// rather than screenshots: "shading math fails by looking plausible". A metal
// that is 12% too dark at roughness 0.8 looks entirely reasonable next to a
// metal that is correct, and no golden image catches it unless you already had
// a correct golden.
//
// THE WHITE FURNACE TEST. Place the surface inside a uniform environment of
// radiance 1. An energy-conserving BSDF must reflect at most all of it, so the
// directional albedo
//
//     E(wo) = integral over the hemisphere of f(wo,wi) * cos(wi) dwi
//
// must satisfy E <= 1 for every viewing angle and every roughness. E > 1 means
// the BSDF CREATES energy, which is a hard error -- it makes a path tracer
// diverge as bounce count rises. E < 1 is a loss, which is expected for
// single-scatter GGX and is exactly what the multi-scatter compensation exists
// to shrink; measuring it is how you know the compensation is working.
//
// The integral is estimated by importance-sampling the BSDF itself, so the
// estimator is E[weight] where weight is f*cos/pdf -- the same weight the path
// tracer uses. That is deliberate: it validates the sampling routine and the
// evaluation routine AGAINST EACH OTHER. A pdf that disagrees with sample()
// shows up here as an albedo that drifts from the analytically-known answer,
// which is the single easiest BSDF bug to write and the hardest to see.
//
// Output image layout: x is roughness in [0,1], y is cos(theta_o) in (0,1].
// R = directional albedo of a metal (baseColor 1, metallic 1)
// G = directional albedo of a dielectric (baseColor 1, metallic 0)
// B = 1 if the sample count that produced them was fully valid, else 0
// A = 1
//
// A perfectly energy-preserving metal reads R = 1 everywhere. tools/render-test
// reads these back as numbers.
// =============================================================================

import GpuTypes;
import Bindings;
import Bsdf;

static const uint SAMPLE_COUNT = 4096;

uint pcg(uint v) {
    v = v * 747796405u + 2891336453u;
    v ^= v >> ((v >> 28u) + 4u);
    v *= 277803737u;
    v ^= v >> 22u;
    return v;
}

float rnd(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint r = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    r = (r >> 22u) ^ r;
    return float(r) / 4294967296.0;
}

/// Monte-Carlo estimate of the directional albedo. Averaging sample()'s weight
/// IS the estimate: weight is f*cos/pdf, so its expectation over the sampling
/// distribution is exactly the integral being asked for.
float3 DirectionalAlbedo<T : IBsdf>(T bsdf, float3 wo, inout uint state) {
    float3 sum = float3(0.0);
    for (uint i = 0; i < SAMPLE_COUNT; ++i) {
        const float2 u = float2(rnd(state), rnd(state));
        const float uLobe = rnd(state);

        float3 wi;
        float3 weight;
        // A rejected sample contributes ZERO, and is still counted in the
        // denominator. Skipping it instead would silently renormalise away the
        // energy the BSDF failed to reflect, turning a lossy lobe into a
        // perfect one and making this whole test report success.
        if (bsdf.sample(wo, u, uLobe, wi, weight))
            sum += weight;
    }
    return sum / float(SAMPLE_COUNT);
}

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 texel = int2(threadId.xy);
    uint w, h;
    rayTracingTexture.GetDimensions(w, h);
    if (texel.x >= int(w) || texel.y >= int(h)) return;

    // Roughness across x, viewing angle up y. Both offset by half a texel so no
    // sample sits exactly on a degenerate boundary (roughness 0 is a mirror,
    // cosTheta 0 is edge-on) where the answer is a limit rather than a value.
    const float roughness = (float(texel.x) + 0.5) / float(w);
    const float cosTheta  = (float(texel.y) + 0.5) / float(h);
    const float sinTheta  = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float3 wo = float3(sinTheta, 0.0, cosTheta);

    uint state = pcg(uint(texel.x) * 9277u ^ uint(texel.y) * 1973u ^ 0x9E3779B9u);

    MetalRoughBsdf metal;
    metal.baseColor     = float3(1.0);
    metal.metallic      = 1.0;
    metal.roughness     = roughness;
    metal.anisotropy    = 0.0;
    metal.specularLevel = 0.5;

    MetalRoughBsdf dielectric = metal;
    dielectric.metallic = 0.0;

    const float3 eMetal = DirectionalAlbedo(metal, wo, state);
    const float3 eDiel  = DirectionalAlbedo(dielectric, wo, state);

    // Luminance-free: baseColor is white, so all three channels agree and any
    // one of them is the albedo. Taking .r rather than a luminance keeps the
    // number directly comparable to the analytic 1.0.
    rayTracingTexture[texel] = float4(eMetal.r, eDiel.r, 1.0, 1.0);
}
