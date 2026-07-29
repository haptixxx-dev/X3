// Thanks:
// jakubg05 on github (This is from the Laura project)
// Sebastian Lague: https://www.youtube.com/watch?v=Qz0KTGYJtUk
// https://jacco.ompf2.com/2022/04/13/how-to-build-a-bvh-part-1-basics/
//
// The reference renderer. Everything below the entry point that is not
// path-tracing-specific -- traversal, intersection, attribute resolution,
// material sampling -- lives in Trace.slang and is shared with PBR and Phong.

import GpuTypes;
import Bindings;
import Trace;

static const float GAMMA = 0.8;

// Wavelength-ish ramp for the BVH heatmap debug modes.
float3 IntersectionsToRgb(uint intersections, uint cutoff) {
    float t = clamp(float(intersections) / max(1.0, float(cutoff)), 0.0, 1.0);
    float w = 380.0 + 370.0 * t;
    float3 color;
    if (w <= 380.0) { color = float3(0.0); }
    else if (w > 380.0 && w <= 440.0) { color = float3((440.0 - w) / (440.0 - 380.0) / 3.0, 0.0, 0.8); }
    else if (w <= 490.0) { color = float3(0.0, (w - 440.0) / (490.0 - 440.0), 1.0); }
    else if (w <= 510.0) { color = float3(0.0, 1.0, (510.0 - w) / (510.0 - 490.0)); }
    else if (w <= 580.0) { color = float3((w - 510.0) / (580.0 - 510.0), 1.0, 0.0); }
    else if (w <= 645.0) { color = float3(1.0, (645.0 - w) / (645.0 - 580.0), 0.0); }
    else if (w <= 780.0) { color = float3(1.0, 0.0, 0.0); }
    else { color = float3(1.0); }

    float factor;
    if (w >= 380.0 && w < 420.0) { factor = 0.3 + 0.7 * (w - 380.0) / (420.0 - 380.0); }
    else if (w >= 420.0 && w < 701.0) { factor = 1.0; }
    else if (w >= 701.0 && w < 781.0) {
        factor = 0.3 + 0.7 * (780.0 - w) / (780.0 - 700.0);
        return pow(color + factor * float3(1.0), float3(GAMMA));
    }
    else { factor = 1.0; }
    return pow(factor * color, float3(GAMMA));
}

uint pcg_hash(uint v) {
    v = v * 747796405u + 2891336453u;
    v ^= v >> ((v >> 28u) + 4u);
    v *= 277803737u;
    v ^= v >> 22u;
    return v;
}

uint InitRngState(int2 pixel, uint frame, uint raySampleIdx) {
    uint s = uint(pixel.x) * 1973u
           ^ uint(pixel.y) * 9277u
           ^ frame          * 26699u
           ^ raySampleIdx   * 374761393u;
    s ^= 0x9E3779B9u;   // golden ratio const
    return pcg_hash(s);
}

/// PCG-based uniform value in [0,1).
/// https://www.pcg-random.org, https://www.shadertoy.com/view/XlGcRh
float RandomValue(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint result = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    result = (result >> 22u) ^ result;
    return float(result) / 4294967295.0;
}

/// Box-Muller transform. Thanks to https://stackoverflow.com/a/6178290
float RandomValueNormalDistribution(inout uint state) {
    float theta = 2 * PI * RandomValue(state);
    float rho = sqrt(-2 * log(RandomValue(state)));
    return rho * cos(theta);
}

/// https://math.stackexchange.com/questions/1585975
float3 RandomDirection(inout uint state) {
    float x = RandomValueNormalDistribution(state);
    float y = RandomValueNormalDistribution(state);
    float z = RandomValueNormalDistribution(state);
    return normalize(float3(x, y, z));
}

