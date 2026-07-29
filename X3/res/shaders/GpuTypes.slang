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
    public float4 params;      // x attenuation, y innerCone, z outerCone, w pad
};

// std140 - 272 B (CPU: Renderer::CameraUBOData)
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
    public float    focalLength;
    public float    nearPlane;
    public float    farPlane;
    public float    _pad;
};

// std140 - 32 B (CPU: Renderer::SettingsUBOData)
public struct SettingsUBO {
    public uint raysPerPixel;
    public uint bouncesPerRay;
    public uint numAccumulatedFrames;
    public uint entityCount;
    public uint debugMode;
    public uint aabbHeatmapCutoff;
    public uint triHeatmapCutoff;
    public uint lightCount;
};

public static const uint X3_INVALID_TEXTURE = 0xFFFFFFFFu;

/// Material::flags.y when the material uses no extended lobes.
public static const uint X3_NO_MATERIAL_EXT = 0xFFFFFFFFu;

// Must equal MAX_MATERIAL_TEXTURES in X3/src/Renderer/TextureTable.h. The
// generated descriptor table checks the two agree at build time.
public static const uint X3_MAX_MATERIAL_TEXTURES = 128;
