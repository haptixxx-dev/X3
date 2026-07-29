// =============================================================================
// Trace -- ray/scene intersection and hit-attribute resolution.
//
// THIS MODULE IS THE POINT OF THE SLANG MIGRATION. Before it, ~250 lines of BVH
// traversal, ray-AABB slab tests and Moller-Trumbore were copy-pasted into
// PathTracing.comp, PBR.comp and Phong.comp, so every fix had to be made three
// times and one of them was always missed. It is now compiled once.
//
// Phase 10 reuses this directly: DDGI probe updates and BVH-traced shadow rays
// need exactly a ray/scene intersection oracle, which is what CheckRayCollision
// is. Keeping it in one module is what makes that reuse a call rather than a
// fourth copy.
// =============================================================================

module Trace;

import GpuTypes;
import Bindings;
import Bsdf;
__exported import GpuTypes;
__exported import Bsdf;

public static const float PI          = 3.1415926;
public static const float INV_TWOPI   = 0.15915494309189535;  // 1/(2pi)
public static const float INV_PI      = 0.3183098861837907;   // 1/pi
public static const float INF_T       = 1e30f;
public static const float SURFACE_BIAS = 1e-4f;

// Per-invocation traversal counters, read back by the debug heatmap modes.
// `static` at module scope is thread-local (SPIR-V Private storage), which is
// the same semantics the GLSL globals had.
public static uint g_AabbIntersectionCount = 0;
public static uint g_TriIntersectionCount = 0;

public struct Ray {
    public float3 origin;
    public float  t;
    public float3 dir;

    // Recorded during traversal, resolved ONCE per ray afterwards.
    public float2 bary;         // (u,v) = barycentric weights of v1 and v2
    public uint   triIdxLocal;  // mesh-local triangle index of the winning hit
    public uint   entityIdx;    // index into EntityLookupTable of the winner

    // Resolved by ResolveHit().
    public float3 normalGeom;   // geometric, world space, from cross(E1,E2)
    public float3 normalShade;  // interpolated + normal-mapped, world space
    /// World-space tangent, needed because ANISOTROPY IS DEFINED RELATIVE TO IT.
    /// An arbitrary basis rotates freely about the normal, which an isotropic
    /// material cannot detect but an anisotropic one shows as brushed metal
    /// whose grain swims as the camera moves. Zero when the mesh had no usable
    /// tangent; ShadingFrame::make falls back to a stable arbitrary basis.
    public float3 tangent;
    public float2 uv;
    public uint   materialIdx;
    public bool   frontFacing;

    // Occlusion queries skip ResolveHit entirely -- see IsInShadow.
    public bool   anyHit;
};

// Slang's stdlib has no matrix inverse (HLSL never had one), so the GLSL
// `inverse()` this code used needs an explicit implementation. Cofactor
// expansion, which is what glslang emitted for the same call.
public float4x4 Inverse(float4x4 m) {
    float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
    float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
    float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
    float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];

    float b00 = a00 * a11 - a01 * a10;
    float b01 = a00 * a12 - a02 * a10;
    float b02 = a00 * a13 - a03 * a10;
    float b03 = a01 * a12 - a02 * a11;
    float b04 = a01 * a13 - a03 * a11;
    float b05 = a02 * a13 - a03 * a12;
    float b06 = a20 * a31 - a21 * a30;
    float b07 = a20 * a32 - a22 * a30;
    float b08 = a20 * a33 - a23 * a30;
    float b09 = a21 * a32 - a22 * a31;
    float b10 = a21 * a33 - a23 * a31;
    float b11 = a22 * a33 - a23 * a32;

    float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    float invDet = 1.0 / det;

    float4x4 r;
    r[0][0] = ( a11 * b11 - a12 * b10 + a13 * b09) * invDet;
    r[0][1] = (-a01 * b11 + a02 * b10 - a03 * b09) * invDet;
    r[0][2] = ( a31 * b05 - a32 * b04 + a33 * b03) * invDet;
    r[0][3] = (-a21 * b05 + a22 * b04 - a23 * b03) * invDet;
    r[1][0] = (-a10 * b11 + a12 * b08 - a13 * b07) * invDet;
    r[1][1] = ( a00 * b11 - a02 * b08 + a03 * b07) * invDet;
    r[1][2] = (-a30 * b05 + a32 * b02 - a33 * b01) * invDet;
    r[1][3] = ( a20 * b05 - a22 * b02 + a23 * b01) * invDet;
    r[2][0] = ( a10 * b10 - a11 * b08 + a13 * b06) * invDet;
    r[2][1] = (-a00 * b10 + a01 * b08 - a03 * b06) * invDet;
    r[2][2] = ( a30 * b04 - a31 * b02 + a33 * b00) * invDet;
    r[2][3] = (-a20 * b04 + a21 * b02 - a23 * b00) * invDet;
    r[3][0] = (-a10 * b09 + a11 * b07 - a12 * b06) * invDet;
    r[3][1] = ( a00 * b09 - a01 * b07 + a02 * b06) * invDet;
    r[3][2] = (-a30 * b03 + a31 * b01 - a32 * b00) * invDet;
    r[3][3] = ( a20 * b03 - a21 * b01 + a22 * b00) * invDet;
    return r;
}

