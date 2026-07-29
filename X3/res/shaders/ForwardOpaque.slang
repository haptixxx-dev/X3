// =============================================================================
// ForwardOpaque -- the clustered forward shading pass. The first rasterized
// PICTURE this engine has produced; the depth prepass before it drew only depth.
//
// It shades with the SAME Bsdf.slang lobes, the SAME SampleLightDirect and the
// SAME AmbientIBL as the reference path tracer. The only thing it does
// differently is where the light list comes from: the cluster the fragment falls
// in, rather than every light in the scene. That is what makes "the raster path
// agrees with the reference" a meaningful statement instead of a coincidence --
// if the two disagree, the light culling is the only thing it can be.
//
// DEPTH TEST IS EQUAL AND DEPTH WRITE IS OFF. The prepass already laid the
// nearest depth down, so testing EQUAL shades each pixel exactly once no matter
// how much overdraw the scene has. That is the whole reason the prepass exists
// and the mitigation for Forward+'s one structural weakness.
//
// MATERIALS COME FROM A PUSH CONSTANT, because THE DRAW IS PER SUBMESH.
// Materials are per-submesh, so the fragment shader has to know which one it is
// shading. Reading TriRefBuffer[root + SV_PrimitiveID] is the obvious way and it
// costs the SPIR-V `Geometry` capability -- Vulkan gates gl_PrimitiveID in a
// fragment shader behind the geometryShader FEATURE, which MoltenVK does not
// have. Splitting the draw per submesh makes it a push constant instead.
//
// THE GEOMETRIC NORMAL COMES FROM SCREEN-SPACE DERIVATIVES for the same reason:
// recomputing it from the triangle's three vertices would need the primitive ID
// back. cross(ddx(P), ddy(P)) is exact for a planar triangle, which every
// triangle is.
// =============================================================================

import GpuTypes;
import Bindings;
import Trace;
import Bsdf;

struct DrawPushConstants {
    uint entityIndex;
    uint materialSlot;   // mesh-local; materialBase is added below
};
[[vk::push_constant]] DrawPushConstants g_Draw;

struct VSOut {
    float4 clip       : SV_Position;
    float3 positionWS : POSITION_WS;
    float3 normalWS   : NORMAL_WS;
    float3 tangentWS  : TANGENT_WS;
    float2 uv         : TEXCOORD0;
    // FLAT, not interpolated. tangent.w is the handedness sign and the
    // invalid-tangent sentinel -- interpolating a value that is only ever
    // -1, 0 or +1 would produce fractions that mean nothing.
    nointerpolation float handedness : HANDEDNESS;
};

