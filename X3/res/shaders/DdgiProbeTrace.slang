// =============================================================================
// DdgiProbeTrace -- one thread per (probe, ray).
//
// THE PASS THAT MAKES DDGI FREE ON THIS ENGINE. Everything below is ray
// generation and shading; the intersection itself is CheckRayCollision from
// Trace.slang, unchanged, the same function the path tracer, PBR and the shadow
// path already call. ENGINE_PLAN.md calls this "the cleanest structural reuse in
// the entire plan" and it is literally one call.
//
// Each thread fires one ray from its probe, shades whatever it hits, and writes
// (radiance, signed distance) into the probe's slot of DdgiRayDataBuffer.
// DdgiProbeBlend then integrates those 64 samples into the probe's two maps.
//
// DISPATCH: (X3_DDGI_PROBE_COUNT, 1, 1) WORKGROUPS of 64 threads -- one
// workgroup per probe, one thread per ray. That grouping is not arbitrary: every
// lane in a workgroup then shares a ray ORIGIN, so their BVH traversals descend
// through the same top-of-tree nodes and diverge only deep down. On a software
// BVH where every lane walks its own explicit stack, origin coherence is the
// only coherence available, and scattering rays from different probes across a
// workgroup would throw it away for nothing.
//
// BINDINGS ASSUMED (all listed in full at the top of Ddgi.slang):
//   set 0 binding 11   RWTexture2D<float4>        u_DdgiIrradiance   (READ here)
//   set 1 binding 3    ConstantBuffer<DdgiUBO>    u_Ddgi
//   set 2 binding 15   RWStructuredBuffer<float4> DdgiRayDataBuffer  (WRITE here)
// plus everything CheckRayCollision and DirectLighting already need, which is
// already bound.
//
// PASS ORDER MATTERS AND IS NOT ENFORCEABLE FROM HERE:
//   ... -> [this pass, reads irradiance atlas] -> barrier -> [blend, writes it]
// The irradiance this pass reads is therefore LAST FRAME'S. That is the entire
// infinite-bounce mechanism -- see the banner over the bounce term below -- and
// it works because of the ordering, not because anything was copied.
// =============================================================================

import GpuTypes;
import Bindings;
import Trace;
import Ddgi;

// -----------------------------------------------------------------------------
/// Radiance leaving a probe-ray hit point back towards the probe.
///
/// GENERIC OVER IBsdf, matching PathTracing.slang's TracePath. Slang specialises
/// it at link time, so the common metallic-roughness instantiation carries no
/// clearcoat or sheen code at all. The tier split is duplicated here rather than
/// forced to the base tier because the probes must agree with the reference
/// renderer about what a material looks like -- a coated material that the path
/// tracer shades with a coat and the probes shade without is exactly the kind of
/// quiet divergence Phase 6 built the shared BSDF library to prevent.
// -----------------------------------------------------------------------------
float3 ShadeProbeHit<T : IBsdf>(T bsdf, Ray ray, Material mat, ShadingFrame frame,
                                float3 hitPoint) {
    float3 L = mat.emission.rgb * mat.emission.w;

    // DIRECT LIGHTING THROUGH THE SHARED LOOP, and note what is NOT set:
    // Trace.slang's g_UseShadowMaps stays false here, so every light traces a
    // real shadow ray.
    //
    // THAT IS MANDATORY, NOT A DEFAULT. The cascaded shadow map covers the
    // camera frustum and nothing else, and probes are updated across the whole
    // volume including behind the camera and outside the view. Sampling the
    // cascades from a probe outside their coverage returns "fully lit" for
    // anything past the last cascade (see SampleCascadedShadow) -- so every
    // probe behind the camera would fill with unshadowed sunlight, and the GI in
    // the room would swing as the camera turned around. Tracing costs more and
    // is the only correct answer for a pass that is not camera-bound.
    L += DirectLighting(bsdf, frame, -ray.dir, hitPoint, ray.normalGeom);

    // ---- ONE BOUNCE, WHICH BUYS INFINITE BOUNCES -----------------------------
    //
    // The hit point is shaded with LAST FRAME'S probe irradiance. Last frame's
    // probes already contained the frame before's bounce, and so on, so the
    // series is geometric: after n frames the probes carry n bounces of light
    // transport for the cost of one. This is what makes DDGI look like a full GI
    // solution rather than a one-bounce ambient term, and it is the single
    // highest-value line in the pass.
    //
    // ENERGY PRESERVATION IS BELOW 1 AND MUST BE. The feedback loop's gain per
    // frame is roughly albedo * energyPreservation. In a white room (albedo near
    // 1) a gain of exactly 1 is a marginally stable loop: the room brightens
    // every frame, slowly, without bound, until everything saturates to white
    // and the artist concludes the lights are too strong. A value around 0.85
    // makes the series converge while still delivering several effective
    // bounces. It is a stability constant, not a look control.
    //
    // bsdf.albedo() RATHER THAN mat.color.rgb. The probes store irradiance, so
    // the bounce is necessarily treated as Lambertian whatever lobe the material
    // has -- but the interface's own albedo() answer folds metallic into it
    // (lerp(baseColor, f0, metallic)), so a metal bounces its own tinted
    // reflectance instead of bouncing white light like chalk. That whitening was
    // the exact bug Phase 6's DirectLighting rewrite existed to kill; it should
    // not come back through the GI path.
    //
    // DdgiSampleIrradiance returns irradiance/pi, which is precisely the factor
    // a Lambertian albedo multiplies. No pi appears here, deliberately.
    L += bsdf.albedo()
       * DdgiSampleIrradiance(hitPoint, ray.normalShade, -ray.dir)
       * DdgiEnergyPreservation();

    return L;
}

