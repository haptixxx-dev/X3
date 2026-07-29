// =============================================================================
// Taa -- temporal anti-aliasing, and the largest image-quality win in the plan.
//
// Every frame renders with a sub-pixel JITTER on the projection, so successive
// frames sample different points inside each pixel. Reprojecting the previous
// result through the velocity buffer and blending gives, over a few frames, an
// effective supersampling the renderer never paid for.
//
// THE ENTIRE DIFFICULTY IS HISTORY REJECTION, not the blend. A pixel whose
// surface was occluded last frame, or moved out of view, has no valid history --
// and blending it in anyway is what produces ghosting: a smeared trail behind
// every moving object. ENGINE_PLAN.md calls this out and it is accurate.
//
// NEIGHBOURHOOD CLAMPING is the mitigation. The current frame's 3x3
// neighbourhood bounds what this pixel could plausibly be; clamping the
// reprojected history into that box discards a sample that disagrees too much
// with its surroundings, which is exactly the disocclusion case. It is not
// perfect -- a thin moving feature against a similar background still ghosts --
// but it costs nine taps and removes the artefact everyone notices.
//
// THE HISTORY IS PING-PONGED, read one image and write the other. Reading and
// writing one image in a single dispatch is a race: the read is at the
// REPROJECTED texel and the write is at this one, so with any camera motion
// another invocation may overwrite what this one is about to read. It does not
// crash, does not trip validation, and simply makes the output differ between
// runs of the same build.
//
// IT RUNS AFTER THE TONEMAP, on display-referred colour, deliberately. Clamping
// in HDR lets one very bright neighbour widen the box until it stops rejecting
// anything, so the clamp silently stops working exactly where ghosting is most
// visible. The cost is that TAA cannot recover highlight detail the tonemap
// already compressed.
// =============================================================================

import GpuTypes;
import Bindings;

// TWO DISPATCHES, and the second one is not an optimisation to remove.
//
// The resolve reads a 3x3 NEIGHBOURHOOD of the render target and writes that
// target too. That is the same unordered read/write hazard the history had: a
// neighbour invocation may overwrite a texel before this one samples it. So the
// resolve writes only to the history, and a second pass copies the history back
// over the target. Two full-screen dispatches instead of one, in exchange for an
// output that is the same every run.
//
// It showed up as an rmse of 0.0001 between runs of the same build -- small
// enough to look like float noise and large enough to make the golden fail
// intermittently, which is the worst size for a bug to be.
struct TaaPush { uint pass; uint _pad0, _pad1, _pad2; };
[[vk::push_constant]] TaaPush g_Taa;

[shader("compute")]
[numthreads(8, 4, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    const int2 p = int2(threadId.xy);
    uint dimX, dimY;
    rayTracingTexture.GetDimensions(dimX, dimY);
    const int2 dims = int2(dimX, dimY);
    if (p.x >= dims.x || p.y >= dims.y) return;

    if (g_Taa.pass == 1) {
        // COPY BACK. The resolve left its result in the history image; this is
        // what the rest of the frame and the readback see.
        rayTracingTexture[p] = float4(u_TaaHistoryWrite[p].rgb, 1.0);
        return;
    }

    const float3 current = rayTracingTexture[p].rgb;

    // NO HISTORY ON THE FIRST FRAME. u_Settings.taaHistoryValid is 0 then, and
    // blending against an uninitialised image would smear whatever the allocator
    // returned across the first several frames.
    if (u_Settings.taaHistoryValid == 0) {
        u_TaaHistoryWrite[p] = float4(current, 1.0);
        return;
    }

    // The 3x3 neighbourhood, as an AABB in colour space.
    float3 lo = current, hi = current;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const float3 c = rayTracingTexture[clamp(p + int2(dx, dy), int2(0), dims - 1)].rgb;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }

    // REPROJECT. Velocity is (current UV - previous UV), so subtracting it gives
    // where this surface was. The sample is point-filtered rather than bilinear:
    // a bilinear history tap blurs the image a little every frame and, over the
    // dozens of frames a static view accumulates, that compounds into visible
    // softening. Point sampling keeps it sharp at the cost of a little aliasing
    // on sub-pixel motion, which is the better trade at this stage.
    const float2 uv = (float2(p) + 0.5) / float2(dims);
    const float2 velocity = u_SceneVelocity.SampleLevel(uv, 0.0).rg;
    const float2 prevUv = uv - velocity;

    // Off-screen history is no history. Anything reprojecting outside the frame
    // was not rendered last frame at all.
    if (any(prevUv < 0.0) || any(prevUv > 1.0)) {
        u_TaaHistoryWrite[p] = float4(current, 1.0);
        return;
    }

    const int2 prevTexel = clamp(int2(prevUv * float2(dims)), int2(0), dims - 1);
    float3 history = u_TaaHistoryRead[prevTexel].rgb;

    // THE CLAMP. This one line is what separates TAA from a smear.
    history = clamp(history, lo, hi);

    // Blend weight. 0.9 history is roughly a ten-frame exponential window: long
    // enough to converge visibly, short enough that a clamp failure decays
    // rather than persisting. A higher weight looks better on a static view and
    // ghosts far worse the moment anything moves.
    const float3 resolved = lerp(current, history, 0.9);

    u_TaaHistoryWrite[p] = float4(resolved, 1.0);
}