public float3 GetSkyboxLight(float3 dir) {
    float u = 0.5f + atan2(dir.z, dir.x) * INV_TWOPI;
    // MINUS, not plus. The importer no longer flips images vertically on load
    // (assimp/glTF UVs are top-left origin, which is also Vulkan's convention),
    // and this line was compensating for that flip.
    float v = 0.5f - asin(dir.y) * INV_PI;
    return skyboxTexture.SampleLevel(float2(u, v), 0.0).rgb;
}

// Slab method ray-AABB intersection.
// https://tavianator.com/2022/ray_box_boundary.html
float IntersectAABB(float3 origin, float3 invDir, float3 bmin, float3 bmax, float rayTMax) {
    float3 t1 = (bmin - origin) * invDir;
    float3 t2 = (bmax - origin) * invDir;
    float3 tMin = min(t1, t2);
    float3 tMax = max(t1, t2);
    float tNear = max(max(tMin.x, tMin.y), tMin.z);
    float tFar  = min(min(tMax.x, tMax.y), tMax.z);
    return (tFar >= tNear && tNear < rayTMax && tFar > 0.0) ? tNear : INF_T;
}

// Moller-Trumbore ray-triangle intersection.
/// TWO-SIDED INTERSECTION, off by default.
///
/// The intersector below CULLS BACK FACES, and that is correct for camera and
/// shadow rays -- it halves the work and every closed mesh looks identical. It
/// is WRONG for a ray that starts inside geometry, because such a ray sees only
/// back faces and therefore passes straight through the solid it is buried in
/// and out to the sky.
///
/// That is not hypothetical. It is what made DDGI leak: a probe inside a wall
/// fired 64 rays, every one escaped the wall unimpeded, and the probe filled
/// with skybox radiance. Interior points then interpolated those probes and a
/// sealed box came out at 128/255 against a path-traced ground truth of 0.
///
/// It also made `Ray::frontFacing` DEAD. It is computed in CheckRayCollision
/// from the hit normal, but with back faces culled the test can only ever be
/// true, so every consumer's back-face branch was unreachable code.
///
/// A module-static rather than a Ray field, matching g_UseShadowMaps: adding a
/// field would mean every construction site has to initialise it, and Slang's
/// uninitialised-variable analysis has already caught one real bug in this
/// codebase that a missed initialiser would reintroduce.
public static bool g_TwoSidedTrace = false;

bool IntersectTri(inout Ray r, TrianglePositions tri, uint meshLocalTriIdx) {
    const float3 E1 = tri.v1.xyz - tri.v0.xyz;
    const float3 E2 = tri.v2.xyz - tri.v0.xyz;
    float3 Ng = cross(E1, E2);
    float det = -dot(r.dir, Ng);
    // The epsilon still rejects rays parallel to the plane, in both modes: a
    // near-zero determinant makes the division below explode rather than merely
    // being a grazing hit.
    if (g_TwoSidedTrace ? (abs(det) < 1e-6) : (det < 1e-6)) return false;

    float invdet = 1.0 / det;
    float3 AO  = r.origin - tri.v0.xyz;
    float3 DAO = cross(AO, r.dir);
    float t = dot(AO, Ng) * invdet;
    float u = dot(E2, DAO) * invdet;
    float v = -dot(E1, DAO) * invdet;
    if (t < 0.0 || u < 0.0 || v < 0.0 || (u + v) > 1.0) return false;
    if (t >= r.t) return false;

    r.t = t;
    // BARYCENTRIC CONVENTION, derived from this exact code rather than guessed:
    // with E1 = v1-v0 and E2 = v2-v0, `u` is the weight of v1 and `v` is the
    // weight of v2. An attribute interpolates as A0*(1-u-v) + A1*u + A2*v.
    r.bary        = float2(u, v);
    r.triIdxLocal = meshLocalTriIdx;
    r.normalGeom  = normalize(Ng);
    return true;
}