[shader("vertex")]
VSOut vertexMain(uint vertexId : SV_VertexID) {
    const EntityHandle e = EntityLookupTable[g_Draw.entityIndex];
    const float4x4 model = TransformBuffer[e.transformIdx];
    const float3x3 normalMat = (float3x3)transpose(Inverse(model));

    // SV_VertexID IS ALREADY THE FETCHED INDEX -- MeshIndexBuffer is bound as
    // the index buffer, so Vulkan hands over the value it read from it. See
    // DepthPrepass.slang for what indexing it a second time costs.
    const Vertex v = VertexBuffer[vertexId];

    VSOut o;
    o.positionWS = mul(model, float4(v.positionU.xyz, 1.0)).xyz;
    o.clip       = mul(u_Camera.viewProj, float4(o.positionWS, 1.0));
    o.normalWS   = mul(normalMat, v.normalV.xyz);
    // TANGENTS TRANSFORM BY THE MODEL MATRIX, not the inverse transpose. Using
    // one for both only shows up under non-uniform scale, as a skewed TBN.
    o.tangentWS  = mul((float3x3)model, v.tangent.xyz);
    o.uv         = float2(v.positionU.w, v.normalV.w);   // UVs live in the .w lanes
    o.handedness = v.tangent.w;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VSOut i, bool frontFacing : SV_IsFrontFace) : SV_Target
{
    const EntityHandle e = EntityLookupTable[g_Draw.entityIndex];
    const uint materialIdx =
        e.materialBase + min(g_Draw.materialSlot, max(e.materialSlotCount, 1u) - 1u);

    // THE GEOMETRIC NORMAL IS NOT THE INTERPOLATED ONE. The shadow-ray bias and
    // the hemisphere clamp below are both defined against the geometric normal,
    // and on a smooth-shaded mesh the two differ by up to the crease angle --
    // which is exactly where self-intersection artefacts come from.
    //
    // Derivatives give it exactly: a triangle is planar, so cross(ddx(P), ddy(P))
    // is its plane normal. The sign of that cross product depends on the
    // rasterizer's screen-space winding, so it is ALIGNED TO THE AUTHORED NORMAL
    // rather than trusted -- and then flipped for a back-facing fragment, which
    // is what ResolveHit does with ray.frontFacing.
    float3 Ng = normalize(cross(ddx(i.positionWS), ddy(i.positionWS)));
    if (dot(Ng, i.normalWS) < 0.0) Ng = -Ng;
    if (!frontFacing) Ng = -Ng;

    float3 Ns = normalize(i.normalWS);
    if (!frontFacing) Ns = -Ns;

    const Material mat = SampleMaterial(MaterialBuffer[materialIdx], i.uv);
    const MaterialExt ext = GetMaterialExt(mat);

    // --- tangent-space normal mapping, mirroring ResolveHit -------------------
    if (i.handedness != 0.0 && mat.textures.y != X3_INVALID_TEXTURE) {
        float3 T = normalize(i.tangentWS);
        T = normalize(T - Ns * dot(Ns, T));              // Gram-Schmidt against Ns
        const float3 B = cross(Ns, T) * i.handedness;
        float3 nts = u_MaterialTextures[NonUniformResourceIndex(mat.textures.y)]
                        .SampleLevel(i.uv, 0.0).xyz * 2.0 - 1.0;
        nts.xy *= mat.pbrParams.w;                       // normalScale
        Ns = normalize(mul(transpose(float3x3(T, B, Ns)), nts));
    }

    // Never let the shading normal cross the geometric hemisphere. A normal map
    // on a grazing surface can otherwise point it back into the geometry, which
    // produces black pixels that read as a lighting bug.
    if (dot(Ns, Ng) < 0.0)
        Ns = normalize(Ns - Ng * (dot(Ns, Ng) * 1.0001));

    // THE CAMERA POSITION, taken the same way MakeCameraRay takes it.
    //
    // NOT `u_Camera.transform[3].xyz`. Slang indexes a matrix by ROW, following
    // HLSL, while glm's `transform[3]` is the translation COLUMN -- so that
    // expression yields row 3 of an affine matrix, which is (0,0,0). The camera
    // then sits at the world origin, V points from the surface to the origin,
    // and the shading frame rejects most fragments with wo.z <= 0: no direct
    // lighting at all, and an ambient term reflecting the sky off everything.
    // The picture looks like a rendering, just of nothing in particular.
    const float4 originH = mul(u_Camera.transform, float4(0.0, 0.0, 0.0, 1.0));
    const float3 V = normalize(originH.xyz / originH.w - i.positionWS);
    const ShadingFrame frame = ShadingFrame.make(Ns, i.tangentWS);

    // THE CLUSTER, derived from VIEW SPACE rather than from SV_Position. Both
    // this and the culling pass call ClusterIndexFromView on a view-space point,
    // so they agree by construction -- deriving one from screen coordinates and
    // the other from a matrix is how a half-tile offset creeps in.
    const float3 posView = mul(u_Camera.view, float4(i.positionWS, 1.0)).xyz;
    const uint cluster = ClusterIndexFromView(posView, u_Camera.proj,
                                              u_Camera.nearPlane, u_Camera.farPlane);

    float3 color;
    // THE TIER SPLIT, exactly as the path tracer does it: both branches are the
    // same generic function and Slang specialises each at link time, so the
    // common path carries no clearcoat or sheen code at all.
    if (MaterialHasExtendedLobes(mat)) {
        color = DirectLightingClustered(MakeCoatedBsdf(mat, ext), frame, V,
                                        i.positionWS, Ng, cluster);
    } else {
        color = DirectLightingClustered(MakeBaseBsdf(mat, ext), frame, V,
                                        i.positionWS, Ng, cluster);
    }
    color += AmbientIBL(mat, ext, Ns, V);
    color += mat.emission.xyz * mat.emission.w;

    color = color / (color + float3(1.0));      // Reinhard
    color = pow(color, float3(1.0 / 2.2));      // gamma
    return float4(color, 1.0);
}
