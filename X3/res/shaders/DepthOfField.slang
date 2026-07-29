// =============================================================================
// DepthOfField -- three passes over the two half-resolution scratch images.
//
// Downsample+CoC -> CoC-weighted disc gather -> full-res composite. Gather-based
// rather than scatter: a scatter needs point sprites and blending from a compute
// shader, which this engine has no path for, and a gather is the same integral
// read from the other end.
//
// ---- WHAT THIS FILE NEEDS ---------------------------------------------------
//
// Bindings, all of them ALREADY DECLARED in Bindings.slang. Nothing new:
//
//   binding 0, set 0  rayTracingTexture  RWTexture2D<float4>
//                     The render target, full resolution. Read 2x2 by pass 0,
//                     read at its OWN texel and written by pass 2.
//   binding 4, set 0  u_SceneDepth       Sampler2D
//                     Reverse-Z depth. Read by pass 0 (four full-res taps per
//                     half-res texel) and pass 2 (one tap).
//   binding 7, set 0  u_BloomA           RWTexture2D<float4>, HALF resolution
//                     Pass 0 writes it: rgb = half-res colour, a = SIGNED CoC
//                     radius in half-res texels. Pass 1 reads it.
//   binding 8, set 0  u_BloomB           RWTexture2D<float4>, HALF resolution
//                     Pass 1 writes it: rgb = blurred colour, a = near-field
//                     coverage in [0,1]. Pass 2 reads it.
//   binding 0, set 1  u_Camera           ConstantBuffer<CameraUBO>
//                     nearPlane and farPlane only, to linearise reverse-Z.
//
// REUSING THE BLOOM PING-PONG IS AN ASSUMPTION, and it is the one thing to check
// when wiring this up. Bloom runs BEFORE the tonemap and depth of field runs
// AFTER TAA, so by the time this dispatches the two images are dead -- nothing
// reads them again this frame. That is what makes the reuse safe, and it is why
// depth of field and motion blur (which reuses u_BloomA the same way) must not
// interleave: run all three passes of this, then all of that. If the pass order
// in Renderer.cpp ever puts bloom after the tonemap, this silently starts
// compositing a bloom buffer into the blur and vice versa. Two dedicated
// half-res images would remove the coupling; they would also mean two more
// bindings written by every pipeline in the engine, which is exactly the cost
// Bindings.slang already refused to pay for the bloom pyramid.
//
// Push constant, 16 bytes, which is the ENTIRE budget: Renderer.cpp declares
// pushConstantSize = 16 for every compute pipeline. A fifth field does not fit
// without changing that for all of them, so anything else this effect wants has
// to be derived or hardcoded -- which is why the tap count and the maximum
// gather radius below are compile-time constants rather than parameters.
//
//   uint  pass            0 downsample+CoC (half res), 1 gather (half res),
//                         2 composite (full res)
//   float focusDistance   view-space distance, world units, where CoC is zero
//   float aperture        unitless strength; CoC tends to +/- this as a
//                         fraction of maxCocRadius, so aperture 1 means the far
//                         background reaches the full radius and 0 disables
//   float maxCocRadius    largest blur radius, in HALF-RES texels. Clamped in
//                         here to kMaxGatherRadius -- see why below.
//
// ---- REVERSE-Z --------------------------------------------------------------
// Near maps to 1, far to 0, the buffer clears to 0. Linear view depth is
// nearPlane / depthSample, and a sample of EXACTLY 0 is the far plane or sky
// that nothing drew over -- dividing by it gives an infinity that turns into a
// NaN the moment it meets a subtraction, and a NaN written into a half-res texel
// spreads across the whole gather radius as black or as garbage. Every read here
// tests for it first.
//
// ---- KNOWN COMPROMISE -------------------------------------------------------
// This runs on DISPLAY-REFERRED colour, because ENGINE_PLAN.md puts depth of
// field after TAA and TAA runs after the tonemap. Optically the blur belongs in
// linear light: a small bright highlight spread over a large disc should stay
// bright, and averaging tonemapped values instead gives a dull grey disc where a
// real lens gives a bright bokeh circle. Moving this before the tonemap fixes it
// and costs a second TAA-ordering argument, so it is recorded rather than done.
// =============================================================================

import GpuTypes;
import Bindings;

struct DofPush {
    uint  pass;
    float focusDistance;
    float aperture;
    float maxCocRadius;
};
[[vk::push_constant]] DofPush g_Dof;