void TraverseBVH(inout Ray ray, EntityHandle entityHandle) {
    uint nodeOffset = entityHandle.rootNodeIdx;  // consecutive BVHs in one buffer
    uint nodeIdx = 0;
    uint stack[64];
    uint stackPtr = 0;
    const float3 origin = ray.origin;
    const float3 invDir = 1.0 / ray.dir;

    while (true) {
        BVHNode node = NodeBuffer[nodeOffset + nodeIdx];
        if (node.triCount != 0) {   // leaf
            uint first = node.leftChild_Or_FirstTri;
            uint count = node.triCount;
            for (uint i = 0; i < count; i++) {
                uint triIndex = BvhPrimIndex[entityHandle.rootTriIdx + first + i];
                const TrianglePositions tri = TriPositionBuffer[entityHandle.rootTriIdx + triIndex];
                // The material is NOT assigned here -- it comes from
                // TriRef.materialSlot at resolve time, which is what makes
                // per-submesh materials cost zero extra loads.
                if (IntersectTri(ray, tri, triIndex)) {
                    g_TriIntersectionCount++;
                }
            }

            if (stackPtr == 0) { break; }
            nodeIdx = stack[--stackPtr];
            continue;
        }

        uint child1Idx = node.leftChild_Or_FirstTri;
        uint child2Idx = node.leftChild_Or_FirstTri + 1;

        float dist1 = IntersectAABB(origin, invDir,
            NodeBuffer[nodeOffset + child1Idx].min,
            NodeBuffer[nodeOffset + child1Idx].max, ray.t);
        float dist2 = IntersectAABB(origin, invDir,
            NodeBuffer[nodeOffset + child2Idx].min,
            NodeBuffer[nodeOffset + child2Idx].max, ray.t);

        if (dist1 > dist2) {
            float tmpDist = dist1; dist1 = dist2; dist2 = tmpDist;
            uint tmpIdx = child1Idx; child1Idx = child2Idx; child2Idx = tmpIdx;
        }

        if (dist1 >= INF_T) {
            if (stackPtr == 0) { break; }
            nodeIdx = stack[--stackPtr];
        } else {
            g_AabbIntersectionCount++;
            nodeIdx = child1Idx;
            if (dist2 < INF_T) {
                g_AabbIntersectionCount++;
                stack[stackPtr++] = child2Idx;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// THE ENTIRE ATTRIBUTE PATH, IN ONE FUNCTION, CALLED ONCE PER RAY.
//
// Cost: one 16-byte TriRef load, three 48-byte Vertex loads, ~30 ALU. That is
// PER RAY, not per triangle test -- traversal tests hundreds of triangles and
// touches only TriPositionBuffer. Do not move any of this into TraverseBVH or
// into the entity loop.
//
// FetchVertex is the sole chokepoint through which a vertex is read, so swapping
// to a packed vertex representation later (Phase 9) is a change to this function
// plus the C++ struct plus its static_asserts, and nothing else.
// -----------------------------------------------------------------------------
Vertex FetchVertex(uint idx) { return VertexBuffer[idx]; }

void ResolveHit(inout Ray ray) {
    EntityHandle e  = EntityLookupTable[ray.entityIdx];
    float4x4 model  = TransformBuffer[e.transformIdx];
    float3x3 normalMat = (float3x3)transpose(Inverse(model));

    TriRef tr = TriRefBuffer[e.rootTriIdx + ray.triIdxLocal];
    ray.materialIdx = e.materialBase + min(tr.materialSlot, max(e.materialSlotCount, 1u) - 1u);

    Vertex a = FetchVertex(tr.i0);
    Vertex b = FetchVertex(tr.i1);
    Vertex c = FetchVertex(tr.i2);

    float u = ray.bary.x, v = ray.bary.y, w = 1.0 - u - v;

    // UVs live in the .w lanes of positionU / normalV. See Gpu::Vertex.
    ray.uv = float2(a.positionU.w, a.normalV.w) * w
           + float2(b.positionU.w, b.normalV.w) * u
           + float2(c.positionU.w, c.normalV.w) * v;

    float3 Ns = normalize(mul(normalMat, a.normalV.xyz * w + b.normalV.xyz * u + c.normalV.xyz * v));
    if (!ray.frontFacing) Ns = -Ns;

    // Interpolated world-space tangent, kept for the shading frame. Tangents
    // transform by the MODEL matrix, not the inverse transpose -- see below.
    const float3 Ti = a.tangent.xyz * w + b.tangent.xyz * u + c.tangent.xyz * v;
    ray.tangent = (a.tangent.w != 0.0) ? mul((float3x3)model, Ti) : float3(0.0);

    Material mat = MaterialBuffer[ray.materialIdx];

    // --- tangent-space normal mapping ---
    // tangent.w == 0.0 is the invalid-tangent sentinel: the mesh had no UVs, so
    // there is no tangent frame and normal mapping must be skipped.
    float hand = a.tangent.w;
    if (hand != 0.0 && mat.textures.y != X3_INVALID_TEXTURE) {
        float3 Ti = a.tangent.xyz * w + b.tangent.xyz * u + c.tangent.xyz * v;
        // TANGENTS TRANSFORM BY THE MODEL MATRIX, NOT the inverse transpose.
        // Using one for both is the classic silent bug -- it only shows up under
        // non-uniform scale, as a subtly skewed TBN.
        float3 T = normalize(mul((float3x3)model, Ti));
        T = normalize(T - Ns * dot(Ns, T));       // Gram-Schmidt against Ns
        float3 B = cross(Ns, T) * hand;
        float3 nts = u_MaterialTextures[NonUniformResourceIndex(mat.textures.y)]
                        .SampleLevel(ray.uv, 0.0).xyz * 2.0 - 1.0;
        nts.xy *= mat.pbrParams.w;                // normalScale
        // Column-major construction: transpose(float3x3(T,B,Ns)) builds the
        // matrix whose COLUMNS are T, B, Ns, which is what GLSL's mat3(T,B,Ns)
        // built and what maps tangent space to world space.
        Ns = normalize(mul(transpose(float3x3(T, B, Ns)), nts));
    }

    // Never let the shading normal cross the geometric hemisphere -- a normal
    // map on a grazing surface can otherwise point the shading normal back into
    // the geometry, which produces black pixels that look like a lighting bug.
    if (dot(Ns, ray.normalGeom) < 0.0)
        Ns = normalize(Ns - ray.normalGeom * (dot(Ns, ray.normalGeom) * 1.0001));

    ray.normalShade = Ns;
}

public void CheckRayCollision(inout Ray ray) {
    ray.t = INF_T;
    for (uint i = 0; i < u_Settings.entityCount; i++) {
        // Transform the ray into the tested entity's local space.
        EntityHandle entityHandle = EntityLookupTable[i];
        float4x4 model = TransformBuffer[entityHandle.transformIdx];
        float4x4 invTransform = Inverse(model);

        Ray rayLocal;
        rayLocal.t = INF_T;
        rayLocal.origin = mul(invTransform, float4(ray.origin, 1.0)).xyz;
        rayLocal.dir    = mul(invTransform, float4(ray.dir, 0.0)).xyz;

        TraverseBVH(rayLocal, entityHandle);

        if (rayLocal.t < ray.t) {
            float3x3 normalMatrix = (float3x3)transpose(invTransform);  // inverse transpose
            float3 worldNormal = normalize(mul(normalMatrix, rayLocal.normalGeom));

            ray.t = length(mul((float3x3)model, rayLocal.dir * rayLocal.t));

            // NOT faceforward(). Record which side was hit and flip the
            // GEOMETRIC normal here; the shading normal is then flipped
            // consistently with it inside ResolveHit. Flipping the two
            // independently is how a backface ends up lit by a normal that
            // disagrees with its own geometry.
            ray.frontFacing = dot(worldNormal, ray.dir) < 0.0;
            ray.normalGeom  = ray.frontFacing ? worldNormal : -worldNormal;

            ray.bary        = rayLocal.bary;
            ray.triIdxLocal = rayLocal.triIdxLocal;
            ray.entityIdx   = i;
        }
    }

    // ONCE, after the loop -- not once per entity, and never for an occlusion
    // query, which only needs to know THAT something was hit.
    if (ray.t < INF_T && !ray.anyHit)
        ResolveHit(ray);
}

// Occlusion query. Shadow rays are the majority of CheckRayCollision calls (one
// per light per bounce), and every one of them would otherwise pay for an
// attribute fetch and a normal-map sample whose result is discarded.
public bool IsInShadow(float3 origin, float3 dirToLight, float distToLight) {
    Ray shadowRay;
    shadowRay.origin = origin;
    shadowRay.dir    = dirToLight;
    shadowRay.t      = distToLight - SURFACE_BIAS;   // stop just before the light
    shadowRay.anyHit = true;

    CheckRayCollision(shadowRay);

    return shadowRay.t < distToLight - SURFACE_BIAS;
}

// -----------------------------------------------------------------------------
// Applies the material's texture maps on top of its scalar factors. Each map
// MODULATES rather than replaces, which is the glTF convention: a white factor
// with a texture gives the texture, and a texture-less material gives the
// factor. Returns a Material whose texture indices are already consumed.
// -----------------------------------------------------------------------------
/// The extended lobe block for a material, or the neutral default when it names
/// none. Keeping the lookup here rather than at each call site is what lets a
/// shader read `ext` unconditionally without ever indexing an empty buffer.
public MaterialExt GetMaterialExt(Material mat) {
    if (mat.flags.y == X3_NO_MATERIAL_EXT) return DefaultMaterialExt();
    return MaterialExtBuffer[mat.flags.y];
}

public Material SampleMaterial(Material mat, float2 uv) {
    if (mat.textures.x != X3_INVALID_TEXTURE)
        mat.color.rgb *= u_MaterialTextures[NonUniformResourceIndex(mat.textures.x)]
                            .SampleLevel(uv, 0.0).rgb;

    if (mat.textures.z != X3_INVALID_TEXTURE) {
        // glTF ORM packing: R = occlusion, G = roughness, B = metallic.
        float3 orm = u_MaterialTextures[NonUniformResourceIndex(mat.textures.z)]
                        .SampleLevel(uv, 0.0).rgb;
        mat.pbrParams.x *= orm.b;   // metallic
        mat.pbrParams.y *= orm.g;   // roughness
        mat.pbrParams.z *= orm.r;   // ao
    }

    if (mat.textures.w != X3_INVALID_TEXTURE)
        mat.emission.rgb *= u_MaterialTextures[NonUniformResourceIndex(mat.textures.w)]
                                .SampleLevel(uv, 0.0).rgb;

    return mat;
}

// -----------------------------------------------------------------------------
// DIRECT LIGHTING -- ONE IMPLEMENTATION, shared by every renderer.
//
// It briefly lived in both PathTracing.slang and PBR.slang, and they diverged
// immediately: one attenuated point lights by 1/(1 + k*d^2) and the other by
// 1/d^2. The lights fixture rendered a spot-lit cylinder black in the path
// tracer and lit in PBR, which is precisely the "reference renderer stops being
// a reference" failure that sharing the BSDF was supposed to prevent -- sharing
// the BSDF is not enough if the light loop feeding it is duplicated.
//
// Phase 7's forward pass uses this too. Do not copy it.
//
// Direct lighting THROUGH THE BSDF. Returns the irradiance arriving from each
// light separately from the BSDF response, so the caller multiplies by f(wo,wi)
// rather than this function assuming Lambert.
//
// Before Phase 6 this loop assumed a purely diffuse surface and multiplied by
// albedo/pi implicitly -- a metal lit by a point light was shaded exactly like
// chalk. Splitting irradiance from response is what lets the same light loop
// serve every lobe, and is what Phase 7's forward pass will reuse verbatim.
// -----------------------------------------------------------------------------
public struct LightSample {
    public float3 radiance;   // incident radiance, already shadowed and attenuated
    public float3 wi;         // world-space direction towards the light
    public bool   valid;
};

/// TRUE while a RASTER pass is shading. It is what selects the cascaded shadow
/// map over a traced shadow ray for the one light that has cascades.
///
/// A MODULE-STATIC RATHER THAN A PARAMETER, matching g_AabbIntersectionCount
/// above, because it would otherwise have to be threaded through
/// DirectLightingClustered, SampleLightDirect and every caller of both for the
/// benefit of one branch.
///
/// THE REFERENCE MUST LEAVE THIS FALSE. PBR.slang and PathTracing.slang keep
/// tracing shadow rays, and that is the entire gate on this phase: if the
/// cascaded map and the traced shadow disagree, forward-lights stops matching
/// lights-pbr and says so. Setting it in the compute renderers would make the
/// two agree by construction and measure nothing.
public static bool g_UseShadowMaps = false;

/// Visibility of the cascaded light at a world position, in [0,1].
///
/// Returns 1 -- fully lit -- for anything beyond the last cascade rather than
/// guessing. The shadow far plane is much closer than the camera's (see
/// ComputeShadowCascades), so distant geometry is simply not covered, and
/// reporting "unshadowed" there is the failure that looks like missing shadows
/// rather than the one that looks like the world going dark.
public float SampleCascadedShadow(float3 posWorld, float3 normalGeom, float viewDepth) {
    uint c = X3_SHADOW_CASCADES;
    for (uint i = 0; i < X3_SHADOW_CASCADES; i++) {
        if (viewDepth <= u_Shadow.splitDepth[i]) { c = i; break; }
    }
    if (c >= X3_SHADOW_CASCADES) return 1.0;

    // NORMAL-OFFSET BIAS, SCALED BY THE CASCADE'S TEXEL SIZE. Moving the sample
    // point along the surface normal rather than along depth is what avoids
    // peter-panning: it slides the lookup sideways into the interior of the
    // surface instead of pushing the whole comparison away from the light, so a
    // contact shadow stays attached to its caster.
    //
    // The scale MUST be per cascade. One shadow texel of the far cascade covers
    // a patch of world tens of times larger than one of the near cascade, so a
    // fixed offset is either useless out there or detaches everything up close.
    const float3 p = posWorld + normalGeom * (u_Shadow.texelWorldSize[c] * u_Shadow.depthBiasScale);

    const float4 clip = mul(u_Shadow.viewProj[c], float4(p, 1.0));
    // Orthographic: w is 1 and there is no perspective divide to do. Dividing
    // anyway is harmless and survives a future switch to a perspective light.
    const float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0)) return 1.0;

    // Tile u into the atlas. v is untouched -- the cascades sit side by side.
    float2 uv = ndc.xy * 0.5 + 0.5;
    uv.x = (uv.x + float(c)) / float(X3_SHADOW_CASCADES);

    // 3x3 PCF, taps one texel apart WITHIN THE CASCADE. The x step is divided by
    // the cascade count because a texel of the atlas is that much narrower in
    // atlas-uv than a texel of one cascade -- getting this wrong samples across
    // the tile boundary into a neighbouring cascade at the edges, which shows up
    // as a hard seam exactly where two cascades meet.
    const float texelV = 1.0 / float(X3_SHADOW_MAP_RESOLUTION);
    const float texelU = texelV / float(X3_SHADOW_CASCADES);

    float lit = 0.0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const float stored = u_ShadowAtlas.SampleLevel(
                uv + float2(float(dx) * texelU, float(dy) * texelV), 0.0).r;
            // REVERSE-Z: a LARGER stored depth is CLOSER to the light. This
            // fragment is lit when it is at least as close as the nearest caster
            // the map recorded, so the comparison is GREATER-OR-EQUAL, not the
            // LESS that forward-Z shadow-map code uses. Copying the conventional
            // test here inverts every shadow in the scene.
            lit += (ndc.z >= stored) ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

/// How many occlusion rays a soft light spends. ENGINE_PLAN.md asks for a
/// per-light budget with a shadow-map fallback; this is the budget half, and the
/// fallback is that the one light with cascades never reaches here at all.
///
/// A hard light (softness 0) spends exactly ONE ray and is unaffected.
public static const uint X3_SOFT_SHADOW_TAPS = 8;

/// A DETERMINISTIC low-discrepancy disc, not a random one, and that is the whole
/// reason soft shadows are checkable here.
///
/// A stochastic tap pattern converges in the path tracer, which accumulates
/// hundreds of samples, and stays noisy in the raster path, which takes one --
/// so the two would disagree by construction and "the rasterizer agrees with the
/// reference" would stop meaning anything. A fixed pattern is identical in both.
///
/// The cost is banding instead of noise: eight taps resolve a penumbra into
/// eight steps rather than a smooth gradient. That is the honest trade at one
/// sample per pixel, and TAA in Phase 11 is what makes a stochastic pattern
/// affordable later.
///
/// Golden-angle spiral: successive points are 137.5 degrees apart, which is the
/// most uniform way to fill a disc with an arbitrary number of samples.
float2 SoftShadowDiscSample(uint i) {
    const float golden = 2.39996323;                  // pi * (3 - sqrt(5))
    const float r = sqrt((float(i) + 0.5) / float(X3_SOFT_SHADOW_TAPS));
    const float a = float(i) * golden;
    return float2(cos(a), sin(a)) * r;
}

/// Fraction of the light visible from `origin`, in [0,1].
///
/// `softness` is the light's ANGULAR radius in radians. Zero takes the single-ray
/// path and is bit-identical to what IsInShadow did before soft shadows existed,
/// which is what keeps every scene authored without a softness value rendering
/// exactly as it did.
float TracedVisibility(float3 origin, float3 toLight, float dist, float softness) {
    if (softness <= 0.0) return IsInShadow(origin, toLight, dist) ? 0.0 : 1.0;

    // An orthonormal basis around the light direction, to spread taps across the
    // disc the light subtends.
    const float3 up = (abs(toLight.y) > 0.999) ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    const float3 tx = normalize(cross(up, toLight));
    const float3 ty = cross(toLight, tx);

    const float spread = tan(softness);

    uint visible = 0;
    for (uint i = 0; i < X3_SOFT_SHADOW_TAPS; i++) {
        const float2 d = SoftShadowDiscSample(i) * spread;
        const float3 dir = normalize(toLight + tx * d.x + ty * d.y);
        if (!IsInShadow(origin, dir, dist)) visible++;
    }
    return float(visible) / float(X3_SOFT_SHADOW_TAPS);
}

public LightSample SampleLightDirect(LightData light, uint lightIndex,
                                     float3 hitPoint, float3 normalGeom) {
    LightSample s;
    s.radiance = float3(0.0);
    s.wi = float3(0.0, 1.0, 0.0);
    s.valid = false;

    const uint type = uint(light.position.w);
    float3 toLight;
    float dist = 1e20;
    float attenuation = 1.0;

    if (type == 0) {                       // directional
        toLight = -normalize(light.direction.xyz);
    } else {                               // point / spot
        const float3 v = light.position.xyz - hitPoint;
        dist = length(v);
        if (dist > light.color.w) return s;     // .w is range
        toLight = v / dist;
        attenuation = 1.0 / (1.0 + light.params.x * dist * dist);

        if (type == 2) {                   // spot cone
            const float cosAngle = dot(-toLight, normalize(light.direction.xyz));
            const float inner = cos(light.params.y);
            const float outer = cos(light.params.z);
            if (cosAngle < outer) return s;
            attenuation *= smoothstep(outer, inner, cosAngle);
        } else if (type != 1) {
            return s;                      // unknown light type
        }
    }

    // Shadow test against the GEOMETRIC normal's offset, matching the bounce
    // ray's bias -- shading-normal offsets self-intersect on grazing
    // normal-mapped surfaces.
    if (dot(normalGeom, toLight) <= 0.0) return s;

    // ONE LIGHT GETS CASCADES; everything else traces. The cascaded path returns
    // a FRACTION rather than a boolean, because PCF is the whole point of having
    // a map -- so it scales the radiance instead of rejecting the sample.
    float visibility;
    if (g_UseShadowMaps && lightIndex == u_Shadow.shadowLightIndex) {
        // View depth, for cascade selection. u_Camera.view is world -> camera and
        // this engine's camera space has +Z forward, so z IS the depth.
        const float viewDepth = mul(u_Camera.view, float4(hitPoint, 1.0)).z;
        visibility = SampleCascadedShadow(hitPoint, normalGeom, viewDepth);
    } else {
        visibility = TracedVisibility(hitPoint + normalGeom * SURFACE_BIAS,
                                      toLight, dist, light.params.w);
    }
    if (visibility <= 0.0) return s;

    s.radiance = light.color.xyz * light.direction.w * attenuation * visibility;
    s.wi = toLight;
    s.valid = true;
    return s;
}

/// Crude skybox IBL. LIVES HERE, not in PBR.slang, because the forward pass
/// needs the identical ambient term -- the same reason DirectLighting does. A
/// second copy is how the raster path and the reference start disagreeing about
/// what a material looks like in shadow.
///
/// Crude skybox IBL. Split-sum without the prefiltered mip chain: the specular
/// term samples the mirror direction and is attenuated by roughness rather than
/// integrated. Proper prefiltered environment maps are Phase 11; this exists so
/// unlit sides of objects are not black.
public float3 AmbientIBL(Material mat, MaterialExt ext, float3 N, float3 V) {
    const float NdotV = saturate(dot(N, V));
    const float metallic  = saturate(mat.pbrParams.x);
    const float roughness = clamp(mat.pbrParams.y, 0.0, 1.0);
    const float ao        = mat.pbrParams.z;
    const float3 f0 = lerp(float3(0.08 * ext.specular.x), mat.color.rgb, metallic);

    const float3 diffuseIBL  = GetSkyboxLight(N) * mat.color.rgb * (1.0 - metallic);
    const float3 specularIBL = GetSkyboxLight(reflect(-V, N)) * (1.0 - roughness * 0.5);

    // THE SAME BAKED TABLE the direct lighting uses, so the ambient and direct
    // terms cannot disagree about how reflective the surface is. They used to
    // each apply their own approximation, which is how an object ends up
    // brighter in shadow than in light.
    const float3 kS = SpecularAlbedo(f0, NdotV, roughness);
    const float3 kD = (1.0 - kS) * (1.0 - metallic);

    return (kD * diffuseIBL + kS * specularIBL) * ao;
}

/// Sums direct lighting over every light, evaluating the supplied BSDF for each.
/// Generic over IBsdf, so a material with no extended lobes instantiates this on
/// MetalRoughBsdf and Slang eliminates the coat and sheen code entirely.
public float3 DirectLighting<T : IBsdf>(T bsdf, ShadingFrame frame, float3 woWorld,
                                 float3 hitPoint, float3 normalGeom) {
    float3 total = float3(0.0);
    const float3 wo = frame.toLocal(woWorld);
    if (wo.z <= 0.0) return total;

    for (uint i = 0; i < u_Settings.lightCount; i++) {
        const LightSample ls = SampleLightDirect(LightBuffer[i], i, hitPoint, normalGeom);
        if (!ls.valid) continue;

        const float3 wi = frame.toLocal(ls.wi);
        if (wi.z <= 0.0) continue;

        total += bsdf.eval(wo, wi) * wi.z * ls.radiance;
    }
    return total;
}

/// The same sum, over the CLUSTER'S light list instead of every light.
///
/// Deliberately a near-copy of the loop above rather than a shared inner
/// function: the bodies must stay identical, and the way to keep them so is that
/// any change to one is visibly a change that has to be made to the other. The
/// alternative -- a shared body parameterised by an index source -- hides the
/// pairing behind an abstraction and this engine has already been bitten once by
/// the path tracer and PBR quietly diverging on exactly this loop.
///
/// The light SET is identical when culling is correct, so this and the loop above
/// must agree to within float summation order. cluster-correctness is the gate on
/// that, and forward-vs-pbr is the gate on this.
public float3 DirectLightingClustered<T : IBsdf>(T bsdf, ShadingFrame frame, float3 woWorld,
                                 float3 hitPoint, float3 normalGeom, uint cluster) {
    float3 total = float3(0.0);
    const float3 wo = frame.toLocal(woWorld);
    if (wo.z <= 0.0) return total;
    if (cluster >= X3_CLUSTER_COUNT) return total;

    const uint base  = cluster * X3_MAX_LIGHTS_PER_CLUSTER;
    const uint count = ClusterLightGrid[cluster];

    for (uint j = 0; j < count; j++) {
        const uint li = ClusterLightIndices[base + j];
        const LightSample ls = SampleLightDirect(LightBuffer[li], li, hitPoint, normalGeom);
        if (!ls.valid) continue;

        const float3 wi = frame.toLocal(ls.wi);
        if (wi.z <= 0.0) continue;

        total += bsdf.eval(wo, wi) * wi.z * ls.radiance;
    }
    return total;
}




/// One bounce of the path, generic over the BSDF so the specialised
/// instantiation below carries only the lobes its material actually declares.

// -----------------------------------------------------------------------------
// Camera ray generation, identical in all three entry points.
// -----------------------------------------------------------------------------
public Ray MakeCameraRay(int2 texelCoords, int2 dims) {
    float x = float(texelCoords.x * 2 - dims.x) / float(dims.x);
    float y = float(texelCoords.y * 2 - dims.y) / float(dims.x);  // /x keeps the ratio

    float4 originH = mul(u_Camera.transform, float4(0.0, 0.0, 0.0, 1.0));

    Ray ray;
    ray.origin = originH.xyz / originH.w;
    // focalLength is the distance from the origin to the screen plane.
    ray.dir    = normalize(mul((float3x3)u_Camera.transform,
                               float3(x, y, u_Camera.focalLength)));
    ray.t      = INF_T;
    ray.anyHit = false;   // camera rays need the full attribute resolve
    return ray;
}
