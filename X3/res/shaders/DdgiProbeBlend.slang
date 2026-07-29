// =============================================================================
// DdgiProbeBlend -- one thread per probe texel.
//
// Turns this frame's 64 ray samples into the probe's two octahedral maps:
//
//   IRRADIANCE (6x6 interior, 8x8 stored): for each texel direction, the
//   cosine-weighted mean radiance over the ray set. Stored value is
//   irradiance/pi -- see the convention banner over DdgiSampleIrradiance.
//
//   DEPTH (14x14 interior, 16x16 stored): for each texel direction, the mean
//   and MEAN-SQUARED distance to the first surface. Two moments, not one,
//   because the Chebyshev visibility test needs a variance and a variance needs
//   the second moment. Storing only the mean would make the test a hard
//   threshold, which bands savagely on any surface that is not a flat wall.
//
// Both are blended into last frame's values by a hysteresis factor. That
// temporal accumulation is not an optimisation -- it is what makes 64 rays per
// probe per frame enough. See the rotation banner in Ddgi.slang.
//
// DISPATCH: (X3_DDGI_PROBE_COUNT, 1, 1) WORKGROUPS of 16x16 -- one workgroup per
// probe, one thread per DEPTH texel of its 16x16 stored tile. The 64 threads in
// the top-left 8x8 corner ALSO each handle one irradiance texel of the 8x8
// stored tile. One dispatch rather than two because both maps integrate the same
// 64 rays, and reading those rays once into groupshared memory instead of twice
// out of the buffer is the whole reason to fuse them: 256 threads x 64 rays is
// 16384 buffer loads per probe if done naively, and 64 if done once.
//
// BINDINGS ASSUMED (all listed in full at the top of Ddgi.slang):
//   set 0 binding 11  RWTexture2D<float4>        u_DdgiIrradiance   (READ+WRITE)
//   set 0 binding 12  RWTexture2D<float2>        u_DdgiDepth        (READ+WRITE)
//   set 1 binding 3   ConstantBuffer<DdgiUBO>    u_Ddgi
//   set 2 binding 15  RWStructuredBuffer<float4> DdgiRayDataBuffer  (READ)
//
// THE READ-MODIFY-WRITE OF THE ATLASES IS SAFE AND IS NOT A HAZARD. Each texel
// is owned by exactly one thread, which reads its own previous value and writes
// its own new one; no thread ever touches another's. This is the same pattern
// PathTracing.slang's frame accumulator uses on rayTracingTexture, and it needs
// the same thing from the render graph: a barrier between the previous frame's
// write and this frame's read, and the atlases declared read-write on this pass.
// =============================================================================

import GpuTypes;
import Bindings;
import Trace;
import Ddgi;

// One probe's rays, hoisted out of the storage buffer once per workgroup.
// 64 * (16 + 12) = 1792 bytes of LDS, which constrains occupancy not at all.
//
// THE DIRECTIONS ARE CACHED TOO, not recomputed per thread. DdgiRayDirection is
// a frac, two trig calls, a sqrt and a quaternion rotation; 256 threads x 64
// rays would evaluate it 16384 times per probe to produce 64 distinct answers.
groupshared float4 s_rayData[X3_DDGI_RAYS_PER_PROBE];
groupshared float3 s_rayDir[X3_DDGI_RAYS_PER_PROBE];

