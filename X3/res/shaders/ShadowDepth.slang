// =============================================================================
// ShadowDepth -- one cascade of the shadow map, from the light's point of view.
//
// Vertex stage ONLY, exactly like DepthPrepass and for the same reason: a
// fragment shader that writes nothing still has to be scheduled and declaring
// one defeats the early-Z rejection a depth-only pass exists for.
//
// THE CASCADE IS A PUSH CONSTANT, not a separate pipeline or a separate buffer
// binding. Every cascade draws the same geometry with a different matrix, so the
// only thing that changes between the four passes is one index -- and the atlas
// sub-rect, which is a dynamic scissor rather than shader state.
//
// It reuses the SAME per-submesh draw list the depth prepass walks. Casters are
// not culled per cascade: the engine has no frustum-cull helper anywhere yet, so
// every cascade draws every opaque submesh. That is the obvious thing to fix
// first if this pass ever costs anything, and it is deliberately not fixed now
// because a cull that silently drops a caster is a shadow that silently
// disappears, and there is no gate for that yet.
// =============================================================================

import GpuTypes;
import Bindings;

struct DrawPushConstants {
    uint entityIndex;
    uint materialSlot;   // unused here; the range is shared with ForwardOpaque
    uint cascadeIndex;
};
[[vk::push_constant]] DrawPushConstants g_Draw;

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID) : SV_Position {
    const EntityHandle e = EntityLookupTable[g_Draw.entityIndex];
    const float4x4 model = TransformBuffer[e.transformIdx];

    // SV_VertexID IS ALREADY THE FETCHED INDEX -- MeshIndexBuffer is bound as the
    // index buffer, so Vulkan hands over the value it read from it. See
    // DepthPrepass.slang for what indexing it a second time costs.
    const float3 positionWS = mul(model, float4(VertexBuffer[vertexId].positionU.xyz, 1.0)).xyz;

    return mul(u_Shadow.viewProj[g_Draw.cascadeIndex], float4(positionWS, 1.0));
}
