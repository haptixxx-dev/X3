// =============================================================================
// GpuTypes -- every struct mirrored between C++ and the GPU.
//
// !!! MIRRORED. The C++ side is X3/src/Renderer/GpuTypes.h (and BVHAccel::Node
// !!! in X3/src/Project/Assets/BVHAccel.h). Any edit here MUST be matched there,
// !!! and the static_assert block at the bottom of GpuTypes.h updated.
//
// This is a real Slang MODULE, not a textual include: it is separately compiled
// to IR and linked, so a type error here surfaces here rather than at every
// import site. It replaces GpuTypes.glsl, which replaced three hand-maintained
// copies of these declarations.
//
// LAYOUT RULE: everything is float4/uint4/uint, so std430, std140 and scalar all
// agree. Do not introduce a bare float3 followed by a float -- it packs
// differently under std140 than under std430/scalar, and the mismatch is silent.
// The build pins -matrix-layout-column-major and -force-glsl-scalar-layout so
// the byte layout matches what the C++ structs upload.
// =============================================================================

module GpuTypes;

// std430 - 32 B (CPU: BVHAccel::Node)
// `min`/`max` are kept as member names from the pre-Slang shaders on purpose.
public struct BVHNode {
    public float3 min;
    public uint   leftChild_Or_FirstTri;
    public float3 max;
    public uint   triCount;
    //  if triCount == 0: leftChild_Or_FirstTri == leftChild
    //  else             leftChild_Or_FirstTri == firstTri
};

// std430 - 48 B (CPU: Gpu::TrianglePositions)
// De-referenced positions. BVH traversal reads THIS and nothing else.
public struct TrianglePositions {
    public float4 v0, v1, v2;
};

// std430 - 16 B (CPU: Gpu::TriRef)
// i0/i1/i2 are GLOBAL indices into VertexBuffer. materialSlot is mesh-local.
public struct TriRef {
    public uint i0, i1, i2;
    public uint materialSlot;
};

// std430 - 48 B (CPU: Gpu::Vertex)
// UVs ride in the .w lanes. tangent.w is handedness; 0.0 == no valid tangent.
public struct Vertex {
    public float4 positionU;
    public float4 normalV;
    public float4 tangent;
};

// std430 - 80 B (CPU: Gpu::Material)
// textures = (baseColor, normal, metalRough, emissive) indices into
// u_MaterialTextures[]; X3_INVALID_TEXTURE means "factor only".
// flags.x = feature bitfield (MATERIAL_FEATURE_* in Bsdf.slang)
// flags.y = index into MaterialExtBuffer, or X3_NO_MATERIAL_EXT
public struct Material {
    public float4 emission;
    public float4 color;
    public float4 pbrParams;
    public uint4  textures;
    public uint4  flags;
};

// std430 - 64 B (CPU: Gpu::MaterialExt)
//
// THE SECOND TIER. Only materials whose flags name an extended lobe get an
// entry, so a scene of plain metallic-roughness materials uploads none of this
// and every shader that instantiates MetalRoughBsdf directly has it eliminated
// at link time. That is the whole reason the lobes are not simply fields on
// Material.
public struct MaterialExt {
    public float4 clearcoat;   // x weight, y roughness, z ior, w pad
    public float4 sheen;       // xyz colour, w roughness
    public float4 aniso;       // x anisotropy, y rotation, z thinFilmThickness, w thinFilmIor
    public float4 specular;    // x specularLevel, yzw specular tint
};

// std430 - 32 B (CPU: Gpu::MeshEntityHandle)
public struct EntityHandle {
    public uint rootTriIdx;
    public uint triCount;
    public uint rootNodeIdx;
    public uint nodeCount;
    public uint transformIdx;
    public uint materialBase;
    public uint materialSlotCount;
    public uint firstVertexIdx;
};

// std430 - 64 B (CPU: Gpu::LightData)
public struct LightData {
    public float4 position;    // xyz position, w type (0=dir, 1=point, 2=spot)
    public float4 direction;   // xyz direction, w intensity
    public float4 color;       // xyz colour, w range
    public float4 params;      // x attenuation, y innerCone, z outerCone, w softness (radians)
};

