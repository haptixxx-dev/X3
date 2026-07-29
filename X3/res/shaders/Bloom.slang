// =============================================================================
// Bloom -- four passes over two half-resolution images.
//
// Extract -> blur horizontally -> blur vertically -> composite. A separable
// Gaussian: a 2D blur of radius r costs r^2 taps, and two 1D blurs of radius r
// cost 2r for the same result, which is the only reason a wide radius is
// affordable at all.
//
// HALF RESOLUTION, and that is not only a cost saving. Bloom is a low-frequency
// effect by definition, so the half-res buffer loses nothing visible -- and the
// bilinear upsample at composite time widens the kernel for free, which is worth
// more than the resolution it gave up.
//
// IT RUNS ON LINEAR RADIANCE, BEFORE THE TONEMAP. Bloom is light spilling in the
// lens, so it has to be applied to the light, not to the display-referred image
// the tonemap produces. Compositing after the tonemap adds a fixed amount of
// brightness regardless of how bright the source was, which is why post-tonemap
// bloom looks like fog rather than glare.
// =============================================================================

import GpuTypes;
import Bindings;
import ToneMapping;

struct BloomPush {
    uint  pass;        // 0 extract, 1 blur H, 2 blur V, 3 composite
    float threshold;   // linear radiance above which a pixel blooms
    float intensity;   // composite weight
    float _pad;
};
[[vk::push_constant]] BloomPush g_Bloom;

// Nine taps, sigma ~= 2.4 at half res. Precomputed rather than evaluated: the
// weights are constant and a per-tap exp() would be nine transcendentals per
// pixel per axis for a number the compiler cannot fold.
static const float kWeights[5] = { 0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162 };

float3 BlurAxis(RWTexture2D<float4> src, int2 uv, int2 dims, int2 axis) {
    float3 sum = src[uv].rgb * kWeights[0];
    for (int i = 1; i < 5; i++) {
        // CLAMPED, not wrapped. A storage-image load is unfiltered and
        // unaddressed -- there is no sampler to apply an address mode -- so an
        // out-of-range read is undefined rather than merely edge-repeated, and
        // wrapping would drag the far side of the screen into the blur.
        const int2 o = axis * i;
        sum += src[clamp(uv + o, int2(0), dims - 1)].rgb * kWeights[i];
        sum += src[clamp(uv - o, int2(0), dims - 1)].rgb * kWeights[i];
    }
    return sum;
}

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 p = int2(threadId.xy);

    uint fullX, fullY;
    rayTracingTexture.GetDimensions(fullX, fullY);
    const int2 full = int2(fullX, fullY);
    const int2 half2 = max(full / 2, int2(1));

    if (g_Bloom.pass == 3) {
        // COMPOSITE, at full resolution, adding the half-res blur back in.
        if (p.x >= full.x || p.y >= full.y) return;

        // Bilinear by hand: the bloom buffers are storage images and cannot be
        // sampled. Doing it point-sampled instead makes the bloom visibly blocky
        // at half res, which is the one artefact that gives the trick away.
        const float2 h = (float2(p) + 0.5) * 0.5 - 0.5;
        const int2 b = int2(floor(h));
        const float2 f = h - float2(b);
        const float3 c00 = u_BloomA[clamp(b + int2(0,0), int2(0), half2 - 1)].rgb;
        const float3 c10 = u_BloomA[clamp(b + int2(1,0), int2(0), half2 - 1)].rgb;
        const float3 c01 = u_BloomA[clamp(b + int2(0,1), int2(0), half2 - 1)].rgb;
        const float3 c11 = u_BloomA[clamp(b + int2(1,1), int2(0), half2 - 1)].rgb;
        const float3 bloom = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);

        rayTracingTexture[p] = float4(rayTracingTexture[p].rgb + bloom * g_Bloom.intensity, 1.0);
        return;
    }

    if (p.x >= half2.x || p.y >= half2.y) return;

    if (g_Bloom.pass == 0) {
        // EXTRACT and downsample. Four full-res taps averaged, so the
        // downsample is a box filter rather than a point sample -- point
        // sampling a half-res extract aliases every bright thin feature into
        // flickering dots the moment the camera moves.
        float3 sum = float3(0.0);
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
                sum += rayTracingTexture[clamp(p * 2 + int2(dx, dy), int2(0), full - 1)].rgb;
        float3 c = sum * 0.25;

        // SOFT KNEE rather than a hard cut. A hard threshold makes bloom appear
        // and vanish as a surface crosses it, which reads as flickering on any
        // moving highlight. This ramps in over the quarter-stop above it.
        const float lum = max(c.r, max(c.g, c.b));
        const float knee = max(g_Bloom.threshold * 0.25, 1e-4);
        const float w = saturate((lum - g_Bloom.threshold + knee) / (2.0 * knee));
        u_BloomA[p] = float4(c * w * w, 1.0);
        return;
    }

    if (g_Bloom.pass == 1) u_BloomB[p] = float4(BlurAxis(u_BloomA, p, half2, int2(1, 0)), 1.0);
    else                   u_BloomA[p] = float4(BlurAxis(u_BloomB, p, half2, int2(0, 1)), 1.0);
}