float3 TracePath<T : IBsdf>(T bsdf, inout Ray ray, inout uint state,
                            Material mat, inout float3 rayColor, out bool terminated) {
    terminated = false;
    float3 contribution = float3(0.0);

    const float3 hitPoint = ray.origin + ray.dir * ray.t;
    const float3 woWorld = -ray.dir;

    // THE SHADING NORMAL CAN FACE AWAY FROM THE VIEWER, and a BSDF written in
    // the local frame has no answer for that: wo.z <= 0 means every lobe
    // reports zero, sample() fails, and the path terminates carrying nothing.
    // The pixel comes out BLACK -- not dark, black -- while the geometry is
    // plainly visible.
    //
    // It happens wherever an interpolated or normal-mapped shading normal
    // disagrees with the geometry it sits on, which is most obvious at grazing
    // angles on coarse faceted meshes. The lights fixture found it on a
    // 24-segment cylinder, whose sides went black in the path tracer while PBR
    // -- which falls back to an ambient term rather than terminating -- showed
    // them lit.
    //
    // Falling back to the geometric normal is the standard remedy: it is
    // guaranteed to face the viewer for any triangle the ray actually hit,
    // because CheckRayCollision already flipped it to. The alternative,
    // terminating the path, throws away energy the surface really does receive.
    ShadingFrame frame = ShadingFrame.make(ray.normalShade, ray.tangent);
    if (dot(woWorld, ray.normalShade) <= 0.0)
        frame = ShadingFrame.make(ray.normalGeom, ray.tangent);

    contribution += mat.emission.rgb * mat.emission.w * rayColor;
    contribution += DirectLighting(bsdf, frame, woWorld, hitPoint, ray.normalGeom) * rayColor;

    // IMPORTANCE SAMPLE THE BSDF for the next bounce, rather than the cosine
    // hemisphere this loop used before Phase 6. On a smooth metal a cosine
    // sample almost never lands in the specular lobe, so the estimator's
    // variance was enormous and the surface converged to something matte no
    // matter how long it ran.
    const float3 wo = frame.toLocal(woWorld);
    float3 wi;
    float3 weight;
    const float2 u = float2(RandomValue(state), RandomValue(state));
    const float uLobe = RandomValue(state);

    if (!bsdf.sample(wo, u, uLobe, wi, weight)) {
        terminated = true;
        return contribution;
    }

    rayColor *= weight;

    ray.origin = hitPoint + ray.normalGeom * SURFACE_BIAS;
    ray.dir = normalize(frame.toWorld(wi));
    ray.anyHit = false;

    // A ray below the geometric horizon would immediately re-hit the surface it
    // came from. Terminating is correct rather than clamping: the sample carries
    // no energy either way, and clamping would bias it upward.
    if (dot(ray.dir, ray.normalGeom) <= 0.0) terminated = true;
    return contribution;
}

float3 TraceRay(inout Ray ray, inout uint state) {
    float3 rayColor = float3(1.0);
    float3 brightness_score = float3(0.0);

    for (uint i = 0; i < u_Settings.bouncesPerRay; i++) {
        CheckRayCollision(ray);

        if (ray.t >= INF_T) {
            brightness_score += GetSkyboxLight(ray.dir) * rayColor;
            break;
        }

        const Material mat = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv);
        const MaterialExt ext = GetMaterialExt(mat);

        bool terminated = false;

        // THE TIER SPLIT. Both branches are the same generic function; Slang
        // specializes each at link time, so the common path carries no clearcoat
        // or sheen code at all rather than branching past it per pixel.
        if (MaterialHasExtendedLobes(mat)) {
            brightness_score += TracePath(MakeCoatedBsdf(mat, ext), ray, state, mat, rayColor, terminated);
        } else {
            brightness_score += TracePath(MakeBaseBsdf(mat, ext), ray, state, mat, rayColor, terminated);
        }
        if (terminated) break;

        // Russian roulette once the throughput is low. Unbiased -- the survivors
        // are scaled by 1/p -- and it is what stops a dark scene spending its
        // whole budget on paths that cannot contribute.
        if (i >= 2) {
            const float p = clamp(max(rayColor.r, max(rayColor.g, rayColor.b)), 0.05, 1.0);
            if (RandomValue(state) > p) break;
            rayColor /= p;
        }
    }
    return brightness_score;
}

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    int2 texelCoords = int2(threadId.xy);
    uint dimX, dimY;
    rayTracingTexture.GetDimensions(dimX, dimY);
    int2 dims = int2(dimX, dimY);
    if (texelCoords.x >= dims.x || texelCoords.y >= dims.y) { return; }

    Ray cameraRay = MakeCameraRay(texelCoords, dims);

    float3 pixelColor = float3(0.0);
    for (uint i = 0; i < u_Settings.raysPerPixel; i++) {
        uint state = InitRngState(texelCoords, u_Settings.numAccumulatedFrames, i);
        Ray ray = cameraRay;
        pixelColor += TraceRay(ray, state);
    }

    if (u_Settings.debugMode == 0)      { pixelColor = pixelColor / float(u_Settings.raysPerPixel); }
    else if (u_Settings.debugMode == 1) { pixelColor = IntersectionsToRgb(g_AabbIntersectionCount, u_Settings.aabbHeatmapCutoff); }
    else                                { pixelColor = IntersectionsToRgb(g_TriIntersectionCount, u_Settings.triHeatmapCutoff); }

    // Accumulate over multiple frames. THIS IS A READ-MODIFY-WRITE ON THE SAME
    // IMAGE, which is why Renderer::writeSlot pins the slot to 0 while
    // accumulating and why Draw() records a barrier before the dispatch.
    float weight = 1.0f / float(u_Settings.numAccumulatedFrames + 1);
    float3 previous = rayTracingTexture[texelCoords].rgb;
    float3 outputColor = previous * (1 - weight) + pixelColor * weight;
    rayTracingTexture[texelCoords] = float4(outputColor, 1.0f);
}
