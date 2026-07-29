// =============================================================================
// MotionBlur -- two passes: a half-resolution gather along the velocity vector,
// then a full-resolution composite.
//
// The velocity buffer already exists (Velocity.slang, Phase 7) and holds
// (current UV - previous UV) per pixel, so the segment a surface swept during
// the frame is known exactly. Sampling the colour buffer along that segment and
// averaging is the whole effect.
//
// ---- WHAT THIS FILE NEEDS ---------------------------------------------------
//
// Bindings, all ALREADY DECLARED in Bindings.slang. Nothing new:
//
//   binding 0, set 0  rayTracingTexture  RWTexture2D<float4>
//                     The render target, full resolution. Pass 0 reads it along
//                     the velocity line; pass 1 reads its OWN texel and writes
//                     that same texel.
//   binding 5, set 0  u_SceneVelocity    Sampler2D
//                     (current UV - previous UV), UV units. Pass 0 only.
//   binding 7, set 0  u_BloomA           RWTexture2D<float4>, HALF resolution
//                     Pass 0 writes it: rgb = blurred colour, a = blur strength
//                     in [0,1]. Pass 1 reads it.
//
// u_BloomB is deliberately NOT used -- two passes need one scratch image, and
// leaving the second free means depth of field and motion blur can be reordered
// without a new conflict.
//
// REUSING THE BLOOM PING-PONG IS AN ASSUMPTION. Bloom runs before the tonemap,
// motion blur runs after TAA, so u_BloomA is dead by the time this dispatches.
// DepthOfField.slang reuses the same two images on the same reasoning, so the
// two effects must run one after the other and never interleaved: all of one
// effect's passes, then all of the other's. The render graph's readWrite
// declaration on bloomA is what orders them.
//
// Push constant, 16 bytes, which is the whole budget -- Renderer.cpp declares
// pushConstantSize = 16 for every compute pipeline, so a fifth field would
// change the layout for every compute shader in the engine. The tap count is
// therefore a compile-time constant rather than a parameter.
//
//   uint  pass         0 gather (half res), 1 composite (full res)
//   float maxBlurUv    hard cap on blur length, in UV units. 0.02-0.04 is
//                      sane; 1.0 is the width of the screen.
//   float intensity    shutter fraction; scales the velocity before the cap.
//                      1.0 blurs over the full frame interval, 0 disables.
//   float _pad         unused, keeps the struct at 16 bytes on both sides
//
// ---- WHY THE CAP IS NOT OPTIONAL --------------------------------------------
// Velocity is unbounded. A fast camera pan, a projectile crossing the frame, or
// a disocclusion where the previous-frame position of a surface is meaningless
// all produce vectors of a large fraction of the screen -- and one frame of
// mouse-flick during a level load can produce several screens' worth.
//
// With a variable tap count that is an unbounded cost: the taps needed to cover
// a segment scale with its length, and one such frame stalls the GPU. With the
// FIXED tap count used here it is worse in a different way -- the taps simply
// spread further apart, so past about one tap per pixel the blur stops being a
// smear and becomes kTapCount recognisable ghost copies of the image, evenly
// spaced across the screen. Neither failure is subtle and both are avoided by
// refusing to blur further than maxBlurUv, which merely under-blurs the fastest
// motion. Under-blurring is the artefact nobody notices.
// =============================================================================

import GpuTypes;
import Bindings;

struct MotionBlurPush {
    uint  pass;
    float maxBlurUv;
    float intensity;
    float _pad;
};
[[vk::push_constant]] MotionBlurPush g_Mb;

