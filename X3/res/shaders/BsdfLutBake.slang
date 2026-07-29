// =============================================================================
// BsdfLutBake -- precomputes the directional albedo of the GGX specular lobe.
//
// WHY A LUT AND NOT A FIT. Bsdf.slang previously used EnvBRDFApprox, Karis'
// analytic mobile fit, to decide how much energy the specular lobe takes. The
// furnace test measured that fit disagreeing with the actual lobe by 8.8% at
// grazing incidence -- a bias, not noise, confirmed by an eightfold sample
// increase moving it 0.003. An earlier attempt to ALSO derive multi-scatter
// compensation from the same fit pushed the error to 113%.
//
// This bakes the real thing. Because the integration uses the SAME
// SampleGGXVNDF and V_SmithGGXCorrelated that Bsdf.slang shades with, the table
// is exact for this lobe by construction rather than by resemblance. If the lobe
// changes, the table changes with it -- there is no fit to re-tune.
//
// THE SPLIT-SUM DECOMPOSITION. Schlick's Fresnel is affine in f0:
//     F = f0 + (1 - f0) * (1 - VdotH)^5
// so the directional albedo separates:
//     E(f0) = f0 * A + B
//     A = E[ (1 - (1-VdotH)^5) * G2/G1 ]
//     B = E[      (1-VdotH)^5  * G2/G1 ]
// under VNDF sampling, whose weight is exactly G2/G1. One table serves every
// material colour.
//
// Output: R = A, G = B, B = A + B (the white-furnace albedo, which is what the
// multi-scatter compensation divides by), A = 1.
// Axes: x = roughness in (0,1], y = NdotV in (0,1].
// =============================================================================

import GpuTypes;
import Bindings;
import Bsdf;

static const uint LUT_SAMPLES = 8192;

uint lutHash(uint v) {
    v = v * 747796405u + 2891336453u;
    v ^= v >> ((v >> 28u) + 4u);
    v *= 277803737u;
    v ^= v >> 22u;
    return v;
}

float lutRnd(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint r = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    r = (r >> 22u) ^ r;
    return float(r) / 4294967296.0;
}

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 texel = int2(threadId.xy);
    uint w, h;
    u_BsdfLut.GetDimensions(w, h);
    if (texel.x >= int(w) || texel.y >= int(h)) return;

    // Texel CENTRES, matching the bilinear fetch in Bsdf.slang. Sampling at
    // corners instead is the classic half-texel error that shows up as a
    // roughness-dependent brightness shift too small to notice and too large to
    // ignore.
    const float roughness = (float(texel.x) + 0.5) / float(w);
    const float NdotV     = (float(texel.y) + 0.5) / float(h);

    const float alpha = max(roughness * roughness, 1e-3);
    const float3 wo = float3(sqrt(max(0.0, 1.0 - NdotV * NdotV)), 0.0, NdotV);

    uint state = lutHash(uint(texel.x) * 9277u ^ uint(texel.y) * 1973u ^ 0x9E3779B9u);

    float A = 0.0;
    float B = 0.0;

    for (uint i = 0; i < LUT_SAMPLES; ++i) {
        const float2 u = float2(lutRnd(state), lutRnd(state));
        const float3 hv = SampleGGXVNDF(wo, alpha, alpha, u);
        const float3 wi = reflect(-wo, hv);
        if (wi.z <= 0.0) continue;

        const float VdotH = saturate(dot(wo, hv));

        // G2/G1 -- the VNDF sample weight, with the pdf already cancelled. This
        // is the same ratio the shading path's weight reduces to, which is what
        // makes the table exact for this lobe.
        const float G2 = 4.0 * wo.z * wi.z * V_SmithGGXCorrelated(wo.z, wi.z, alpha);
        const float G1 = 2.0 * wo.z / max(wo.z + sqrt(alpha * alpha + (1.0 - alpha * alpha) * wo.z * wo.z), 1e-6);
        const float weight = G2 / max(G1, 1e-6);

        const float Fc = pow(saturate(1.0 - VdotH), 5.0);
        A += (1.0 - Fc) * weight;
        B += Fc * weight;
    }

    A /= float(LUT_SAMPLES);
    B /= float(LUT_SAMPLES);

    u_BsdfLut[texel] = float4(A, B, A + B, 1.0);
}