// THE GATHER RADIUS IS BOUNDED AT COMPILE TIME, and the bound is not cosmetic.
// The tap pattern below has a fixed count, so the taps spread over whatever
// radius they are given: at radius 8 they are about six half-res texels apart,
// at radius 40 they are about 160 apart. Past the point where the spacing
// exceeds a couple of texels the disc stops being a disc and becomes 32 visible
// copies of every bright highlight -- the artefact reads as a scattering of
// duplicated dots, not as blur, and it looks like a bug rather than an effect.
// So maxCocRadius is clamped here instead of being trusted.
static const float kMaxGatherRadius = 8.0;   // half-res texels == 16 full-res px

// A VOGEL (golden-angle) DISC rather than concentric rings. Rings put every tap
// on one of N radii, so a bright point source resolves into N concentric copies
// of itself; the golden-angle spiral has uniform density with no repeated radius
// and no repeated angle, so the same undersampling degrades into softness.
//
// PRECOMPUTED, for the reason Bloom.slang precomputes its Gaussian weights: the
// alternative is 32 sin/cos pairs per pixel for numbers the compiler cannot
// fold. Unit disc; scaled by the gather radius at use.
static const uint kTapCount = 32;
static const float2 kDisc[kTapCount] = {
    float2( 0.125000,  0.000000), float2(-0.159645,  0.146248),
    float2( 0.024436, -0.278438), float2( 0.201222,  0.262459),
    float2(-0.369268, -0.065318), float2( 0.349802, -0.222516),
    float2(-0.117002,  0.435242), float2(-0.223136, -0.429634),
    float2( 0.484115,  0.176798), float2(-0.503641,  0.207896),
    float2( 0.242788, -0.518824), float2( 0.179414,  0.572001),
    float2(-0.540757, -0.313380), float2( 0.634370, -0.139464),
    float2(-0.387146,  0.550675), float2(-0.089440, -0.690200),
    float2( 0.549072,  0.462758), float2(-0.738878,  0.030555),
    float2( 0.538955, -0.536332), float2(-0.036058,  0.779792),
    float2(-0.512818, -0.614527), float2( 0.812360,  0.109302),
    float2(-0.688311,  0.478909), float2( 0.188086, -0.836061),
    float2( 0.435033,  0.759191), float2(-0.850448, -0.271316),
    float2( 0.826102, -0.381680), float2(-0.357888,  0.855156),
    float2(-0.319407, -0.888034), float2( 0.849909,  0.446688),
    float2(-0.944035,  0.248845), float2( 0.536596, -0.834530),
};

/// Linear view distance from a reverse-Z depth sample.
///
/// A sample of exactly 0 is the far plane -- either nothing drew there or the
/// sky did -- and is returned as farPlane rather than divided by. Letting it
/// through as a division produces +inf, `inf - focusDistance` is still inf, and
/// the clamp below would keep it, so the NaN only appears later when it is
/// multiplied by a zero weight. That failure surfaces as a black hole in the
/// blurred image several passes from its cause.
float LinearViewDepth(float rawDepth) {
    if (rawDepth <= 0.0) return u_Camera.farPlane;
    return min(u_Camera.nearPlane / rawDepth, u_Camera.farPlane);
}

