#ifndef X3_GPU_TYPES_GLSL
#define X3_GPU_TYPES_GLSL

// =============================================================================
// !!! MIRRORED. The C++ side is X3/src/Renderer/GpuTypes.h (and BVHAccel::Node
// !!! in X3/src/Project/Assets/BVHAccel.h). ANY edit here MUST be matched there,
// !!! and the static_assert block at the bottom of GpuTypes.h updated.
//
// Before Phase 2 these declarations were copy-pasted into all three .comp files,
// so every layout fix had to be made three times and drift was a matter of when.
// This file is included by all three instead. Phase 3's Slang reflection is what
// finally removes the hand-sync entirely.
//
// LAYOUT RULE: everything is vec4/uvec4/uint, so std430, std140 and scalar all
// agree. Do not introduce a bare vec3 followed by a float -- it packs in std430
// and scalar but not in std140, and the mismatch is silent.
// =============================================================================

// std430 - 32 B (CPU: BVHAccel::Node, Project/Assets/BVHAccel.h)
// Member names min/max are kept from the pre-Phase-2 shaders on purpose.
struct BVHNode {
    vec3 min;
    uint leftChild_Or_FirstTri;
    vec3 max;
    uint triCount;
    /*  if triCount == 0: leftChild_Or_FirstTri == leftChild
        else             leftChild_Or_FirstTri == firstTri  */
};

// std430 - 48 B (CPU: Gpu::TrianglePositions)
// De-referenced positions. BVH traversal reads THIS and nothing else.
struct TrianglePositions { vec4 v0, v1, v2; };

// std430 - 16 B (CPU: Gpu::TriRef)
// i0/i1/i2 are GLOBAL indices into VertexBuffer. materialSlot is mesh-local.
struct TriRef { uint i0; uint i1; uint i2; uint materialSlot; };

// std430 - 48 B (CPU: Gpu::Vertex)
// UVs ride in the .w lanes. tangent.w is handedness; 0.0 == no valid tangent.
struct Vertex { vec4 positionU; vec4 normalV; vec4 tangent; };

// std430 - 64 B (CPU: Gpu::Material)
// textures = (baseColor, normal, metalRough, emissive) indices into
// u_MaterialTextures[]; X3_INVALID_TEXTURE means "factor only".
struct Material { vec4 emission; vec4 color; vec4 pbrParams; uvec4 textures; };

// std430 - 32 B (CPU: Gpu::MeshEntityHandle)
struct EntityHandle {
    uint rootTriIdx; uint triCount; uint rootNodeIdx; uint nodeCount;
    uint transformIdx; uint materialBase; uint materialSlotCount; uint firstVertexIdx;
};

// std430 - 64 B (CPU: Gpu::LightData)
struct LightData { vec4 position; vec4 direction; vec4 color; vec4 params; };

#define X3_INVALID_TEXTURE 0xFFFFFFFFu

// Must equal Gpu::MAX_MATERIAL_TEXTURES in X3/src/Renderer/TextureTable.h and
// the `count` on set 0 binding 2 in Renderer.cpp's kComputeSetLayouts.
#define X3_MAX_MATERIAL_TEXTURES 128

#endif
