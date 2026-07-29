// =============================================================================
// Velocity -- screen-space motion vectors, for TAA and motion blur.
//
// ENGINE_PLAN.md asks for this in Phase 7 rather than Phase 11 because
// retrofitting it later is painful: every pass that produces geometry has to
// learn where that geometry WAS, and adding that after the fact means revisiting
// all of them.
//
// The output is (current UV - previous UV), so a fragment that did not move
// writes exactly zero and TAA's reprojection is `uv - velocity`. UV space rather
// than NDC or pixels because that is what every consumer wants and it is
// resolution-independent.
//
// TWO CLIP-SPACE POSITIONS PER VERTEX: this frame's viewProj against the model,
// and last frame's prevViewProj against PrevTransformBuffer. Both are needed --
// a camera-only reprojection (the cheap version, derived from depth) cannot see
// an object that moved while the camera stood still.
//
// Depth test EQUAL and no depth write, exactly as the forward pass: the prepass
// already decided which surface is visible, and velocity must describe THAT one.
// =============================================================================

import GpuTypes;
import Bindings;

struct DrawPushConstants {
    uint entityIndex;
    uint materialSlot;   // unused here; the range is shared with ForwardOpaque
    uint cascadeIndex;   // unused here; shared with ShadowDepth
};
[[vk::push_constant]] DrawPushConstants g_Draw;

struct VSOut {
    float4 clip     : SV_Position;
    float4 currClip : CURR_CLIP;
    float4 prevClip : PREV_CLIP;
};

[shader("vertex")]
VSOut vertexMain(uint vertexId : SV_VertexID) {
    const EntityHandle e = EntityLookupTable[g_Draw.entityIndex];
    const float3 posOS = VertexBuffer[vertexId].positionU.xyz;

    const float3 currWS = mul(TransformBuffer[e.transformIdx], float4(posOS, 1.0)).xyz;
    const float3 prevWS = mul(PrevTransformBuffer[e.transformIdx], float4(posOS, 1.0)).xyz;

    VSOut o;
    o.currClip = mul(u_Camera.viewProj, float4(currWS, 1.0));
    o.prevClip = mul(u_Camera.prevViewProj, float4(prevWS, 1.0));
    // SV_Position must be THIS frame's, or the fragment lands where the geometry
    // used to be and fails the EQUAL depth test against the prepass.
    o.clip = o.currClip;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOut i) : SV_Target {
    // THE DIVIDE HAPPENS HERE, not in the vertex shader. Interpolating an
    // already-divided NDC position interpolates linearly in screen space, which
    // is not what a perspective projection does -- the error is largest exactly
    // where a surface is most oblique to the camera.
    const float2 currNdc = i.currClip.xy / i.currClip.w;
    const float2 prevNdc = i.prevClip.xy / i.prevClip.w;

    // NDC [-1,1] to UV [0,1] is a halving; the +0.5 offsets cancel in the
    // difference, so only the scale survives.
    return float4((currNdc - prevNdc) * 0.5, 0.0, 0.0);
}
