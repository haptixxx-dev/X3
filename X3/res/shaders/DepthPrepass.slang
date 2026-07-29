// =============================================================================
// DepthPrepass -- the first geometry this engine has ever rasterized.
//
// Vertex stage ONLY. A depth prepass with a fragment shader is slower than one
// without: a fragment stage that writes nothing still has to be scheduled, and
// declaring one can defeat the early-Z rejection the prepass exists to enable.
//
// WHY A PREPASS AT ALL. Forward+ shades in the forward pass, so any pixel drawn
// more than once is shaded more than once. Laying depth down first means the
// expensive pass runs with EQUAL depth testing and shades each pixel exactly
// once. It is also what mitigates Forward+'s main weakness -- overdraw -- which
// ENGINE_PLAN.md lists as an accepted cost of the technique.
//
// VERTICES ARE PULLED, not bound. There is no vertex input state and no vertex
// buffer: SV_VertexID indexes the same Gpu::Vertex StructuredBuffer the path
// tracer reads at every hit. That is what makes "validate the rasterizer against
// the reference" a comparison of two renderings of the same bytes rather than of
// two meshes that are probably the same.
// =============================================================================

import GpuTypes;
import Bindings;

// The entity being drawn. One draw call per entity, so this is the only thing
// that changes between them -- everything else is in the bound buffers.
struct DrawPushConstants {
    uint entityIndex;
};
[[vk::push_constant]] DrawPushConstants g_Draw;

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID) : SV_Position {
    const EntityHandle e = EntityLookupTable[g_Draw.entityIndex];
    const float4x4 model = TransformBuffer[e.transformIdx];

    // MeshIndexBuffer holds GLOBAL vertex indices already -- Gpu::TriRef's
    // contract -- so there is nothing to rebase here.
    const uint vi = MeshIndexBuffer[vertexId];
    const float3 positionWS = mul(model, float4(VertexBuffer[vi].positionU.xyz, 1.0)).xyz;

    return mul(u_Camera.viewProj, float4(positionWS, 1.0));
}