// ODD, so one tap lands exactly on the centre and a stationary pixel is
// unchanged rather than being a 50/50 blend of its two nearest neighbours.
static const uint kTapCount = 15;

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 p = int2(threadId.xy);

    uint fullX, fullY;
    rayTracingTexture.GetDimensions(fullX, fullY);
    const int2 full  = int2(fullX, fullY);
    const int2 half2 = max(full / 2, int2(1));

    // -------------------------------------------------------------------------
    // PASS 1 -- COMPOSITE, at full resolution.
    //
    // A SECOND PASS RATHER THAN BLURRING IN PLACE. Pass 0 reads a whole line of
    // the render target and pass 1 writes it; doing both in one dispatch is the
    // unordered read/write hazard Taa.slang was split in two to avoid -- another
    // invocation may overwrite a texel before this one loads it, which does not
    // crash, does not trip validation, and makes the output differ between runs
    // of the same build. Here pass 1 touches only its OWN texel of the target,
    // which is ordered within the invocation and therefore safe.
    // -------------------------------------------------------------------------
    if (g_Mb.pass == 1) {
        if (p.x >= full.x || p.y >= full.y) return;

        // Bilinear by hand, as Bloom.slang does: u_BloomA is a storage image and
        // storage images cannot be sampled. Point-sampling the half-res layer
        // makes every blurred region blocky, which is the artefact that gives
        // away that the blur was computed at half resolution.
        const float2 h = (float2(p) + 0.5) * 0.5 - 0.5;
        const int2   b = int2(floor(h));
        const float2 f = h - float2(b);
        const float4 c00 = u_BloomA[clamp(b + int2(0,0), int2(0), half2 - 1)];
        const float4 c10 = u_BloomA[clamp(b + int2(1,0), int2(0), half2 - 1)];
        const float4 c01 = u_BloomA[clamp(b + int2(0,1), int2(0), half2 - 1)];
        const float4 c11 = u_BloomA[clamp(b + int2(1,1), int2(0), half2 - 1)];
        const float4 blurred = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);

        // BLENDED BY THE STRENGTH PASS 0 MEASURED, not applied everywhere. A
        // static part of the frame must come out bit-identical to what it was:
        // replacing it with the half-res layer would soften the entire image
        // whenever anything anywhere moved, and a motion blur that blurs a
        // stationary scene reads as a broken resolve, not as motion.
        const float3 sharp = rayTracingTexture[p].rgb;
        rayTracingTexture[p] = float4(lerp(sharp, blurred.rgb, saturate(blurred.a)), 1.0);
        return;
    }

    // -------------------------------------------------------------------------
    // PASS 0 -- THE GATHER, at half resolution, into u_BloomA.
    //
    // HALF RESOLUTION because a smear along a line is low-frequency along that
    // line by construction, and the composite blends it back only where there is
    // motion -- so the resolution loss lands exactly where detail has already
    // been destroyed by the blur itself. The taps still read the FULL-RES target,
    // so the half-res part is the output grid, not the source.
    // -------------------------------------------------------------------------
    if (p.x >= half2.x || p.y >= half2.y) return;

    const float2 uv = (float2(p) + 0.5) / float2(half2);

    // The centre colour, as a 2x2 box of the full-res target. Point-sampling the
    // downsample instead aliases thin bright features into flickering dots, the
    // same failure Bloom.slang's extract pass names -- and it would show up here
    // on every pixel where the blur strength is low, i.e. most of the frame.
    float3 boxSum = float3(0.0);
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++)
            boxSum += rayTracingTexture[clamp(p * 2 + int2(dx, dy), int2(0), full - 1)].rgb;
    const float3 centre = boxSum * 0.25;

    // ONE VELOCITY SAMPLE, at this pixel. Per-tap velocities plus a tile-max
    // pre-pass (McGuire's reconstruction filter) is the version that lets a fast
    // object smear PAST its own silhouette; without it, a moving object gathers
    // from where it currently is not, so its trailing edge is softened but the
    // static background beyond the silhouette stays sharp and the smear stops
    // abruptly. That is the known limit of this filter, and the upgrade is a
    // third pass plus one more scratch image, not a rewrite of this one.
    float2 velocity = u_SceneVelocity.SampleLevel(uv, 0.0).rg * g_Mb.intensity;

    // THE CAP. See the header: unbounded velocity is either unbounded cost or,
    // with a fixed tap count, kTapCount visible ghost copies of the frame.
    const float len = length(velocity);
    if (len > g_Mb.maxBlurUv && len > 1e-8)
        velocity *= g_Mb.maxBlurUv / len;

    // Below half a pixel of motion there is nothing to integrate, and gathering
    // anyway is actively harmful: the taps land on neighbouring texels and
    // return a slightly softened copy of a pixel that did not move. Reporting
    // zero strength makes the composite keep the sharp pixel untouched.
    const float blurPixels = length(velocity * float2(full));
    if (blurPixels < 0.5) {
        u_BloomA[p] = float4(centre, 0.0);
        return;
    }

    // CENTRED ON THE PIXEL, spanning half the segment either side.
    //
    // Velocity is (current UV - previous UV), so the surface came FROM
    // uv - velocity, and the blur runs BACKWARD along the vector -- positive s
    // below steps toward the previous position. A one-sided integral over
    // [uv, uv - velocity] is what a shutter opening at the start of the frame
    // actually captures, but it puts the whole smear behind the object and the
    // object then reads as lagging its own position by half a blur length --
    // most visible on a cursor or a tracked object, which appears to trail the
    // input. Centring is the standard trade: the smear straddles the object and
    // it stays visually anchored where it is.
    float3 sum = float3(0.0);
    for (uint i = 0; i < kTapCount; i++) {
        const float s = (float(i) / float(kTapCount - 1)) - 0.5;   // [-0.5, 0.5]
        const float2 tapUv = uv - velocity * s;

        // CLAMPED IN TEXEL SPACE, and it has to be. These are storage-image
        // loads: unfiltered, unaddressed, with no sampler and therefore no
        // address mode -- an out-of-range load is undefined rather than
        // edge-clamped. Worse, the samplers on this engine's sampled bindings
        // default to REPEAT, so anyone reworking this into a sampled read gets
        // the far side of the screen dragged into the blur at every frame edge.
        const int2 q = clamp(int2(tapUv * float2(full)), int2(0), full - 1);
        sum += rayTracingTexture[q].rgb;
    }

    // UNIFORM WEIGHTS, i.e. a box along the segment. A real shutter is a box in
    // time, so this is the honest reconstruction; a Gaussian along the line
    // would look smoother and would be a lie about what a camera does, and it
    // would also weaken exactly the far ends of the streak that sell the effect.
    const float3 blurred = sum / float(kTapCount);

    // STRENGTH RAMPS IN over the first couple of pixels of motion rather than
    // switching on at the half-pixel cutoff above. A hard switch makes the
    // boundary between the blurred and unblurred parts of a slowly accelerating
    // object pop, and on a slow camera pan it makes the whole frame flip between
    // sharp and soft on the frame where the motion crosses the threshold.
    const float strength = saturate((blurPixels - 0.5) / 2.0);

    u_BloomA[p] = float4(blurred, strength);
}