[shader("compute")]
// 16 == X3_DDGI_DEPTH_TILE == X3_DDGI_DEPTH_TEXELS + 2. A literal because
// numthreads must be one; IF THE DEPTH TILE SIZE CHANGES, CHANGE THIS TOO.
// Getting it wrong does not fail to compile -- it leaves the outer rows of every
// depth tile unwritten, which is stale border data and therefore a seam.
[numthreads(16, 16, 1)]
void main(uint3 gid  : SV_GroupID,
          uint3 gtid : SV_GroupThreadID) {
    // BOTH OF THESE CONDITIONS ARE UNIFORM ACROSS THE WORKGROUP, which is what
    // makes returning before the barrier below legal. A per-thread early return
    // ahead of a GroupMemoryBarrierWithGroupSync is undefined behaviour and
    // shows up as a hang or as garbage LDS on some drivers and not others --
    // the worst class of bug this engine can ship.
    if (!DdgiEnabled()) return;
    const uint storageProbeIndex = gid.x;
    if (storageProbeIndex >= X3_DDGI_PROBE_COUNT) return;

    const uint rayCount = DdgiRayCount();
    const uint flat     = gtid.y * 16u + gtid.x;

    if (flat < X3_DDGI_RAYS_PER_PROBE) {
        if (flat < rayCount) {
            s_rayData[flat] = DdgiRayDataBuffer[DdgiRaySlot(storageProbeIndex, flat)];
            s_rayDir[flat]  = DdgiRayDirection(flat);
        } else {
            // Slots above the runtime budget are never read, but leaving them
            // uninitialised means any future loop that forgets the bound reads
            // whatever the last dispatch left in LDS.
            s_rayData[flat] = float4(0.0);
            s_rayDir[flat]  = float3(0.0);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // STORAGE coordinate, not logical. This pass never asks where the probe is
    // in the world -- only which tile is its own -- so it needs no unscrolling,
    // and doing it anyway would send every write to the wrong tile the moment
    // the volume scrolled.
    const int3 storage = DdgiStorageCoordFromIndex(storageProbeIndex);

    // FRAME 0 TAKES THE NEW VALUE OUTRIGHT. The atlas is cleared to zero at
    // creation, and blending a fresh probe towards zero at 0.97 would take
    // roughly two hundred frames to reach its correct value -- several seconds
    // of the scene fading up from black on every load, which reads as a bug.
    const float hysteresis = (DdgiFrameIndex() == 0u) ? 0.0 : DdgiHysteresis();

    // =========================================================================
    // IRRADIANCE -- the top-left 8x8 threads only.
    // =========================================================================
    if (gtid.x < X3_DDGI_IRRADIANCE_TILE && gtid.y < X3_DDGI_IRRADIANCE_TILE) {
        // BORDER TEXELS COMPUTE THEIR OWN ANSWER rather than copying one. The
        // mirror maps this texel to the interior texel that shares its
        // direction; running the identical blend on the identical inputs
        // produces the identical value, with no ordering between the two
        // threads and no second dispatch. Ddgi.slang's DdgiTileTexelToInterior
        // banner has the full argument, including why the C++ must zero-clear.
        const int2   interior = DdgiTileTexelToInterior(int2(gtid.xy),
                                                        X3_DDGI_IRRADIANCE_TEXELS);
        const float3 texDir   = DdgiInteriorTexelDirection(interior,
                                                           X3_DDGI_IRRADIANCE_TEXELS);

        // COSINE-WEIGHTED MEAN RADIANCE, and the maths behind the normalisation:
        //
        //   Rays are uniform on the SPHERE, so each carries solid angle 4pi/N.
        //   E(n) = integral over the hemisphere of L(w) (n.w) dw
        //        ~ (4pi/N) * sum_i max(0, n.w_i) L_i
        //   and  (4pi/N) * sum_i max(0, n.w_i) ~ integral of max(0, n.w) dw = pi
        //   so   E(n) ~ pi * [ sum_i w_i L_i / sum_i w_i ],  w_i = max(0, n.w_i)
        //
        // The bracketed term -- the weight-normalised mean -- is what gets
        // stored. Dividing by the ACTUAL sum of weights rather than by the
        // analytic pi is what makes 64 rays work: the analytic constant is only
        // correct in the limit. Measured over 2000 random texel directions with
        // this exact 64-ray spherical-Fibonacci set, (4pi/N)*sum(w) lands in
        // [0.984, 1.012] * pi. Using the constant would bake that +/-1.2%
        // per-direction error in as a FIXED mottling, and fixed error is the one
        // thing temporal accumulation cannot remove. Normalising by the realised
        // sum makes it cancel exactly.
        //
        // The pi is deliberately NOT reapplied. Stored value is irradiance/pi,
        // which is exactly what a Lambertian albedo multiplies.
        float3 sum     = float3(0.0);
        float  sumW    = 0.0;

        for (uint r = 0; r < rayCount; r++) {
            const float4 d = s_rayData[r];

            // BACKFACE RAYS ARE EXCLUDED, NOT COUNTED AS BLACK. They carry no
            // information about the light arriving at the probe -- they only say
            // the probe is partly inside something. Counting them as zero
            // radiance while still counting their weight would darken the probe
            // in proportion to how buried it is, which is a plausible-looking
            // wrong answer; excluding them leaves the probe reporting what its
            // unoccluded directions actually see, and lets the DEPTH map (which
            // does record them, shortened) be the thing that rejects the probe.
            if (DdgiRayIsBackface(d.w)) continue;

            const float w = max(0.0, dot(texDir, s_rayDir[r]));
            if (w <= 0.0) continue;

            sum  += d.rgb * w;
            sumW += w;
        }

        const int2 texel = DdgiTileOrigin(storage, X3_DDGI_IRRADIANCE_TILE) + int2(gtid.xy);

        // sumW is essentially never zero here -- a cosine lobe covers half the
        // sphere, so ~32 of 64 rays contribute -- but a probe entirely inside
        // geometry has every ray flagged backface and reaches this with nothing.
        // KEEPING THE PREVIOUS VALUE is right: "no measurement this frame" must
        // not be confused with "measured zero", which would make a buried probe
        // pull its neighbours' interpolation towards black.
        if (sumW > 1e-9) {
            const float3 previous = DdgiLoadIrradiance(texel);
            DdgiStoreIrradiance(texel, lerp(sum / sumW, previous, hysteresis));
        }
    }

    // =========================================================================
    // DEPTH MOMENTS -- all 256 threads, one per texel of the 16x16 stored tile.
    // =========================================================================
    {
        const int2   interior = DdgiTileTexelToInterior(int2(gtid.xy), X3_DDGI_DEPTH_TEXELS);
        const float3 texDir   = DdgiInteriorTexelDirection(interior, X3_DDGI_DEPTH_TEXELS);
        const float  sharpness = DdgiDepthSharpness();

        // A MUCH SHARPER LOBE THAN THE IRRADIANCE BLEND USES, and for a
        // different reason. Irradiance genuinely is a cosine integral over a
        // hemisphere, so the cosine weight is the physics. Depth is not an
        // integral of anything -- the map wants "the distance to the surface in
        // THIS direction", so the weight is a reconstruction kernel and it
        // should be as narrow as the sample count supports. Too wide and a wall
        // three metres away smears its distance across directions that see open
        // space, which softens the Chebyshev cutoff and leaks. Too narrow and
        // most texels get no sample at all -- see DdgiDepthSharpness for the
        // arithmetic tying it to the ray count.
        float2 sum  = float2(0.0);
        float  sumW = 0.0;

        for (uint r = 0; r < rayCount; r++) {
            const float w = pow(max(0.0, dot(texDir, s_rayDir[r])), sharpness);
            if (w <= 1e-9) continue;

            // abs(), because backface hits were stored negative-and-shortened by
            // the trace pass. The shortening is the point -- see
            // DdgiEncodeRayDistance -- and it must survive into the moments,
            // which is why the flag rides in the sign rather than in a separate
            // lane that this loop could forget to consult.
            const float dist = DdgiDecodeDepthDistance(s_rayData[r].w);

            sum  += float2(dist, dist * dist) * w;
            sumW += w;
        }

        const int2 texel = DdgiTileOrigin(storage, X3_DDGI_DEPTH_TILE) + int2(gtid.xy);

        // NO SAMPLE LANDED IN THIS TEXEL'S LOBE THIS FRAME. With 196 interior
        // directions fed by 64 rays this is the COMMON case, not an edge case,
        // and the correct response is to leave the texel alone: the ray set is
        // rotated every frame, so a texel skipped now is hit within a few
        // frames and its value is a rolling accumulation over many rotations.
        //
        // WRITING A FALLBACK HERE WOULD BE A LEAK GENERATOR. The obvious
        // fallback -- maxRayDistance, "nothing occludes me" -- fails OPEN: it
        // tells the Chebyshev test the probe can see straight through the wall,
        // in exactly the directions the ray set happened to miss this frame, and
        // the leak flickers as the rotation moves. Failing to update is the only
        // safe behaviour when there is no measurement.
        if (sumW > 1e-9) {
            const float2 previous = DdgiLoadDepth(texel);
            DdgiStoreDepth(texel, lerp(sum / sumW, previous, hysteresis));
        }
    }
}