/// SIGNED circle of confusion, in HALF-RES texels.
///
/// Positive means the surface is BEHIND the focus plane (background), negative
/// means in FRONT of it (near field). The sign is the whole reason this is
/// stored rather than the magnitude: the gather needs to know which side of the
/// focus plane a tap is on to decide whether it is allowed to bleed.
///
/// aperture * (z - focus) / z is the thin-lens form reduced to what is
/// measurable here. It is zero at the focus plane, tends to +aperture as z goes
/// to infinity -- so the background blur saturates instead of growing without
/// bound -- and goes strongly negative as z approaches the camera, which is why
/// the near field blurs faster than the far field, exactly as a real lens does.
float SignedCoc(float linearZ, float radius) {
    const float z    = max(linearZ, u_Camera.nearPlane);
    const float norm = g_Dof.aperture * (z - g_Dof.focusDistance) / z;
    return clamp(norm, -1.0, 1.0) * radius;
}

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 p = int2(threadId.xy);

    uint fullX, fullY;
    rayTracingTexture.GetDimensions(fullX, fullY);
    const int2 full  = int2(fullX, fullY);
    const int2 half2 = max(full / 2, int2(1));

    const float radius = clamp(g_Dof.maxCocRadius, 0.0, kMaxGatherRadius);

    // -------------------------------------------------------------------------
    // PASS 2 -- COMPOSITE, at full resolution.
    // -------------------------------------------------------------------------
    if (g_Dof.pass == 2) {
        if (p.x >= full.x || p.y >= full.y) return;

        const float2 uv    = (float2(p) + 0.5) / float2(full);
        const float  rawZ  = u_SceneDepth.SampleLevel(uv, 0.0).r;
        const float  cocC  = SignedCoc(LinearViewDepth(rawZ), radius);

        // Bilinear by hand, exactly as Bloom.slang does it and for the same
        // reason: u_BloomB is a storage image and storage images cannot be
        // sampled. Point-sampling the half-res layer instead makes every blurred
        // region visibly blocky, which is the one artefact that gives away that
        // the blur was not computed at full resolution.
        const float2 h = (float2(p) + 0.5) * 0.5 - 0.5;
        const int2   b = int2(floor(h));
        const float2 f = h - float2(b);
        const float4 c00 = u_BloomB[clamp(b + int2(0,0), int2(0), half2 - 1)];
        const float4 c10 = u_BloomB[clamp(b + int2(1,0), int2(0), half2 - 1)];
        const float4 c01 = u_BloomB[clamp(b + int2(0,1), int2(0), half2 - 1)];
        const float4 c11 = u_BloomB[clamp(b + int2(1,1), int2(0), half2 - 1)];
        const float4 blurred = lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);

        // TWO REASONS TO USE THE BLURRED LAYER, and they are not the same test.
        //
        // The first is this pixel's own CoC: a pixel that is itself out of focus
        // wants the blur. Ramped rather than switched, because a hard cut at
        // some CoC threshold puts a visible contour line through any smooth
        // depth gradient -- a wall receding from the camera would show a seam
        // where the blur switched on.
        //
        // The second is NEAR-FIELD COVERAGE, which pass 1 measured. A sharp
        // in-focus background pixel that a blurry foreground object spills over
        // has a CoC of zero and must STILL take the blurred layer, otherwise the
        // foreground blur stops dead at the object's silhouette -- the giveaway
        // artefact of naive depth of field, where an out-of-focus foreground has
        // a razor-sharp outline.
        const float fromCoc = saturate((abs(cocC) - 0.5) / 1.5);
        const float blend   = max(fromCoc, saturate(blurred.a));

        const float3 sharp = rayTracingTexture[p].rgb;
        rayTracingTexture[p] = float4(lerp(sharp, blurred.rgb, blend), 1.0);
        return;
    }

    if (p.x >= half2.x || p.y >= half2.y) return;

    // -------------------------------------------------------------------------
    // PASS 0 -- DOWNSAMPLE AND CIRCLE OF CONFUSION.
    // -------------------------------------------------------------------------
    if (g_Dof.pass == 0) {
        // Colour is a 2x2 box, not a point sample. Point sampling a half-res
        // downsample aliases every thin bright feature into dots that flicker
        // the moment the camera moves -- the same failure Bloom.slang's extract
        // pass names.
        float3 sum = float3(0.0);
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
                sum += rayTracingTexture[clamp(p * 2 + int2(dx, dy), int2(0), full - 1)].rgb;
        const float3 colour = sum * 0.25;

        // DEPTH IS NOT AVERAGED. Across a silhouette the mean of a near and a
        // far depth is a distance at which no surface exists, so the CoC derived
        // from it is a middling blur along every object outline -- a soft halo
        // tracing every edge in the frame, which is far more visible than either
        // neighbouring blur level.
        //
        // The NEAREST of the four is taken instead (under reverse-Z the nearest
        // is the LARGEST raw sample). The near field is the side that has to
        // expand outward over its surroundings, and it can only do that if the
        // half-res texels straddling a foreground silhouette carry the
        // foreground's CoC. The cost is a half-res-texel-wide rim on the far
        // side of a silhouette that is slightly under-blurred; that is a
        // one-texel error inside already-blurry content, against an outline
        // around every object in the frame for the other choice.
        //
        // Sampled at EXACT full-res texel centres so the LINEAR sampler on this
        // binding returns one texel unblended: an interpolated depth is, again,
        // a depth no surface has.
        float rawNearest = 0.0;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                const int2 q = clamp(p * 2 + int2(dx, dy), int2(0), full - 1);
                const float2 uv = (float2(q) + 0.5) / float2(full);
                rawNearest = max(rawNearest, u_SceneDepth.SampleLevel(uv, 0.0).r);
            }
        }

        // The CoC goes in ALPHA. rgba16f holds +/-8 with room to spare, the
        // gather needs colour and CoC from the same tap, and packing them
        // together means one load per tap instead of two.
        const float coc = SignedCoc(LinearViewDepth(rawNearest), radius);
        u_BloomA[p] = float4(colour, coc);
        return;
    }

    // -------------------------------------------------------------------------
    // PASS 1 -- THE GATHER, at half resolution, u_BloomA -> u_BloomB.
    //
    // A SEPARATE IMAGE FOR THE OUTPUT, not a blur in place. This reads a
    // neighbourhood up to kMaxGatherRadius across and writes one texel; reading
    // a neighbourhood of an image the same dispatch writes is the unordered
    // hazard Taa.slang was split to avoid -- another invocation may overwrite a
    // texel before this one loads it. It does not crash, does not trip
    // validation, and makes the output differ between runs of the same build,
    // which showed up in TAA as an rmse that failed the golden intermittently.
    // -------------------------------------------------------------------------
    const float4 centre = u_BloomA[p];
    const float  cocC   = centre.a;

    // THE GATHER RADIUS IS THE MAXIMUM, NOT THIS PIXEL'S OWN CoC. A perfectly
    // sharp pixel must still look kMaxGatherRadius outward, because a blurry
    // foreground object several texels away is supposed to spill onto it. Sizing
    // the search by the centre's CoC would make an in-focus pixel search radius
    // zero, and the near field would then be clipped exactly at the silhouette
    // of whatever is out of focus.
    //
    // It also makes the cost constant: the same 32 taps for every pixel in every
    // frame, so the pass has no content-dependent timing spike.
    //
    // THE CENTRE IS ADDED EXPLICITLY, not left to the pattern. kDisc[0] sits a
    // texel out, so a fully in-focus neighbourhood -- where every tap's reach is
    // zero and every weight is therefore zero -- would normalise by zero. With
    // the centre in at weight 1, an in-focus pixel provably returns itself.
    float  wC      = saturate(abs(cocC) + 1.0) / max(cocC * cocC, 1.0);
    float3 sumC    = centre.rgb * wC;
    float  sumW    = wC;
    float  sumNear = (cocC < 0.0) ? wC : 0.0;

    for (uint i = 0; i < kTapCount; i++) {
        const float2 off = kDisc[i] * radius;
        const int2   q   = clamp(p + int2(round(off)), int2(0), half2 - 1);
        const float4 s   = u_BloomA[q];
        const float  d   = length(off);          // half-res texels from centre
        const float  cocT = s.a;

        // EACH TAP IS WEIGHTED BY ITS OWN CoC, and this is the line that stops
        // the background bleeding onto a sharp foreground edge.
        //
        // Read as a scatter turned inside out: a source pixel spreads its light
        // over a disc of ITS OWN radius, so it can only reach this pixel if that
        // radius is at least the distance between them. `reach` is that radius.
        //
        // Then the asymmetry. If the tap is BEHIND the centre (cocT > cocC, it
        // is farther from the camera), its reach is clamped to the CENTRE's own
        // CoC. Physically a background point's blur disc is occluded by whatever
        // is in front of it, and this shader has no occlusion information -- so
        // without the clamp a very blurry distant surface sprays its colour over
        // a sharp foreground object and every in-focus silhouette in the frame
        // gets a coloured halo of whatever is behind it. With it, a perfectly
        // sharp centre (cocC == 0) accepts nothing from behind at all, and a
        // half-blurred centre accepts proportionally.
        //
        // A tap in FRONT of the centre is deliberately NOT clamped: that is the
        // near field, and it is supposed to spill over what is behind it.
        float reach = abs(cocT);
        if (cocT > cocC) reach = min(reach, abs(cocC));

        // Falls off over one texel at the disc edge rather than cutting: a hard
        // cut makes the boundary of every bokeh circle alias, and with a fixed
        // tap pattern that aliasing is a fixed pattern, so it reads as a
        // repeating texture rather than as noise.
        float w = saturate(reach - d + 1.0);

        // ENERGY CONSERVATION. A pixel scattering over radius r spreads its
        // light across about pi*r^2, so each receiver gets a 1/r^2 share. The
        // number of taps that can reach a given pixel also grows as r^2, so the
        // two cancel for a uniform region -- but not for a small bright blurry
        // spot, which without this term stays as bright spread out as it was
        // concentrated and reads as a glowing blob.
        w /= max(reach * reach, 1.0);

        sumC += s.rgb * w;
        sumW += w;

        // NEAR-FIELD COVERAGE, for the composite. Fraction of the gathered
        // weight that came from in front of the FOCUS PLANE (cocT < 0), not from
        // in front of the centre. Inside a blurred foreground object every tap
        // qualifies and coverage is 1; on the sharp background just outside its
        // silhouette only the taps that reached across qualify, so coverage
        // falls off smoothly and the composite fades the blur out over exactly
        // the distance the foreground's CoC can reach.
        if (cocT < 0.0) sumNear += w;
    }

    // Unreachable given the explicit centre weight above, and kept anyway: the
    // alternative to a branch here is a division by zero the first time someone
    // changes the weighting, and it writes a NaN into a texel the composite then
    // spreads over four full-res pixels.
    if (sumW <= 1e-5) {
        u_BloomB[p] = float4(centre.rgb, 0.0);
        return;
    }

    u_BloomB[p] = float4(sumC / sumW, saturate(sumNear / sumW));
}