// std140 - 416 B (CPU: Renderer::CameraUBOData)
//
// `transform` is the camera's world matrix and `focalLength` its ray-generation
// parameter -- between them they are everything the PATH TRACER needs, which is
// why they were the whole struct until Phase 7.
//
// A RASTERIZER CANNOT USE EITHER. It needs to project a world-space vertex to
// clip space, which is view * proj, and neither is derivable from a focal length
// alone: the projection also encodes the aspect ratio and the near/far planes,
// and the view matrix is the INVERSE of `transform`, which a shader would
// otherwise have to invert per vertex.
public struct CameraUBO {
    public float4x4 transform;      // camera -> world
    public float4x4 view;           // world -> camera (inverse of transform)
    public float4x4 proj;           // camera -> clip, reverse-Z
    public float4x4 viewProj;       // premultiplied, so a vertex shader does one mul
    public float4x4 prevViewProj;   // last frame's, for the velocity pass
    public float4x4 viewProjJittered;  // what raster passes project with
    public float4   jitter;         // xy this frame, zw last frame, in NDC
    public float    focalLength;
    public float    nearPlane;
    public float    farPlane;
    public float    _pad;
};

public static const uint X3_SHADOW_CASCADES  = 4;
/// Side length of ONE cascade's square region. The atlas is this tall and
/// X3_SHADOW_CASCADES times this wide. Must equal Gpu::SHADOW_MAP_RESOLUTION.
public static const uint X3_SHADOW_MAP_RESOLUTION = 1024;
public static const uint X3_NO_SHADOW_LIGHT  = 0xFFFFFFFFu;

// std140 - 304 B (CPU: Renderer::ShadowUBOData)
//
// A UBO OF ITS OWN rather than more fields on CameraUBO. The cascade matrices
// are a property of one light, not of the camera, and CameraUBO is already 336 B
// bound by every pipeline in the engine -- including the ones that will never
// look at a shadow.
//
// float4 for the split distances rather than a float[4], because std140 pads
// every array element to 16 bytes: `float splitDepth[4]` occupies 64 B with 48
// of them padding, and the C++ mirror would have to reproduce that padding
// exactly. One float4 is 16 B on both sides with nothing to get wrong.
public struct ShadowUBO {
    public float4x4 viewProj[X3_SHADOW_CASCADES];   // world -> light clip, reverse-Z
    public float4   splitDepth;                     // view-space far distance per cascade
    public float4   texelWorldSize;                 // world size of one shadow texel per cascade

    /// Index into LightBuffer of the light these cascades belong to, or
    /// X3_NO_SHADOW_LIGHT when the scene has no directional light. Only one
    /// light gets cascades; everything else keeps tracing.
    public uint  shadowLightIndex;
    public float depthBiasScale;                    // multiplies the normal offset
    public float _pad0;
    public float _pad1;
};

// std140 - 44 B padded to 48 (CPU: Renderer::SettingsUBOData)
public struct SettingsUBO {
    public uint raysPerPixel;
    public uint bouncesPerRay;
    public uint numAccumulatedFrames;
    public uint entityCount;
    public uint debugMode;
    public uint aabbHeatmapCutoff;
    public uint triHeatmapCutoff;
    public uint lightCount;
    /// 0 on the first frame after a resize, a scene change or a settings change.
    /// Blending against an uninitialised history image would smear whatever the
    /// allocator returned across the next several frames.
    public uint taaHistoryValid;
    public uint _pad0;
    public uint _pad1;
};

// =============================================================================
// CLUSTERED FORWARD+ -- the light-culling grid.
//
// The frustum is diced into X * Y screen tiles by Z depth slices, each cluster
// gets the list of lights that can reach it, and the forward pass loops that
// list instead of every light in the scene.
//
// DEPTH SLICES ARE EXPONENTIAL, not linear. A linear slicing puts almost every
// slice beyond anything the camera is looking at -- with near 0.05 and far
// 1000, the first linear slice alone spans 41 units, which is the whole of a
// typical interior. The exponential distribution z(k) = near * (far/near)^(k/Z)
// makes each slice a constant RATIO deeper than the last, so slice thickness
// tracks the depth precision the projection actually has. This is the same
// distribution as Olsson/Assarsson clustered shading and the same one DOOM 2016
// documented.
// =============================================================================

public static const uint X3_CLUSTER_X = 16;
public static const uint X3_CLUSTER_Y = 9;
public static const uint X3_CLUSTER_Z = 24;
public static const uint X3_CLUSTER_COUNT = X3_CLUSTER_X * X3_CLUSTER_Y * X3_CLUSTER_Z;

// FIXED SLOTS PER CLUSTER, not a compacted list allocated from a global atomic
// counter. The compacted form saves memory and costs a counter buffer, a reset
// pass, and per-run-nondeterministic offsets; the whole worst case here is
// 3456 * 64 * 4 = 884 KB, which is not worth any of that. It also keeps the
// light list deterministic, which the golden-image tests depend on.
public static const uint X3_MAX_LIGHTS_PER_CLUSTER = 64;

