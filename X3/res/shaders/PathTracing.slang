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
import Ddgi;

// Velocity is in UV units and a whole pixel of motion is ~0.0016 at 640 wide.
// 20x puts a 30-pixel-per-frame pan near saturation, which is fast motion.
static const float kVelocityDebugGain = 20.0;

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

/// Traces until the ray hits a surface that actually COVERS it, treating
/// Material::color.a as coverage rather than as a blend factor.
///
/// ALPHA AS COVERAGE IS THE MODEL, and it is the one that makes the rasterizer
/// checkable. With probability `1 - alpha` the ray passes straight through and
/// keeps going; averaged over samples that converges to exactly what
/// back-to-front alpha blending produces, so the transparent forward pass has a
/// reference to be compared against instead of only a golden of itself.
///
/// It also needs no sorting and no ordering assumptions, which is the whole
/// reason a path tracer is the right thing to define the answer here.
///
/// SHADOWS ARE STILL OPAQUE. IsInShadow does an any-hit traversal that never
/// resolves a material, so a transparent surface casts a solid shadow. That is
/// physically wrong and DELIBERATELY SHARED: both the reference and the raster
/// path call the same function, so the two still agree and the gate still means
/// something. Fixing it is a change to both at once, not to one.
///
/// The pass-through count is capped rather than unbounded -- a ray inside a
/// stack of transparent surfaces would otherwise loop until the shader was
/// killed, and a cap that is hit produces a slightly-too-opaque pixel instead.
public void TraceCovered(inout Ray ray, inout uint state) {
    for (uint k = 0; k < 8; k++) {
        CheckRayCollision(ray);
        if (ray.t >= INF_T) return;                   // missed everything

        const float alpha = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv).color.a;
        if (alpha >= 1.0 || RandomValue(state) < alpha) return;   // covered

        // Not covered: continue from just past the surface, same direction.
        // The bias is along the RAY, not the normal -- the ray is leaving
        // through the surface rather than reflecting off it.
        ray.origin = ray.origin + ray.dir * (ray.t + SURFACE_BIAS);
        ray.anyHit = false;
    }
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
        // TraceCovered, not CheckRayCollision: a surface with alpha below 1
        // covers the ray only some of the time, and when it does not the ray
        // continues through without spending a bounce.
        TraceCovered(ray, state);

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
    else if (u_Settings.debugMode == 3) {
        // THE DEPTH PREPASS'S OUTPUT. This exists so the rasterizer is gated on
        // something rather than merely not crashing: nothing else in the engine
        // reads the depth buffer yet, so without this the prepass could be
        // drawing nothing at all and every test would still pass.
        //
        // Reverse-Z: 1 is the near plane, 0 is the far plane and empty sky.
        //
        // SHOWN LOGARITHMICALLY, because the raw value is useless to look at.
        // Reverse-Z deliberately concentrates precision near the camera, so an
        // object eight units away sits at roughly near/8 = 0.006 -- one or two
        // levels out of 255. The first version of this displayed the raw value
        // and read as a completely black image, which is indistinguishable from
        // a prepass that drew nothing.
        //
        // near/depth recovers linear view distance; the log maps [near, far]
        // across the full range so silhouettes and ordering are both visible.
        const float2 uv = (float2(texelCoords) + 0.5) / float2(dims);
        const float d = u_SceneDepth.SampleLevel(uv, 0.0).r;
        if (d <= 0.0) {
            pixelColor = float3(0.0);          // far plane / nothing drawn
        } else {
            const float linearZ = u_Camera.nearPlane / d;
            const float t = log(linearZ / u_Camera.nearPlane)
                          / log(u_Camera.farPlane / u_Camera.nearPlane);
            pixelColor = float3(saturate(1.0 - t));   // near = bright
        }
    }
    else if (u_Settings.debugMode == 8) {
        // THE PROBE IRRADIANCE ATLAS ITSELF, stretched over the frame.
        //
        // Every other DDGI view answers "what does this surface receive"; this
        // answers "what do the probes actually contain", which is the only way
        // to tell a leak caused by bad probe DATA from one caused by bad probe
        // SELECTION. They look identical in a shaded image and have nothing in
        // common as bugs.
        //
        // Red channel of the alpha lane doubles as validity, so a fully black
        // tile is a probe that has never been written and a dark one is a probe
        // that legitimately sees nothing.
        const int2 a = int2(float2(texelCoords) / float2(dims)
                            * float2(X3_DDGI_IRRADIANCE_ATLAS_W, X3_DDGI_IRRADIANCE_ATLAS_H));
        pixelColor = u_DdgiIrradiance[clamp(a, int2(0),
            int2(X3_DDGI_IRRADIANCE_ATLAS_W - 1, X3_DDGI_IRRADIANCE_ATLAS_H - 1))].rgb;
    }
    else if (u_Settings.debugMode == 7) {
        // WHICH CASCADE a surface samples, and whether the shadow map agrees
        // with the traced shadow at all.
        //
        // Cascade index as a flat colour, DARKENED where the cascaded lookup
        // says shadowed. Two things are checkable at once: the split distances
        // land where ComputeShadowCascades says they do -- the bands are visible
        // and their boundaries measurable -- and the shadow shapes inside each
        // band match the traced ones in the shaded view.
        Ray probe = MakeCameraRay(texelCoords, dims);
        CheckRayCollision(probe);

        if (probe.t >= INF_T || u_Shadow.shadowLightIndex == X3_NO_SHADOW_LIGHT) {
            pixelColor = float3(0.0);
        } else {
            const float3 hitPoint = probe.origin + probe.dir * probe.t;
            const float viewDepth = mul(u_Camera.view, float4(hitPoint, 1.0)).z;

            uint c = X3_SHADOW_CASCADES;
            for (uint i = 0; i < X3_SHADOW_CASCADES; i++)
                if (viewDepth <= u_Shadow.splitDepth[i]) { c = i; break; }

            if (c >= X3_SHADOW_CASCADES) {
                pixelColor = float3(0.25);            // beyond the last cascade
            } else {
                const float3 kCascadeColor[4] = {
                    float3(1.0, 0.3, 0.3), float3(0.3, 1.0, 0.3),
                    float3(0.3, 0.5, 1.0), float3(1.0, 1.0, 0.3)
                };
                const float vis = SampleCascadedShadow(hitPoint, probe.normalGeom, viewDepth);
                pixelColor = kCascadeColor[c] * (0.25 + 0.75 * vis);
            }
        }
    }
    else if (u_Settings.debugMode == 6) {
        // THE VELOCITY BUFFER. Screen-space motion in UV units, so the values
        // are tiny -- a pixel of motion at 640 wide is 0.0016 -- and shown raw
        // it is a black frame, which is exactly what a velocity pass that never
        // ran looks like. The same trap the depth view fell into.
        //
        // Amplified and biased to 0.5: red is rightward motion, green downward,
        // and a static frame is a flat mid-grey rather than black. The gain is
        // fixed rather than normalised so two runs of different scenes are
        // directly comparable.
        const float2 uv = (float2(texelCoords) + 0.5) / float2(dims);
        const float2 v = u_SceneVelocity.SampleLevel(uv, 0.0).rg;
        pixelColor = float3(saturate(v * kVelocityDebugGain + 0.5), 0.5);
    }
    else if (u_Settings.debugMode == 4 || u_Settings.debugMode == 5) {
        // THE CLUSTERED LIGHT GRID, viewed two ways. Both need the primary hit,
        // so they share the trace and split after it.
        Ray probe = MakeCameraRay(texelCoords, dims);
        CheckRayCollision(probe);

        if (probe.t >= INF_T) {
            pixelColor = float3(0.0);                     // sky: no surface, no cluster
        } else {
            const float3 hitPoint = probe.origin + probe.dir * probe.t;
            const float3 posView  = mul(u_Camera.view, float4(hitPoint, 1.0)).xyz;
            const uint cluster = ClusterIndexFromView(posView, u_Camera.proj,
                                                      u_Camera.nearPlane, u_Camera.farPlane);

            if (cluster >= X3_CLUSTER_COUNT) {
                // A visible surface that maps to no cluster. Blue, because it is
                // neither a pass nor the failure mode mode 5 is looking for --
                // it means ClusterIndexFromView and the rasterizer disagree about
                // the frustum, which is a different bug.
                pixelColor = float3(0.0, 0.0, 1.0);
            } else if (u_Settings.debugMode == 4) {
                pixelColor = IntersectionsToRgb(ClusterLightGrid[cluster],
                                                max(u_Settings.lightCount, 1u));
            } else {
                // CULLING CORRECTNESS, not a picture. For every light that
                // actually reaches this point under the SAME predicate the
                // shading uses, check the point's cluster list contains it.
                //
                // Only omissions are errors. The cluster test is a sphere, so a
                // spot light whose cone misses the point is still listed -- that
                // is over-inclusion, which costs a little work and cannot change
                // the image.
                //
                // O(lights * listLength) per pixel and entirely a debug path.
                const uint base  = cluster * X3_MAX_LIGHTS_PER_CLUSTER;
                const uint count = ClusterLightGrid[cluster];
                bool missing = false;

                for (uint li = 0; li < u_Settings.lightCount; li++) {
                    const LightData light = LightBuffer[li];
                    const uint type = uint(light.position.w);

                    bool affects;
                    if (type == 0) {
                        affects = true;
                    } else {
                        // Exactly SampleLightDirect's early-out, inverted:
                        // it returns nothing when dist > range.
                        affects = length(light.position.xyz - hitPoint) <= light.color.w;
                    }
                    if (!affects) continue;

                    bool present = false;
                    for (uint j = 0; j < count; j++)
                        if (ClusterLightIndices[base + j] == li) present = true;

                    if (!present) missing = true;
                }

                pixelColor = missing ? float3(1.0, 0.0, 0.0)    // a light was culled that should not have been
                                     : float3(0.0, 1.0, 0.0);
            }
        }
    }
    else                                { pixelColor = IntersectionsToRgb(g_TriIntersectionCount, u_Settings.triHeatmapCutoff); }

    // Accumulate over multiple frames. THIS IS A READ-MODIFY-WRITE ON THE SAME
    // IMAGE, which is why Renderer::writeSlot pins the slot to 0 while
    // accumulating and why Draw() records a barrier before the dispatch.
    float weight = 1.0f / float(u_Settings.numAccumulatedFrames + 1);
    float3 previous = rayTracingTexture[texelCoords].rgb;
    float3 outputColor = previous * (1 - weight) + pixelColor * weight;
    rayTracingTexture[texelCoords] = float4(outputColor, 1.0f);
}