[shader("compute")]
// 64 == X3_DDGI_RAYS_PER_PROBE. Kept as a literal because numthreads must be a
// literal in every backend this may target; if that constant changes, THIS LINE
// MUST CHANGE WITH IT and the bounds check below is what will otherwise silently
// drop or duplicate rays.
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    // TWO-SIDED, and DDGI is the only caller that needs it. A probe can be
    // inside geometry, and with the default single-sided intersector its rays
    // pass straight through the solid and fill it with sky -- see
    // g_TwoSidedTrace in Trace.slang for the measurement that found this.
    g_TwoSidedTrace = true;

    const uint flat = tid.x;
    const uint storageProbeIndex = flat / X3_DDGI_RAYS_PER_PROBE;
    const uint rayIndex          = flat % X3_DDGI_RAYS_PER_PROBE;

    if (storageProbeIndex >= X3_DDGI_PROBE_COUNT) return;
    if (!DdgiEnabled()) return;

    // Rays above the runtime budget are not traced. Their buffer slots keep last
    // frame's values and the blend pass never reads them, because both passes
    // ask DdgiRayCount() rather than deciding for themselves.
    if (rayIndex >= DdgiRayCount()) return;

    const int3   logical  = DdgiLogicalCoordFromStorageIndex(storageProbeIndex);
    const float3 probePos = DdgiProbeWorldPosition(logical);

    Ray ray;
    ray.origin = probePos;
    ray.dir    = DdgiRayDirection(rayIndex);
    ray.t      = INF_T;
    // FULL ATTRIBUTE RESOLVE, not an occlusion query. A probe ray needs the
    // material, the UV and the shading normal at the hit, so it cannot take
    // IsInShadow's cheap any-hit path.
    ray.anyHit = false;

    CheckRayCollision(ray);

    float3 radiance;
    float  distance;

    if (ray.t >= INF_T) {
        // MISSED EVERYTHING -- the probe sees sky in this direction. This is
        // where a probe volume gets its ambient fill from, and it is why an
        // outdoor scene needs no separate ambient hack.
        radiance = GetSkyboxLight(ray.dir);
        distance = DdgiEncodeRayDistance(0.0, false, true);
    }
    else if (!ray.frontFacing) {
        // HIT A BACKFACE: this ray started inside geometry, or the surface is
        // one-sided and the probe is behind it. It carries NO light -- shading
        // the inside of a wall would produce whatever the material happens to
        // look like lit from the wrong side, and pouring that into the probe is
        // how a probe buried in a wall ends up glowing.
        //
        // The distance is encoded negative and shortened, which is what tells
        // the depth map "there is a surface right next to me here" and makes the
        // Chebyshev test reject this probe for the room outside. See
        // DdgiEncodeRayDistance for the full argument -- it is the primary
        // defence against a probe inside geometry.
        radiance = float3(0.0);
        distance = DdgiEncodeRayDistance(ray.t, true, false);
    }
    else {
        const float3 hitPoint = ray.origin + ray.dir * ray.t;
        const Material    mat = SampleMaterial(MaterialBuffer[ray.materialIdx], ray.uv);
        const MaterialExt ext = GetMaterialExt(mat);

        // Same shading-normal guard PathTracing.slang documents at length: a
        // normal-mapped or interpolated normal can face away from the incoming
        // ray, and a BSDF evaluated in that frame reports zero for every lobe.
        // The pixel there is not dark, it is BLACK -- and in a probe that black
        // sample is averaged into 63 correct ones and shows as an unexplained
        // dark patch in the GI that moves as the ray rotation moves.
        ShadingFrame frame = ShadingFrame.make(ray.normalShade, ray.tangent);
        if (dot(-ray.dir, ray.normalShade) <= 0.0)
            frame = ShadingFrame.make(ray.normalGeom, ray.tangent);

        if (MaterialHasExtendedLobes(mat)) {
            radiance = ShadeProbeHit(MakeCoatedBsdf(mat, ext), ray, mat, frame, hitPoint);
        } else {
            radiance = ShadeProbeHit(MakeBaseBsdf(mat, ext), ray, mat, frame, hitPoint);
        }

        distance = DdgiEncodeRayDistance(ray.t, true, true);
    }

    // NaN AND INF GUARD, at the one place all three branches meet.
    //
    // Not defensive programming for its own sake. A single NaN reaching the ray
    // buffer propagates through the blend's weighted sum into the probe texel,
    // where HYSTERESIS PRESERVES IT FOREVER -- lerp(NaN, x, h) is NaN. One bad
    // sample from one grazing-angle division permanently kills a probe, and the
    // symptom is a fixed dark or white blotch in the GI that survives reloading
    // the scene. Clamping here costs three instructions.
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = float3(0.0);
    if (isnan(distance) || isinf(distance))           distance = DdgiMaxRayDistance();

    DdgiRayDataBuffer[DdgiRaySlot(storageProbeIndex, rayIndex)] =
        float4(radiance, distance);
}