// std430 - 32 B (CPU: Gpu::ClusterAABB). VIEW SPACE.
//
// View space rather than world space because the cluster grid is fixed to the
// camera: rebuilding it per frame in world space would mean rebuilding it on
// every camera MOVE, whereas in view space it only changes when the PROJECTION
// does. The lights are transformed into view space to test against it, which is
// one transform per light per cluster instead of one per cluster per frame.
public struct ClusterAABB {
    public float4 minPoint;   // xyz view-space min, w unused
    public float4 maxPoint;   // xyz view-space max, w unused
};

/// The cluster a VIEW-SPACE position falls in.
///
/// Both the culling pass and every consumer must derive this the same way, so it
/// lives here rather than being written twice. Deriving it from view space (not
/// from gl_FragCoord) is what lets the compute-shader validation pass and the
/// raster forward pass agree by construction.
///
/// Returns X3_CLUSTER_COUNT for a position BEHIND the camera, and only for that.
///
/// LATERAL EXTENT IS CLAMPED, NOT REJECTED, and that is deliberate. Rejecting on
/// `abs(ndc) > 1` looks like the careful thing to do and is wrong at the one
/// place it fires: a point on the frustum edge computes an ndc of exactly +/-1 in
/// exact arithmetic and a hair outside it in floats. The first version of this
/// did reject, and debugMode 5 painted a one-pixel blue stripe down the left edge
/// of the frame -- 149 pixels of "this visible surface belongs to no cluster",
/// which is not a thing that can be true.
///
/// Every consumer asks about a point it is already shading, and such a point is
/// inside the frustum by construction, so the saturating min() below is the whole
/// answer. A point far outside laterally does clamp to an edge cluster whose
/// light list was not culled for it -- and nothing shades such a point.
public uint ClusterIndexFromView(float3 posView, float4x4 proj, float nearZ, float farZ) {
    if (posView.z <= 0.0) return X3_CLUSTER_COUNT;

    const float4 clip = mul(proj, float4(posView, 1.0));
    if (clip.w <= 0.0) return X3_CLUSTER_COUNT;
    const float2 ndc = clip.xy / clip.w;

    const uint tx = min(uint(max(ndc.x * 0.5 + 0.5, 0.0) * float(X3_CLUSTER_X)), X3_CLUSTER_X - 1);
    const uint ty = min(uint(max(ndc.y * 0.5 + 0.5, 0.0) * float(X3_CLUSTER_Y)), X3_CLUSTER_Y - 1);

    // max() against the near plane, so the log is never negative and the slice
    // index is never below zero.
    const float z = max(posView.z, nearZ);
    const uint tz = min(uint(log(z / nearZ) / log(farZ / nearZ) * float(X3_CLUSTER_Z)),
                        X3_CLUSTER_Z - 1);

    return tx + X3_CLUSTER_X * (ty + X3_CLUSTER_Y * tz);
}

/// The view-space depth range of slice k. Inverse of the slicing above.
public float2 ClusterSliceDepth(uint k, float nearZ, float farZ) {
    const float ratio = farZ / nearZ;
    return float2(nearZ * pow(ratio, float(k)     / float(X3_CLUSTER_Z)),
                  nearZ * pow(ratio, float(k + 1) / float(X3_CLUSTER_Z)));
}

// std140 - 112 B (CPU: Renderer::DdgiUBOData)
//
// Every member is 16 B, obeying this file's layout rule. `rayRotation` is a unit
// quaternion re-randomised per frame on the CPU rather than hashed per pass:
// the trace and blend passes both recompute ray directions from the ray index
// and must agree BIT FOR BIT, and a CPU value also keeps the passes
// reproducible for the golden images.
public struct DdgiUBO {
    public float4 gridOrigin;    // xyz world position of logical probe (0,0,0)
    public float4 gridSpacing;   // xyz world distance between adjacent probes
    public float4 rayRotation;   // xyzw unit quaternion, per frame
    public float4 params0;       // x hysteresis, y depthSharpness, z normalBias, w viewBias
    public float4 params1;       // x maxRayDistance, y energyPreservation, z volumeFade, w pad
    public int4   scroll;        // xyz probe-grid scroll, w pad
    public uint4  counts;        // x frameIndex, y raysPerProbe, z enabled, w pad
};

public static const uint X3_INVALID_TEXTURE = 0xFFFFFFFFu;

/// Material::flags.y when the material uses no extended lobes.
public static const uint X3_NO_MATERIAL_EXT = 0xFFFFFFFFu;

// Must equal MAX_MATERIAL_TEXTURES in X3/src/Renderer/TextureTable.h. The
// generated descriptor table checks the two agree at build time.
public static const uint X3_MAX_MATERIAL_TEXTURES = 128;
