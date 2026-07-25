# EXECUTION SPEC — Phase 2: Mesh Format and Vertex Attributes

**Repo:** `/home/sarah/Coding/Haptixxx/X3`, branch `vulkan-migration`. Assumes Phase 0 and Phase 1 are complete (OpenGL deleted, per-frame descriptor sets and buffer rings in place). Nothing builds on this machine; all claims below are from reading source.

---

## 0. Ground truth (verified, cite these when in doubt)

| Fact | Location |
|---|---|
| `struct Triangle { glm::vec4 v0, v1, v2; }` — positions only | `X3/src/Project/Assets/AssetTypes.h:12-14` |
| `struct Material` = 3× `vec4` = 48 B | `X3/src/Project/Assets/AssetTypes.h:17-21` |
| `MeshMetadata` = firstTriIdx / TriCount / firstNodeIdx / nodeCount | `X3/src/Project/Assets/AssetTypes.h:27-33` |
| AssetPool buffers: `MeshBuffer`, `IndexBuffer`, `NodeBuffer`, `TextureBuffer` | `X3/src/Project/Assets/AssetManager.h:45-48` |
| `AssetPool::AssetType` version-counter enum | `X3/src/Project/Assets/AssetManager.h:62-69` |
| Import reads **only** `subMesh->mVertices` | `X3/src/Project/Assets/AssetManager.cpp:212-227` |
| Preset `aiProcessPreset_TargetRealtime_MaxQuality` | `X3/src/Project/Assets/AssetManager.cpp:190` |
| BVH built over `meshBuffer` slice | `X3/src/Project/Assets/AssetManager.cpp:232-233` |
| `stbi_set_flip_vertically_on_load(1)` — "OpenGL-style orientation" | `X3/src/Project/Assets/AssetManager.cpp:259` |
| Textures appended into one flat `std::vector<unsigned char>` | `X3/src/Project/Assets/AssetManager.cpp:270-279` |
| Primitives built from bare position triples via `makeTri` | `X3/src/Project/Assets/AssetManager.cpp:320-357` |
| `BVHAccel::Node` = 32 B | `X3/src/Project/Assets/BVHAccel.h:13-20` |
| `BVHAccel` ctor takes `const std::vector<Triangle>&` | `X3/src/Project/Assets/BVHAccel.h:50`, `BVHAccel.cpp:7-10` |
| Centroids from `v0+v1+v2` only | `X3/src/Project/Assets/BVHAccel.h:65-73` |
| `UpdateAABB` grows over `t.v0/v1/v2` only | `X3/src/Project/Assets/BVHAccel.cpp:42-53` |
| Binning grows over `tri.v0/v1/v2` only | `X3/src/Project/Assets/BVHAccel.cpp:78-83` |
| Prim-index slice aliases the triangle slice: `m_IdxBuff = &indexBuffer[m_FirstTriIdx]` | `X3/src/Project/Assets/BVHAccel.cpp:25` |
| `MeshEntityHandle` = 6× uint = 24 B, **private nested in `Renderer`** | `X3/src/Renderer/Renderer.h:42-56` |
| `LightData` = 4× vec4 = 64 B, private nested | `X3/src/Renderer/Renderer.h:59-64` |
| One material per entity from `MaterialComponent` | `X3/src/Renderer/Renderer.cpp:172-190` |
| Skybox is the sole texture consumer of `TextureBuffer` | `X3/src/Renderer/Renderer.cpp:238-249` |
| SSBO bindings 0-6 all in set 2; mesh/node/index rebuilt on version change | `X3/src/Renderer/Renderer.cpp:253-351` |
| GLSL struct mirrors (×3 files) | `PathTracing.comp:33-71`, `PBR.comp:27-63`, `Phong.comp:27-63` |
| `vec3 Ng = cross(E1, E2)` — flat shading | `PathTracing.comp:227`, `PBR.comp:142`, `Phong.comp:142` |
| Leaf loop / material assignment | `PathTracing.comp:258-265` |
| Entity loop, normal transform, nearest-hit resolve | `PathTracing.comp:313-338` |
| Shading uses `mat.color.xyz` and `ray.normal` | `PathTracing.comp:459-477` |
| Descriptor tables hand-written by comment | `X3/src/Platform/Vulkan/VulkanComputeShader.cpp:19-42` |
| **`write.descriptorCount = 1; // Assuming count is 1 for now`** | `X3/src/Platform/Vulkan/VulkanComputeShader.cpp:206` |
| `DescriptorBinding::count` exists but is ignored | `X3/src/Platform/Vulkan/VulkanComputeShader.h:15` |
| Resource registry keyed by binding number only, no arrays | `X3/src/Platform/Vulkan/VulkanContext.h:44-56` |
| Vulkan 1.2 required; **no device features requested anywhere** | `VulkanContext.cpp:52`, `:75-78`, `:95-104` |
| Descriptor pool: 1000 of each type, maxSets 1000 | `X3/src/Platform/Vulkan/VulkanContext.cpp:463-481` |
| Texture: `mipLevels = 1`, format hardcoded `R8G8B8A8_SRGB`, sampler `maxLod = 0` | `VulkanTexture2D.cpp:84`, `:86`, `:142`, `:174` |
| Physics reads `AssetPool::MeshBuffer` as `Triangle` | `X3/src/Physics/PhysicsWorld.cpp:1033-1039`, `:1095-1102` |
| `MaterialComponent` is one flat material | `X3/src/Project/Scene/Components.h:62-71` |
| `.lrscn` material (de)serialization | `Scene.cpp:205-220`, `Scene.cpp:465-473` |
| `.lrscn` root keys — **no version key** | `Scene.cpp:146-151` |
| `.lrmeta` = `{Guid, SourcePath}` **only** | `AssetManager.cpp:25-38`, `:50-63` |
| `.lrproj` = `{bootSceneGuid, RenderSettings}` **only** | `X3/src/Project/ProjectManager.h:24-29` |
| Shader compile rule (globs `*.comp`, no `-I`) | `X3/CMakeLists.txt:83-98` |
| assimp `CalcTangentSpace` bails without normals **or** UV0 | `X3/libs/assimp/code/PostProcessing/CalcTangentsProcess.cpp:121-127` |
| Preset chain includes `CalcTangentSpace`, `GenSmoothNormals`, `JoinIdenticalVertices`, `Triangulate`, `GenUVCoords`, `SortByPType`, `OptimizeMeshes` | `X3/libs/assimp/include/assimp/postprocess.h:666-701` |
| `aiMesh::HasTextureCoords(0)`, `mTangents`, `mBitangents` | `X3/libs/assimp/include/assimp/mesh.h:488-498, 568, 588` |
| `aiScene::GetEmbeddedTexture()` for `.glb` | `X3/libs/assimp/include/assimp/scene.h:441-466` |

### Two latent bugs this phase must fix, because it rewrites the code that contains them

1. **Node transforms are discarded.** `AssetManager.cpp:212` iterates `scene->mMeshes[i]` flat and never touches `scene->mRootNode` / `aiNode::mTransformation`. Any file whose meshes sit under a transformed node imports at the wrong position/orientation/scale. Currently masked because the sample `.glb`s are single-node.
2. **Embedded textures are unreachable.** Both sample models (`SampleModels/*.glb`) embed their textures. `LoadTexture` (`AssetManager.cpp:250`) only takes a filesystem path. Material textures from `.glb` will resolve to paths like `*0` and fail until `aiScene::GetEmbeddedTexture` is wired in.

---

## 1. Proposed new mesh representation

### 1.1 The decision, up front

**Three buffers, not one:**

| Buffer | Contents | Consumer | Why |
|---|---|---|---|
| `TriPositionBuffer` | de-referenced 3× `vec4` positions per triangle, **48 B/tri** | BVH build + BVH traversal **only** | Keeps the traversal inner loop at one contiguous 48-byte load per triangle test — byte-identical to today's layout, so `IntersectTri` and the whole build path are untouched |
| `TriRefBuffer` | 3 global vertex indices + material slot, **16 B/tri** | attribute fetch at a hit; submesh draw ranges in Phase 7 | One aligned 16-byte load gives everything needed to resolve a hit |
| `VertexBuffer` | de-duplicated `Vertex`, **48 B/vertex** | attribute fetch at a hit; real vertex buffer in Phase 7 | assimp's `JoinIdenticalVertices` already de-dups; this is the buffer Forward+ binds |

**Indexed vs fat-triangle: both, deliberately.** Attributes are indexed; positions are not. Rationale:

- Traversal tests hundreds of triangles per ray and needs *only* positions. Making it indexed costs 3 scattered index loads *then* 3 dependent scattered position loads — six uncoalesced fetches with a dependency chain, per triangle test. The de-referenced copy costs one.
- Attributes are fetched **once per ray**, at the winning hit. Indexing them is free at that frequency and saves ~3× the memory (48 B/vertex vs 144 B/triangle if de-referenced).
- The redundancy is bounded: ~48 B/tri of duplicated positions. A 1 M-triangle scene pays +48 MB. `BVHAccel::Node` already costs ~64 MB at that scale (`nodeBuffer.resize(… + 2N-1)`, `BVHAccel.cpp:21`). This is not the memory problem.
- Phase 9's cook step is the designated place to revisit this (`ENGINE_PLAN.md:299` — meshoptimizer). Do not pre-optimize here.

**Positions stay separate from attributes for BVH cache behaviour: yes, explicitly.** This is the single most important layout decision in the phase.

### 1.2 Attribute packing: do NOT pack. Justification.

**No octahedral normals. No half-float UVs. No quantized tangents.**

The proposed `Vertex` is 48 B laid out as three `vec4`s, with UVs living in the `.w` lanes that would otherwise be pure padding — exactly the "extra padding float per vertex … might use the space for some other data in the future" that `AssetTypes.h:11` anticipated:

```cpp
struct Vertex {              // std430 == std140 == scalar layout, 48 B, align 16
    glm::vec4 positionU;     // offset  0 : xyz = position,  w = uv.x
    glm::vec4 normalV;       // offset 16 : xyz = normal,    w = uv.y
    glm::vec4 tangent;       // offset 32 : xyz = tangent,   w = handedness (+1 / -1, 0 = invalid)
};
```

Why not pack:
- The zero-padding layout above wastes **nothing**. A packed `{vec3 pos; uint octN; uint octT; uint uvHalf;}` is 24 B before std430 padding rounds it — a 2× win on a buffer that is not the bottleneck (traversal reads `TriPositionBuffer`, not this).
- This phase's exit criterion is *"smooth-shaded, textured, normal-mapped geometry"* (`ENGINE_PLAN.md:159`). Its risk is correctness. Adding an encode/decode layer on the same commit that introduces normals for the first time makes every wrong-looking pixel ambiguous between "my interpolation is wrong" and "my octahedral decode is wrong."
- Phase 3 (Slang, `ENGINE_PLAN.md:187` — layout pinning) and Phase 9 (cook + meshoptimizer + BC7) both re-lay-out this data anyway. Packing here is work done twice.
- Three `vec4`s means std430, std140 and scalar layouts all agree, which removes a whole class of C++/GLSL mismatch bugs at precisely the moment when the mirror-by-comment problem (§3) is at its worst.

**Not painting into a corner:** every shader read of a vertex goes through one function, `Vertex FetchVertex(uint idx)`. Swapping to a packed representation later is a change to that function, the C++ struct, and the six `static_assert`s — nothing else.

### 1.3 Tangents: full `vec4` with handedness sign. Not reconstructed.

- `w` costs zero bytes in this layout (it is padding otherwise).
- Bitangent is **not** stored: `B = cross(N, T) * tangent.w`. Standard, matches glTF, saves 12 B/vertex.
- Reconstructing tangents per-hit from UV/position derivatives yields the *per-face* tangent. assimp's `CalcTangentSpace` smooths across the 45° default `AI_CONFIG_PP_CT_MAX_SMOOTHING_ANGLE` (`CalcTangentsProcess.cpp:75-77`). Faceted tangents on curved, smooth-shaded surfaces produce visible normal-map seams. Store what assimp computed.
- `w == 0.0` is the **invalid tangent sentinel** (mesh had no UVs → assimp produced no tangents, `CalcTangentsProcess.cpp:125-127`). The shader must branch on it.

### 1.4 Final C++ definitions

New file **`X3/src/Renderer/GpuTypes.h`**. This file is the single C++ home for every GPU-mirrored struct — it collapses today's spread across `AssetTypes.h`, `BVHAccel.h` and `Renderer.h` into one place, and becomes the obvious target for Phase 3's reflection codegen.

```cpp
#pragma once
#include "lrpch.h"          // glm
#include <cstddef>          // offsetof
#include <type_traits>

namespace X3::Gpu
{
    // ---- 48 B. BVH-only. Byte-identical to the old X3::Triangle. ----
    struct TrianglePositions {
        glm::vec4 v0{}, v1{}, v2{};
    };

    // ---- 16 B. GLOBAL vertex indices (into VertexBuffer) + mesh-local material slot. ----
    struct TriRef {
        uint32_t i0 = 0, i1 = 0, i2 = 0;
        uint32_t materialSlot = 0;
    };

    // ---- 48 B. UVs live in the .w lanes. See §1.2. ----
    struct Vertex {
        glm::vec4 positionU{};   // xyz position, w uv.x
        glm::vec4 normalV{};     // xyz normal,   w uv.y
        glm::vec4 tangent{};     // xyz tangent,  w handedness (+1/-1); 0 == no valid tangent
    };

    static constexpr uint32_t INVALID_TEXTURE = 0xFFFFFFFFu;

    // ---- 64 B. First 48 B are byte-identical to the old X3::Material. ----
    struct Material {
        glm::vec4  emission  = { 0.0f, 1.0f, 0.0f, 1.0f }; // xyz colour, w strength
        glm::vec4  color     = { 0.0f, 0.0f, 0.0f, 1.0f }; // xyz baseColor factor, w alpha
        glm::vec4  pbrParams = { 0.0f, 0.5f, 1.0f, 1.0f }; // x metallic, y roughness, z ao, w normalScale
        glm::uvec4 textures  = { INVALID_TEXTURE, INVALID_TEXTURE,
                                 INVALID_TEXTURE, INVALID_TEXTURE };
                                 // x baseColor, y normal, z metalRough, w emissive
    };

    // ---- 32 B (was 24). ----
    struct MeshEntityHandle {
        uint32_t firstTriIdx  = 0;
        uint32_t triCount     = 0;
        uint32_t firstNodeIdx = 0;
        uint32_t nodeCount    = 0;
        uint32_t transformIdx = 0;
        uint32_t materialBase = 0;   // index of this entity's slot 0 in MaterialBuffer
        uint32_t materialSlotCount = 0;
        uint32_t _pad0 = 0;
    };

    // ---- 64 B. Moved verbatim out of Renderer.h:59-64. ----
    struct LightData {
        glm::vec4 position{};
        glm::vec4 direction{};
        glm::vec4 color{};
        glm::vec4 params{};
    };
}
```

`pbrParams.w` was dead padding (`AssetTypes.h:20`); it now carries `normalScale`. `emission`, `color` and `pbrParams.xyz` are unchanged, so `PathTracing.comp:462`, `:467`, `:470` need no edit for those fields.

### 1.5 Final GLSL definitions

New file **`X3/res/shaders/GpuTypes.glsl`**, `#include`d by all three `.comp` files. This deletes the three-way duplication at `PathTracing.comp:33-71`, `PBR.comp:27-63`, `Phong.comp:27-63`.

```glsl
#ifndef X3_GPU_TYPES_GLSL
#define X3_GPU_TYPES_GLSL

// !!! MIRRORED. C++ side: X3/src/Renderer/GpuTypes.h and Project/Assets/BVHAccel.h.
// !!! Any edit here MUST be matched there, and the static_asserts updated.

// std430 - 32 B (CPU: BVHAccel::Node, Project/Assets/BVHAccel.h:13)
struct BVHNode { vec3 min; uint leftChild_Or_FirstTri; vec3 max; uint triCount; };

// std430 - 48 B (CPU: Gpu::TrianglePositions)
struct TrianglePositions { vec4 v0, v1, v2; };

// std430 - 16 B (CPU: Gpu::TriRef)
struct TriRef { uint i0; uint i1; uint i2; uint materialSlot; };

// std430 - 48 B (CPU: Gpu::Vertex)
struct Vertex { vec4 positionU; vec4 normalV; vec4 tangent; };

// std430 - 64 B (CPU: Gpu::Material)
struct Material { vec4 emission; vec4 color; vec4 pbrParams; uvec4 textures; };

// std430 - 32 B (CPU: Gpu::MeshEntityHandle)
struct EntityHandle {
    uint rootTriIdx; uint triCount; uint rootNodeIdx; uint nodeCount;
    uint transformIdx; uint materialBase; uint materialSlotCount; uint _pad0;
};

// std430 - 64 B (CPU: Gpu::LightData)
struct LightData { vec4 position; vec4 direction; vec4 color; vec4 params; };

#define X3_INVALID_TEXTURE 0xFFFFFFFFu
#endif
```

Keep the member names `min` / `max` on `BVHNode` (as at `PathTracing.comp:34,36`) — renaming them is churn for nothing.

### 1.6 AssetPool and metadata changes

`X3/src/Project/Assets/AssetManager.h:45-48` becomes:

```cpp
std::vector<Gpu::TrianglePositions> TriPositionBuffer;  // was MeshBuffer
std::vector<Gpu::TriRef>            TriRefBuffer;       // NEW
std::vector<Gpu::Vertex>            VertexBuffer;       // NEW
std::vector<uint32_t>               BvhPrimIndexBuffer; // was IndexBuffer  (RENAME — see §2.3)
std::vector<BVHAccel::Node>         NodeBuffer;
std::unordered_map<LR_GUID, TexturePixels> TexturePixels; // replaces flat TextureBuffer, see §6.3
```

`AssetPool::AssetType` (`AssetManager.h:62-69`) gains `TriRefBuffer`, `VertexBuffer`; `MeshBuffer` → `TriPositionBuffer`; `IndexBuffer` → `BvhPrimIndexBuffer`.

`MeshMetadata` (`AssetTypes.h:27-33`) becomes:

```cpp
struct SubmeshInfo {
    uint32_t firstTriIdx = 0;    // MESH-LOCAL, relative to MeshMetadata::firstTriIdx
    uint32_t triCount    = 0;
    uint32_t materialSlot = 0;   // dense 0..slotCount-1
    std::string name;            // aiMesh::mName, for editor UI
};

struct MeshMetadata : public Metadata {
    uint32_t firstTriIdx    = 0;
    uint32_t TriCount       = 0;
    uint32_t firstNodeIdx   = 0;
    uint32_t nodeCount      = 0;
    uint32_t firstVertexIdx = 0;   // NEW
    uint32_t vertexCount    = 0;   // NEW
    uint32_t materialSlotCount = 0;// NEW
    std::vector<SubmeshInfo> submeshes;        // NEW, CPU-only
    std::vector<Gpu::Material> importedMaterials; // NEW, one per slot, from aiMaterial
    ~MeshMetadata() override = default;
};
```

`TriRef::i0/i1/i2` are **global** indices into `AssetPool::VertexBuffer` (the importer adds `firstVertexIdx` when writing). This costs one add at import and saves one add per hit in the shader. Document it in a comment on `TriRef`.

---

## 2. BVH impact

### 2.1 What changes: essentially only a type name

`BVHAccel` consumes positions and nothing else. Every read is `.v0/.v1/.v2`:

- `PrecomputeCentroids` — `BVHAccel.h:65-73`
- `UpdateAABB` — `BVHAccel.cpp:42-53`
- `FindBestSplitPlane` binning — `BVHAccel.cpp:78-83`
- `SubDivide` partition (centroids only) — `BVHAccel.cpp:117-165`

Since `Gpu::TrianglePositions` is byte- and member-identical to today's `Triangle`, the required edits are:

1. `BVHAccel.h:50` — `BVHAccel(const std::vector<Gpu::TrianglePositions>& triPositions, uint32_t firstTriIdx, uint32_t triCount)`
2. `BVHAccel.h:82` — `const std::vector<Gpu::TrianglePositions>& m_TriBuff;`
3. `BVHAccel.h:69`, `BVHAccel.cpp:46`, `BVHAccel.cpp:78` — `const Gpu::TrianglePositions& t = …`
4. `BVHAccel.cpp:7-8` — ctor signature

**Nothing else in BVHAccel changes.** `Node` layout is unchanged (32 B). Split heuristic, binning, SAH, partition, node-count math: all unchanged. Build time and traversal cost are unchanged.

### 2.2 Traversal needs only positions — confirmed

`TraverseBVH` (`PathTracing.comp:245-311`) reads exactly: `NodeBuffer[...].min/.max/.leftChild_Or_FirstTri/.triCount`, `IndexBuffer[...]`, and `MeshBuffer[...]` passed to `IntersectTri`. `IntersectTri` (`PathTracing.comp:224-243`) reads only `tri.v0/v1/v2`. There is no attribute access anywhere in the traversal path, and there must not be one after this phase.

### 2.3 Where the attribute fetch happens — exactly

Three distinct points. Do not conflate them.

**(a) Inside `IntersectTri` (`PathTracing.comp:224-243`) — record, do not fetch.**
Add three scalar writes on the accepted-hit path only (after line 239's rejection tests):
```
r.t          = t;
r.bary       = vec2(u, v);       // NEW
r.triIdxLocal = meshLocalTriIdx; // NEW, passed in as a new parameter
r.normalGeom = normalize(Ng);    // was r.normal
```
The signature becomes `bool IntersectTri(inout Ray r, const TrianglePositions tri, uint meshLocalTriIdx)`.

**Barycentric convention (derived from this exact code — do not guess):** with `E1 = v1-v0`, `E2 = v2-v0`, `AO = origin-v0`, `DAO = cross(AO,dir)`, `det = -dot(dir,Ng)`, the code's `u = dot(E2,DAO)*invdet` is the weight of **v1**, and `v = -dot(E1,DAO)*invdet` is the weight of **v2**. Therefore:
```
A_interp = A0*(1.0 - u - v) + A1*u + A2*v
```

**(b) Inside the leaf loop (`PathTracing.comp:258-265`) — pass the index, keep the material write.**
`triIndex` at line 259 is the **mesh-local** triangle index (the permutation value). Pass it to `IntersectTri`. Replace line 263's `ray.materialIdx = entityHandle.materialIdx;` with nothing — the material now comes from `TriRef.materialSlot` at resolve time (§5).

**(c) In `CheckRayCollision` (`PathTracing.comp:313-338`) — record the winner; resolve once, after the loop.**
Inside the `if (rayLocal.t < ray.t)` block, carry forward `rayLocal.bary`, `rayLocal.triIdxLocal`, and the loop index `i` (entity index). Transform and normalise the *geometric* normal here as today (line 330-334).

Then **after the entity loop closes**, call a new `ResolveHitAttributes(inout Ray ray)` exactly once. This is the only place in the whole shader that touches `TriRefBuffer` or `VertexBuffer`. Cost: one 16-byte load + three 48-byte loads + ~30 ALU, **per ray**, not per triangle test.

Do not fetch attributes inside `TraverseBVH`. Do not fetch them inside the entity loop.

---

## 3. The struct-sync problem — interim measure

Phase 3 fixes this with Slang reflection (`ENGINE_PLAN.md:179-181`). Until then, three cheap measures. All three, not a subset.

### 3.1 Collapse the GLSL side from three copies to one

Create `X3/res/shaders/GpuTypes.glsl` (§1.5). In all three `.comp` files, delete the struct block (`PathTracing.comp:31-71`, `PBR.comp:25-63`, `Phong.comp:25-63`) and replace with:
```glsl
#extension GL_GOOGLE_include_directive : require
#include "GpuTypes.glsl"
```
placed immediately after the `#version 460 core` line.

CMake changes at `X3/CMakeLists.txt:83-98`:
- add `-I ${CMAKE_CURRENT_SOURCE_DIR}/res/shaders` to the `glslc` command at line 93;
- add the include to `DEPENDS` at line 94 so edits retrigger:
  `DEPENDS ${GLSL} ${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/GpuTypes.glsl`
- the `file(GLOB_RECURSE …)` at lines 84-87 globs `*.comp/*.vert/*.frag` only, so `GpuTypes.glsl` will not be compiled standalone. Correct as-is; do not add `*.glsl`.

This is the highest-value part of §3: three hand-maintained copies is where the drift actually happens.

### 3.2 Collapse the C++ side into one file

Create `X3/src/Renderer/GpuTypes.h` (§1.4). Then:
- Delete `struct Triangle` (`AssetTypes.h:12-14`) and `struct Material` (`AssetTypes.h:17-21`). Add `#include "Renderer/GpuTypes.h"` to `AssetTypes.h`.
- Delete `struct MeshEntityHandle` (`Renderer.h:42-56`) and `struct LightData` (`Renderer.h:59-64`); `#include "Renderer/GpuTypes.h"` and use `Gpu::MeshEntityHandle` / `Gpu::LightData` in `ParsedScene` (`Renderer.h:66-79`). Drop the `struct Material;` forward declaration at `Renderer.h:19`.
- `BVHAccel::Node` stays where it is (`BVHAccel.h:13-20`) — it is algorithm-owned, and moving it would drag `BVHAccel.h` into the renderer's include graph.

Note: these two structs are currently **private nested types of `class Renderer`** (`Renderer.h:27`), so `static_assert`s on them cannot be written outside the class. Moving them out is what makes §3.3 possible.

### 3.3 The exact `static_assert`s

**At the bottom of `X3/src/Renderer/GpuTypes.h`, inside `namespace X3::Gpu`:**

```cpp
// glm must be tightly packed for any of this to hold.
static_assert(sizeof(glm::vec4)  == 16 && alignof(glm::vec4)  == 16);
static_assert(sizeof(glm::uvec4) == 16 && alignof(glm::uvec4) == 16);
static_assert(sizeof(glm::vec3)  == 12);
static_assert(sizeof(glm::mat4)  == 64);

// offsetof is only well-defined on standard-layout types.
static_assert(std::is_standard_layout_v<TrianglePositions>);
static_assert(std::is_standard_layout_v<TriRef>);
static_assert(std::is_standard_layout_v<Vertex>);
static_assert(std::is_standard_layout_v<Material>);
static_assert(std::is_standard_layout_v<MeshEntityHandle>);
static_assert(std::is_standard_layout_v<LightData>);

// --- TrianglePositions : std430 48 B ---
static_assert(sizeof(TrianglePositions) == 48);
static_assert(offsetof(TrianglePositions, v0) ==  0);
static_assert(offsetof(TrianglePositions, v1) == 16);
static_assert(offsetof(TrianglePositions, v2) == 32);

// --- TriRef : std430 16 B ---
static_assert(sizeof(TriRef) == 16);
static_assert(offsetof(TriRef, i0)           ==  0);
static_assert(offsetof(TriRef, i1)           ==  4);
static_assert(offsetof(TriRef, i2)           ==  8);
static_assert(offsetof(TriRef, materialSlot) == 12);

// --- Vertex : std430 48 B ---
static_assert(sizeof(Vertex) == 48);
static_assert(offsetof(Vertex, positionU) ==  0);
static_assert(offsetof(Vertex, normalV)   == 16);
static_assert(offsetof(Vertex, tangent)   == 32);

// --- Material : std430 64 B ---
static_assert(sizeof(Material) == 64);
static_assert(offsetof(Material, emission)  ==  0);
static_assert(offsetof(Material, color)     == 16);
static_assert(offsetof(Material, pbrParams) == 32);
static_assert(offsetof(Material, textures)  == 48);

// --- MeshEntityHandle : std430 32 B ---
static_assert(sizeof(MeshEntityHandle) == 32);
static_assert(offsetof(MeshEntityHandle, firstTriIdx)       ==  0);
static_assert(offsetof(MeshEntityHandle, triCount)          ==  4);
static_assert(offsetof(MeshEntityHandle, firstNodeIdx)      ==  8);
static_assert(offsetof(MeshEntityHandle, nodeCount)         == 12);
static_assert(offsetof(MeshEntityHandle, transformIdx)      == 16);
static_assert(offsetof(MeshEntityHandle, materialBase)      == 20);
static_assert(offsetof(MeshEntityHandle, materialSlotCount) == 24);

// --- LightData : std430 64 B ---
static_assert(sizeof(LightData) == 64);
static_assert(offsetof(LightData, position)  ==  0);
static_assert(offsetof(LightData, direction) == 16);
static_assert(offsetof(LightData, color)     == 32);
static_assert(offsetof(LightData, params)    == 48);
```

**At the bottom of `X3/src/Project/Assets/BVHAccel.h`, after the closing `};` of `class BVHAccel` (line 91) and still inside `namespace X3`:**

```cpp
static_assert(std::is_standard_layout_v<BVHAccel::Node>);
static_assert(sizeof(BVHAccel::Node) == 32);
static_assert(offsetof(BVHAccel::Node, min)                    ==  0);
static_assert(offsetof(BVHAccel::Node, leftChild_Or_FirstTri)  == 12);
static_assert(offsetof(BVHAccel::Node, max)                    == 16);
static_assert(offsetof(BVHAccel::Node, triCount)               == 28);
```

These catch ABI/padding surprises on the C++ side. They cannot verify the GLSL side — §3.1 is what reduces that risk, and Phase 3 is what eliminates it. Say so in a comment above the block.

---

## 4. Assimp import changes

### 4.1 Preset: keep it, adjust nothing mandatory

`aiProcessPreset_TargetRealtime_MaxQuality` (`AssetManager.cpp:190`) already supplies everything needed — verified at `postprocess.h:666-701`:
`CalcTangentSpace | GenSmoothNormals | JoinIdenticalVertices | ImproveCacheLocality | LimitBoneWeights | RemoveRedundantMaterials | SplitLargeMeshes | Triangulate | GenUVCoords | SortByPType | FindDegenerates | FindInvalidData | FindInstances | ValidateDataStructure | OptimizeMeshes`.

Two facts the implementer must internalise rather than assume:

- **`aiProcess_CalcTangentSpace` requires UV0 and normals.** `CalcTangentsProcess.cpp:121-127` returns `false` and logs *"Failed to compute tangents; need UV data in channel"* if `mNormals == nullptr` or `mTextureCoords[0] == nullptr`. `mTangents` will then be `nullptr`. **Do not assume it is non-null.**
- **`aiProcess_GenUVCoords` does not invent UVs.** It only converts non-UV mapping modes (spherical/cylindrical/planar, declared in the material) into a UV channel. A mesh authored with no texture coordinates comes out with none.

**Do not add `aiProcess_PreTransformVertices`.** It would incidentally fix the node-transform bug but it destroys the node graph and merges meshes by material, which fights the per-submesh material design in §5. Fix the transform bug directly (§4.3).

Optional, only if profiling shows submesh explosion: `importer.SetPropertyInteger(AI_CONFIG_PP_SLM_TRIANGLE_LIMIT, …)` to tune `SplitLargeMeshes`. Leave at default in this phase.

### 4.2 Exact aiMesh members to read

Per `aiMesh` (`X3/libs/assimp/include/assimp/mesh.h`):

| Member | Guard | Fallback |
|---|---|---|
| `mPrimitiveTypes` | `if ((m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) continue;` | skip mesh entirely (`SortByPType` has already split points/lines out) |
| `mVertices[v]` | always non-null | — |
| `mNormals[v]` | `m->HasNormals()` (mesh.h:559) | if null, compute face normals on CPU and average by position; log warn |
| `mTextureCoords[0][v].x/.y` | `m->HasTextureCoords(0)` (mesh.h:588) | `uv = vec2(0,0)`; log warn naming `m->mName.C_Str()` |
| `mNumUVComponents[0]` | — | use `.x`,`.y` only; ignore a 3rd component |
| `mTangents[v]`, `mBitangents[v]` | `m->HasTangentsAndBitangents()` (mesh.h:568) | see §4.4 |
| `mFaces[f].mNumIndices == 3` | already checked at `AssetManager.cpp:218` | keep the `continue` |
| `mMaterialIndex` | always valid; index `scene->mMaterials[...]` | — |
| `mName` | — | store into `SubmeshInfo::name` |

Handedness: `w = (dot(cross(N, T), B) < 0.0f) ? -1.0f : 1.0f`, using assimp's `mBitangents` as `B`. `mBitangents` are then discarded.

### 4.3 The rewritten `LoadMesh`

Replace `AssetManager.cpp:196-233` wholesale. Ordering matters:

1. **Recursive node walk.** New private helper:
   `void CollectMeshInstances(const aiScene* scene, const aiNode* node, const glm::mat4& parent, std::vector<std::pair<uint32_t, glm::mat4>>& out);`
   Accumulate `world = parent * ToGlm(node->mTransformation)` (assimp matrices are row-major; transpose on conversion). Emit `(node->mMeshes[k], world)` for every mesh index on every node. A mesh referenced by two nodes is emitted twice — that is correct, it is two instances.
   Seed with `scene->mRootNode` and identity. **This fixes the bug at `AssetManager.cpp:212`.**
2. **Two passes for sizing.** First pass over the instance list sums `mNumVertices` and triangle-count (faces with exactly 3 indices) to `reserve()` all four buffers up front.
3. **Material slot table.** Build a dense map `aiMaterialIndex -> slot` in first-encounter order across instances. Populate `MeshMetadata::importedMaterials[slot]` from each `aiMaterial` (§4.5). Set `materialSlotCount`.
4. **Per instance, append vertices.** For each source vertex: transform position by `world`; transform normal and tangent by `mat3(transpose(inverse(world)))` for the normal and `mat3(world)` for the tangent, then re-normalise both. Record `instanceFirstVertex = VertexBuffer.size()` before appending.
5. **Per instance, append triangles.** For each triangulated face emit:
   - `TriRefBuffer.push_back({ instanceFirstVertex + idx[0], + idx[1], + idx[2], slot })`
   - `TriPositionBuffer.push_back({ vec4(P[idx[0]],0), vec4(P[idx[1]],0), vec4(P[idx[2]],0) })` — the de-referenced world-space positions written in step 4.
   `TriPositionBuffer` and `TriRefBuffer` must be appended **in lockstep** and end the same length. Assert it.
6. **Submesh ranges.** After each instance, push a `SubmeshInfo{ localFirstTri, localTriCount, slot, name }` with mesh-local `firstTriIdx`.
7. **Tangent fallback** for instances where `mTangents == nullptr` but UVs exist — call the shared CPU helper (§4.4).
8. **Build the BVH** exactly as today (`AssetManager.cpp:232-233`), now over `TriPositionBuffer`.
9. `MarkUpdated` for `TriPositionBuffer`, `TriRefBuffer`, `VertexBuffer`, `NodeBuffer`, `BvhPrimIndexBuffer`, `Metadata`.

### 4.4 Meshes lacking UVs / tangents

New shared CPU helper — used by both the importer fallback and the primitive generators (§4.6). Put it in a new `X3/src/Project/Assets/MeshUtils.h/.cpp`:

```cpp
namespace X3 {
    // Per-triangle tangents from UV derivatives, accumulated per vertex, then
    // Gram-Schmidt orthogonalised against the vertex normal. Sets tangent.w
    // to the handedness sign, or 0.0 where the UV parameterisation is degenerate.
    // vertices[i].normalV.xyz must already be valid.
    void ComputeTangents(std::vector<Gpu::Vertex>& vertices,
                         const std::vector<Gpu::TriRef>& tris,
                         uint32_t firstTri, uint32_t triCount);
}
```

Policy matrix:

| assimp gave | Action |
|---|---|
| normals + UVs + tangents | use directly |
| normals + UVs, no tangents | `ComputeTangents()` |
| normals, **no UVs** | `uv = (0,0)`, `tangent = vec4(0)` → `w == 0` sentinel. Log `LOG_ENGINE_WARN` once per submesh naming the mesh. Normal mapping is skipped in-shader. |
| no normals (shouldn't happen under the preset) | compute face normals, average by position, then as above |

`tangent.w == 0.0` is the single sentinel the shader tests. Do not use a zero-length tangent vector as the test — floating point.

### 4.5 Material import from `aiMaterial`

New private helper `Gpu::Material AssetManager::ImportMaterial(const aiScene*, const aiMaterial*, const std::filesystem::path& modelDir, LR_GUID meshGuid);`

Read, in this order, falling back left to right:
- base colour: `AI_MATKEY_BASE_COLOR` (`material.h:1022`) → `AI_MATKEY_COLOR_DIFFUSE` (`material.h:996`) → `vec4(1)`
- metallic: `AI_MATKEY_METALLIC_FACTOR` (`material.h:1026`) → `0.0`
- roughness: `AI_MATKEY_ROUGHNESS_FACTOR` (`material.h:1030`) → `0.5`
- emissive: `AI_MATKEY_COLOR_EMISSIVE` → `vec3(0)`; strength from `AI_MATKEY_EMISSIVE_INTENSITY` (`material.h:1090`) → `1.0`

Textures via `mat->GetTexture(type, 0, &path)` for:
`aiTextureType_BASE_COLOR` (12) → fall back to `aiTextureType_DIFFUSE` (1);
`aiTextureType_NORMALS` (6);
`aiTextureType_METALNESS` (15) → fall back to `aiTextureType_DIFFUSE_ROUGHNESS` (16) — in glTF these resolve to the same ORM image;
`aiTextureType_EMISSIVE` (4).
(Enum values at `material.h:225,240,288,291-293`.)

**Embedded textures (`.glb` — both sample models).** If the returned path starts with `'*'`, or `scene->GetEmbeddedTexture(path.C_Str())` (`scene.h:441`) returns non-null:
- `aiTexture::mHeight == 0` → `mPcData` is a compressed blob of `mWidth` bytes → `stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(tex->pcData), tex->mWidth, …, 4)`
- `mHeight != 0` → raw `aiTexel` BGRA8 array, `mWidth * mHeight` texels → swizzle to RGBA.
Otherwise resolve the path relative to `modelDir` and go through the existing `stbi_load` path.

Each imported texture is registered in the AssetPool under a derived GUID and its index in the GPU texture table is written into `Gpu::Material::textures` (§6). Deduplicate by (source path | embedded index) so an ORM map shared by five materials uploads once.

### 4.6 Primitive meshes

`CreatePrimitiveMeshes` (`AssetManager.cpp:349-557`) and the `makeTri` lambda (`:351-357`) must produce attributes. Replace `makeTri(vec3,vec3,vec3)` with an emit-vertex helper taking `(position, normal, uv)`; run `ComputeTangents()` (§4.4) over the finished vertex/index arrays for each primitive.

Analytic normals and UVs:
- **Cube** (`:362-386`): per-face constant normal; UV = the two in-plane axes remapped to `[0,1]`. Vertices must **not** be shared across faces (normals differ).
- **Sphere** (`:391-416`): `N = normalize(p)`; `uv = (phi/2π, theta/π)`.
- **Plane** (`:421-429`): `N = (0,1,0)`; `uv = (x+0.5, z+0.5)`.
- **Cylinder** (`:434-461`): side `N = normalize(x,0,z)`, `uv = (angle/2π, (y+h)/2h)`; caps `N = ±Y`, planar UV.
- **Capsule** (`:466-526`): body as cylinder; hemispheres as sphere about the cap centre.
- **Cone** (`:531-554`): side normal `normalize(vec3(cos a, r/h, sin a))` — **not** the radial normal; cap `N = -Y`.

`CreatePrimitiveMesh` (`AssetManager.h:161`, `AssetManager.cpp:320`) changes signature to take vertices + triangle refs:
```cpp
void CreatePrimitiveMesh(LR_GUID guid,
                         const std::vector<Gpu::Vertex>& vertices,
                         const std::vector<Gpu::TriRef>& tris,
                         const char* name);
```
It de-references positions into `TriPositionBuffer` itself, so the generators never build that array by hand.

---

## 5. Per-submesh materials

### 5.1 Design: one BVH per asset, material slot per triangle

**Do not split the BVH per submesh.** `CheckRayCollision` (`PathTracing.comp:315`) already loops over every entity linearly; multiplying entities by submesh count multiplies that cost. Keep one BVH per mesh asset spanning all submeshes — the SAH build handles the concatenation fine.

The material index at a hit becomes:
```
materialIdx = entity.materialBase + TriRefBuffer[entity.rootTriIdx + triIdxLocal].materialSlot
```
`materialSlot` rides free in `TriRef.w` — the fetch is already happening to get the vertex indices, so **per-submesh materials cost zero extra loads.**

### 5.2 Entity lookup table changes

`Gpu::MeshEntityHandle` (§1.4) replaces the single `materialIdx` (`Renderer.h:48`) with `materialBase` + `materialSlotCount`. 24 B → 32 B. `MaterialBuffer` is no longer one entry per entity; it is a **flattened, variable-stride** array: entity *i*'s materials occupy `[materialBase, materialBase + materialSlotCount)`.

### 5.3 `Renderer::Parse` changes (`Renderer.cpp:154-191`)

Replace the per-entity material block (`:172-181`) and the `emplace_back` (`:183-190`) with:

```
uint32_t slotCount = max(1u, metadata->materialSlotCount);
uint32_t base = pScene->MaterialBuffer.size();
for (slot in 0..slotCount):
    if entity has MaterialComponent and slot < component.slots.size():
        append MaterialComponent.slots[slot] converted to Gpu::Material
    else if slot < metadata->importedMaterials.size():
        append metadata->importedMaterials[slot]      // from the model file
    else:
        append Gpu::Material{}                         // default
pScene->MeshEntityLookupTable.emplace_back(
    metadata->firstTriIdx, metadata->TriCount,
    metadata->firstNodeIdx, metadata->nodeCount,
    transformIdx, base, slotCount, 0);
```

The `MaterialBuffer.reserve(renderableView.size_hint())` at `Renderer.cpp:158` becomes an under-estimate — that is fine, but change it to `size_hint() * 2` to reduce reallocation.

The MaterialSSBO upload (`Renderer.cpp:277-288`) and its `m_Cache.materialSize` guard already key on element count, so they work unchanged with the variable stride.

### 5.4 `MaterialComponent` becomes a slot vector

`Components.h:62-71`. Extract the current fields into a plain `MaterialDesc`, add texture GUIDs, and make the component a vector:

```cpp
struct MaterialDesc {
    glm::vec4 emission = {0,0,0,0};
    glm::vec4 color    = {1,1,1,1};
    float metallic  = 0.0f;
    float roughness = 0.5f;
    float ao        = 1.0f;
    float normalScale = 1.0f;              // was _padding
    LR_GUID baseColorTex = LR_GUID::INVALID;
    LR_GUID normalTex    = LR_GUID::INVALID;
    LR_GUID metalRoughTex= LR_GUID::INVALID;
    LR_GUID emissiveTex  = LR_GUID::INVALID;
};

struct MaterialComponent {
    std::vector<MaterialDesc> slots{ MaterialDesc{} };  // never empty
};
```

Touch points, all of which currently assume a flat struct:
- `Scene.cpp:42-43` (duplicate) and `Scene.cpp:111-112` (copy) — value-copy of a vector, works as written.
- `Scene.cpp:205-220` serialize / `Scene.cpp:465-473` deserialize — §8.
- `InspectorPanel.cpp:174-175` — draw a collapsible per slot, labelled from `MeshMetadata::SubmeshInfo::name`. Copy/paste (`InspectorPanel.cpp:178-195`) becomes per-slot; `EditorState.h:33 copiedMaterial` becomes `MaterialDesc`.
- `SceneHierarchyPanel.cpp:53` `GetOrAddComponent<MaterialComponent>()` — the default ctor gives one slot; then resize to `MeshMetadata::materialSlotCount` when a mesh is assigned.
- `AssetsPanel.cpp:435-441` — add a submesh/slot count row next to `Triangle Count`.

**Slot-count reconciliation:** when `MeshComponent::guid` changes, resize `MaterialComponent::slots` to the new `materialSlotCount`, preserving overlapping entries and seeding new ones from `MeshMetadata::importedMaterials`. Do this in the inspector's mesh-assignment path, not in `Renderer::Parse` (which takes `const Scene*`).

---

## 6. Texture management

### 6.1 Binding model: fixed-size array of combined image samplers. Recommended.

Options considered and rejected:
- **`sampler2DArray`** — requires every layer to share dimensions and format. Real material textures are heterogeneous. Would force a global resize. No.
- **Full bindless** (`VARIABLE_DESCRIPTOR_COUNT` + `UPDATE_AFTER_BIND` + `PARTIALLY_BOUND`) — more feature surface than needed, and the weakest part of MoltenVK's descriptor-indexing support. `ENGINE_PLAN.md:379` says macOS gets tested continuously; do not take the risk for a win this phase does not need.

**Chosen:** `layout(SET(0) binding = 2) uniform sampler2D u_MaterialTextures[MAX_MATERIAL_TEXTURES];` with `MAX_MATERIAL_TEXTURES = 128`, **every slot always written** (unused slots point at a shared 1×1 dummy). Indexed with `nonuniformEXT()` because the material index is divergent across lanes in a path tracer.

**Why this does not need redoing in Phase 7:** Forward+ wants exactly the same thing — one bound texture table, index sourced from the material struct. Phase 9's BC7 changes the *format* of the images, not the binding model. The only thing that would ever force a change is exceeding 128 textures, at which point bump the constant (the pool sizing in §6.2 is the only other place that cares).

### 6.2 Vulkan work required — five concrete blockers

1. **Enable the device features.** `VulkanContext.cpp:75-78` (`pickPhysicalDevice`, the `vkb::PhysicalDeviceSelector`) currently requests *nothing*. Add:
   - `VkPhysicalDeviceFeatures::shaderSampledImageArrayDynamicIndexing = VK_TRUE` via `selector.set_required_features(...)`
   - `VkPhysicalDeviceVulkan12Features::descriptorIndexing = VK_TRUE`
   - `VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing = VK_TRUE`
     via `selector.set_required_features_12(...)`
   Not needed: `runtimeDescriptorArray`, `descriptorBindingVariableDescriptorCount`, `descriptorBindingPartiallyBound`, any `updateAfterBind`. Keeping the feature set this small is the whole point of the fixed-size choice.
   Instance already requires 1.2 (`VulkanContext.cpp:52`), so no `VK_EXT_descriptor_indexing` extension string is needed.
2. **Fix `descriptorCount`.** `VulkanComputeShader.cpp:206` hardcodes `write.descriptorCount = 1` with the comment *"Assuming count is 1 for now"*, while `DescriptorBinding::count` (`VulkanComputeShader.h:15`) is already carried and ignored. Use `binding.count`, and for arrays point `pImageInfo` at a contiguous `std::vector<VkDescriptorImageInfo>` of that length.
3. **Array-aware registry.** `VulkanContext.h:44-56` maps one binding to one resource. Add:
   ```cpp
   void registerSampledImageArray(uint32_t binding, std::vector<BoundSampledImage> images);
   const std::unordered_map<uint32_t, std::vector<BoundSampledImage>>& getBoundSampledImageArrays() const;
   ```
   and a matching `m_ImageInfoArrays` in `VulkanComputeShader` (mirroring `m_ImageInfos`). Pointers into `unordered_map` mapped values are stable, matching the existing pattern at `VulkanComputeShader.cpp:222`.
4. **Add the descriptor table entry.** `VulkanComputeShader.cpp:19-22`, set 0:
   `{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_MATERIAL_TEXTURES, VK_SHADER_STAGE_COMPUTE_BIT}  // u_MaterialTextures[]`
5. **Grow the descriptor pool.** `VulkanContext.cpp:464-465, 480`: three shaders × `MAX_FRAMES_IN_FLIGHT` sets (after the Phase 1c fix) × 128 = 768, plus the skybox and ImGui's per-viewport-texture allocations. Raise `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` to **4096** and `maxSets` to **256**. Under-sizing here fails as a `vkAllocateDescriptorSets` error at first dispatch, which is at least loud.

macOS check-item: verify `shaderSampledImageArrayNonUniformIndexing` reports supported under MoltenVK on the target machine before writing shader code against it; older MoltenVK requires Metal argument buffers to be enabled for it.

### 6.3 CPU-side storage: kill the flat buffer

`AssetPool::TextureBuffer` (`AssetManager.h:48`) is a single growing `std::vector<unsigned char>` with `texStartIdx` offsets (`AssetManager.cpp:270-279`). A single 4K RGBA8 albedo is 64 MB; a handful of them makes every subsequent `insert()` copy hundreds of megabytes. Replace with:

```cpp
struct TexturePixels {
    std::vector<unsigned char> data;
    int32_t width = 0, height = 0, channels = 0;
    bool    isSRGB = false;
};
std::unordered_map<LR_GUID, TexturePixels> TexturePixels;   // in AssetPool
```

Delete `TextureMetadata::texStartIdx` (`AssetTypes.h:36`). The only reader is `Renderer.cpp:243`.

### 6.4 Format and colour space — get this right or normal maps are silently wrong

`VulkanTexture2D` hardcodes `VK_FORMAT_R8G8B8A8_SRGB` at `VulkanTexture2D.cpp:86` and `:142`. Sampling a normal map or an ORM map through an sRGB view applies an EOTF to data that is not colour. Add a format parameter:

```cpp
enum class TextureColorSpace { SRGB, LINEAR };
// ITexture2D.h:10
static std::shared_ptr<ITexture2D> Create(const unsigned char* data, int width, int height,
                                          TextureColorSpace colorSpace, bool generateMips);
```
- baseColor, emissive → `VK_FORMAT_R8G8B8A8_SRGB`
- normal, metalRough, AO → `VK_FORMAT_R8G8B8A8_UNORM`

Drop the `textureUnit` parameter from `Create` — array membership is decided by the texture table, not by a GL-style unit. This also removes `ITexture2D::ChangeTextureUnit` (`ITexture2D.h:12`), consistent with Phase 1b's table (`ENGINE_PLAN.md:117`).

### 6.5 UV origin — pick one convention, change both ends together

`AssetManager.cpp:259` calls `stbi_set_flip_vertically_on_load(1)` with the comment *"OpenGL-style orientation"*. glTF/assimp UVs use a top-left origin (V down), which is Vulkan's convention. Flipping the image **and** using assimp UVs unchanged gives vertically mirrored textures on every model.

**Decision: stop flipping. Remove the call at `AssetManager.cpp:259`.** Use assimp UVs verbatim.

This inverts the skybox, which is currently compensated by the flip. Fix it at the same commit: `PathTracing.comp:207` (and the identical lines in `PBR.comp` and `Phong.comp`)
```glsl
float v = 0.5 - asin(ray.dir.y) * INV_PI;   // was 0.5 + asin(...)
```
Verify visually against `SampleSkyboxes/*.hdr` before moving on. This is the single most likely thing to eat a day if done piecemeal.

Unrelated but worth one line in the commit message: `.hdr` skyboxes go through `stbi_load` (`AssetManager.cpp:260`), an 8-bit path, so HDR range is being crushed today. Out of scope for Phase 2; note it for Phase 11.

### 6.6 Mip generation

Required, not optional. Reasons: Phase 7 needs mips regardless; adding them later means reopening the upload path and the sampler; and without them a path tracer aliases badly on minified surfaces.

In `VulkanTexture2D::createImage` (`VulkanTexture2D.cpp:45-132`):
1. `mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(w, h)))) + 1` (replaces `:84`).
2. `usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT` (`:89`).
3. After the level-0 copy (`:122`), a `vkCmdBlitImage` chain: for each level *i*, barrier level *i-1* `TRANSFER_DST → TRANSFER_SRC`, blit with `VK_FILTER_LINEAR`, halving extents with `max(1, dim/2)`.
4. Final barrier transitions **all** levels to `SHADER_READ_ONLY_OPTIMAL`. The existing `transitionImageLayout` (`:181-214`) hardcodes `levelCount = 1` (`:191`) and supports only two transition pairs (`:198-207`) — it must gain level-range parameters and a `TRANSFER_DST → TRANSFER_SRC` case, and its `destinationStage` must become `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` (it is currently fragment-only at `:207`, which is wrong for a compute-only renderer today).
5. **Guard:** `vkGetPhysicalDeviceFormatProperties(...).optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`. If absent, fall back to `mipLevels = 1`.
6. Image view `subresourceRange.levelCount = mipLevels` (`:145`).

### 6.7 Sampler management

`VulkanTexture2D::createSampler` (`:154-179`) creates one `VkSampler` per texture and sets `maxLod = 0.0f` (`:174`), which disables mips even when they exist.

Replace with **one shared sampler** owned by `VulkanContext`, created once:
`magFilter/minFilter = LINEAR`, `mipmapMode = LINEAR`, `addressMode* = REPEAT`, `minLod = 0`, `maxLod = VK_LOD_CLAMP_NONE`, `anisotropyEnable = VK_FALSE` (enabling it requires the `samplerAnisotropy` feature — defer to Phase 7).
Rationale: 128 material textures would otherwise mean 128 sampler objects; `maxSamplerAllocationCount` is as low as 4000 on some drivers and lower under MoltenVK. A combined-image-sampler descriptor may repeat the same `VkSampler` handle across all array elements.

### 6.8 The texture table

New `X3/src/Renderer/TextureTable.h/.cpp` owned by `Renderer`:
- `uint32_t GetOrCreate(LR_GUID, TextureColorSpace)` → index in `[0, MAX_MATERIAL_TEXTURES)`, uploading from `AssetPool::TexturePixels` on first request.
- Slot 0 is reserved for a shared 1×1 opaque-white dummy, created at `Renderer::Init`.
- `void Bind()` calls `VulkanContext::registerSampledImageArray(2, views)` with all `MAX_MATERIAL_TEXTURES` entries filled, unused → dummy.
- Table is rebuilt when `AssetPool::GetUpdateVersion(AssetType::Textures)` changes, following the pattern already at `Renderer.cpp:306-351`.

`MaterialDesc`'s texture GUIDs are resolved to table indices in `Renderer::Parse`, written into `Gpu::Material::textures`; unresolvable GUIDs become `INVALID_TEXTURE` (`0xFFFFFFFF`) and log a warning without failing the frame.

The skybox stays exactly where it is (set 0 binding 1, `Renderer.cpp:238-249`) — it is not a material texture and does not belong in the table.

---

## 7. Shader changes to `PathTracing.comp`

Apply to `PathTracing.comp` first; `PBR.comp` and `Phong.comp` get the identical traversal/resolve changes (their `IntersectTri` at `:139-142`, leaf loop at `:175-177`, and hit shading at `:463`/`:367` are copy-pasted). Hoist everything shareable into `GpuTypes.glsl` per §3.1 to avoid a fourth divergence.

### 7.1 Preamble

After `#version 460 core` (line 6):
```glsl
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "GpuTypes.glsl"
```

### 7.2 New / changed bindings

```glsl
layout(SET(0) binding = 2) uniform sampler2D u_MaterialTextures[128];      // NEW

layout(std430, SET(2) binding = 5) readonly buffer BvhPrimIndexSSBO {      // RENAMED from IndexBufferSSBO
    uint BvhPrimIndex[];
};
layout(std430, SET(2) binding = 7) readonly buffer TriRefSSBO {            // NEW
    TriRef TriRefBuffer[];
};
layout(std430, SET(2) binding = 8) readonly buffer VertexSSBO {            // NEW
    Vertex VertexBuffer[];
};
```
Bindings 7 and 8 in set 2 are free. The SSBO at binding 3 changes element type to `TrianglePositions` and is renamed `TriPositionBuffer`.

Corresponding C++ additions: two `IShaderStorageBuffer::Create(size, 7|8, STATIC_DRAW)` members on `Renderer` (`Renderer.h:117`), uploaded in the version-gated block alongside `Renderer.cpp:311-351`.

### 7.3 `Ray` gains hit state

`PathTracing.comp:75-81`:
```glsl
struct Ray {
    vec3  origin;   float t;
    vec3  dir;      uint  entityIdx;
    vec3  normalGeom;   // geometric, from cross(E1,E2)
    vec3  normalShade;  // interpolated + normal-mapped, world space
    vec2  bary;         // (u,v) = weights of v1, v2
    uint  triIdxLocal;  // mesh-local triangle index
    uint  materialIdx;
    vec2  uv;
    bool  frontFacing;
};
```

### 7.4 `IntersectTri` — record barycentrics and index

`PathTracing.comp:224-243`. Signature gains `uint meshLocalTriIdx`. Replace lines 240-242 with:
```glsl
r.t           = t;
r.bary        = vec2(u, v);
r.triIdxLocal = meshLocalTriIdx;
r.normalGeom  = normalize(Ng);
return true;
```
Nothing else in the function changes. The `u`, `v`, `t` computed at `:234-236` are exactly what is needed; do not recompute.

### 7.5 Leaf loop

`PathTracing.comp:258-265`:
```glsl
uint triIndex = BvhPrimIndex[entityHandle.rootTriIdx + first + i];
const TrianglePositions tri = TriPositionBuffer[entityHandle.rootTriIdx + triIndex];
if (IntersectTri(ray, tri, triIndex)) {
    g_TriIntersectionCount++;
}
```
Delete `ray.materialIdx = entityHandle.materialIdx;` (line 263).

### 7.6 `CheckRayCollision` — carry the winner, resolve once

`PathTracing.comp:313-338`. Inside the `if (rayLocal.t < ray.t)` block (`:328-336`), additionally carry `ray.bary = rayLocal.bary; ray.triIdxLocal = rayLocal.triIdxLocal; ray.entityIdx = uint(i);` and store the world geometric normal into `ray.normalGeom`.

**Do not `faceforward` blindly at `:334`.** Instead record `ray.frontFacing = dot(ray.normalGeom, ray.dir) < 0.0;` and flip `normalGeom` if false. Shading normals must be flipped consistently with the geometric normal, not independently.

Then, **after** the entity loop closes (after line 337), call `ResolveHit(ray)` once, guarded on `ray.t < INF_T`.

### 7.7 `ResolveHit` — the entire attribute path, in one function

```glsl
Vertex FetchVertex(uint idx) { return VertexBuffer[idx]; }   // sole packing chokepoint (§1.2)

void ResolveHit(inout Ray ray) {
    EntityHandle e = EntityLookupTable[ray.entityIdx];
    mat4 model     = TransformBuffer[e.transformIdx];
    mat3 normalMat = mat3(transpose(inverse(model)));

    TriRef tr = TriRefBuffer[e.rootTriIdx + ray.triIdxLocal];
    ray.materialIdx = e.materialBase + min(tr.materialSlot, e.materialSlotCount - 1u);

    Vertex a = FetchVertex(tr.i0);
    Vertex b = FetchVertex(tr.i1);
    Vertex c = FetchVertex(tr.i2);

    float u = ray.bary.x, v = ray.bary.y, w = 1.0 - u - v;

    ray.uv = vec2(a.positionU.w, a.normalV.w) * w
           + vec2(b.positionU.w, b.normalV.w) * u
           + vec2(c.positionU.w, c.normalV.w) * v;

    vec3 Ns = normalize(normalMat * (a.normalV.xyz * w + b.normalV.xyz * u + c.normalV.xyz * v));
    if (!ray.frontFacing) Ns = -Ns;

    Material mat = MaterialBuffer[ray.materialIdx];

    // --- tangent-space normal mapping ---
    float hand = a.tangent.w;                    // constant across a triangle (see §1.3)
    if (hand != 0.0 && mat.textures.y != X3_INVALID_TEXTURE) {
        vec3 Ti = a.tangent.xyz * w + b.tangent.xyz * u + c.tangent.xyz * v;
        vec3 T  = normalize(mat3(model) * Ti);   // TANGENTS use model, NOT inverse-transpose
        T = normalize(T - Ns * dot(Ns, T));      // Gram-Schmidt against the interpolated normal
        vec3 B = cross(Ns, T) * hand;
        vec3 nts = textureLod(u_MaterialTextures[nonuniformEXT(mat.textures.y)], ray.uv, 0.0).xyz
                 * 2.0 - 1.0;
        nts.xy *= mat.pbrParams.w;               // normalScale
        Ns = normalize(mat3(T, B, Ns) * nts);
    }

    // Never let the shading normal cross the geometric hemisphere.
    if (dot(Ns, ray.normalGeom) < 0.0)
        Ns = normalize(Ns - ray.normalGeom * (dot(Ns, ray.normalGeom) * 1.0001));
    ray.normalShade = Ns;
}
```

**Transform rules, stated because they are the classic silent bug:** normals transform by `mat3(transpose(inverse(model)))` (as already done at `PathTracing.comp:330`); **tangents transform by `mat3(model)`**. Under non-uniform scale, using the same matrix for both produces a subtly skewed TBN that only shows up on scaled objects.

### 7.8 Shading (`PathTracing.comp:452-483`)

```glsl
Material mat = MaterialBuffer[ray.materialIdx];

vec3 albedo = mat.color.rgb;
if (mat.textures.x != X3_INVALID_TEXTURE)
    albedo *= textureLod(u_MaterialTextures[nonuniformEXT(mat.textures.x)], ray.uv, 0.0).rgb;

float metallic  = mat.pbrParams.x;
float roughness = mat.pbrParams.y;
if (mat.textures.z != X3_INVALID_TEXTURE) {
    vec2 mr = textureLod(u_MaterialTextures[nonuniformEXT(mat.textures.z)], ray.uv, 0.0).bg;
    metallic  *= mr.x;   // glTF ORM: B = metallic
    roughness *= mr.y;   //           G = roughness
}

vec3 emitted = mat.emission.rgb * mat.emission.w;
if (mat.textures.w != X3_INVALID_TEXTURE)
    emitted *= textureLod(u_MaterialTextures[nonuniformEXT(mat.textures.w)], ray.uv, 0.0).rgb;
```

Then in the existing body:
- `:463` `brightness_score += emitted * rayColor;`
- `:466` offset along **`ray.normalGeom`**, shade with **`ray.normalShade`**:
  `SampleLights(hitPoint + ray.normalGeom * SURFACE_BIAS, ray.normalShade)`
- `:467` `brightness_score += directLight * albedo * rayColor;`
- `:470` `rayColor *= albedo;`
- `:473` `ray.origin = hitPoint + ray.normalGeom * SURFACE_BIAS;`
- `:477` `ray.dir = normalize(ray.normalShade + RandomDirection(state));`

Offsetting along the geometric normal while sampling along the shading normal is deliberate: shading-normal offsets self-intersect on grazing normal-mapped surfaces.

`metallic` and `roughness` are sampled and available but not yet consumed by `PathTracing.comp`'s pure-diffuse loop. That is correct for this phase — the BSDF work is Phase 6. `PBR.comp` (`:463` onwards) already consumes `pbrParams` and should be wired to the sampled values.

### 7.9 `IsInShadow` (`PathTracing.comp:341-351`)

`CheckRayCollision` now calls `ResolveHit`, which is wasted work for occlusion queries. Add an `bool anyHit` flag on `Ray`, or split out a `CheckRayCollisionOcclusion()` that skips the resolve. Not optional — shadow rays are the majority of `CheckRayCollision` calls (one per light per bounce, `:429-443`).

### 7.10 Optional, explicitly out of the required path

A ray-cone texture LOD (`textureLod(..., lod)` with `lod` derived from triangle UV area vs world area plus cone spread) removes the aliasing that mip-0 sampling causes on minified surfaces. Mips are being generated in §6.6 so the data is there. Do this only after the required path renders correctly, and only if aliasing is visible.

---

## 8. Migration and compatibility

### 8.1 Mesh data: no migration needed. State this clearly.

Verified from source:
- **`.lrmeta` contains only `{Guid, SourcePath}`.** Writer: `AssetManager.cpp:25-29`. Reader: `AssetManager.cpp:50-58`. There is **no cached geometry, no cached BVH, no vertex data** on disk. `.lrmeta` is a GUID sidecar, not an asset cache.
- **`.lrproj` contains only `{bootSceneGuid, RenderSettings}`** (`ProjectManager.h:24-29`). No asset data.
- **Every asset is re-imported from `sourcePath` on every project open** — `LoadAssetPoolFromFolder` (`AssetManager.cpp:128-152`) → `LoadAssetFile` → `LoadMesh`, with a full BVH rebuild each time (`AssetManager.cpp:232`, and `ENGINE_PLAN.md:293` calls this out as a Phase 9 problem).
- `ProjectExporter` copies source asset files and rewrites `.lrmeta::sourcePath` to the copied location (`ProjectExporter.cpp:76-97`), and copies `engine_res` including shaders (`:147`). Exported projects also re-import from source.

**Consequence: the mesh format change is invisible to on-disk project data.** Existing `.lrproj` projects open, re-import from their `.fbx`/`.glb`/`.obj` sources under the new importer, and get normals, UVs and tangents for free. No version bump, no converter, no `.lrmeta` migration. Do not write one.

**The one thing that breaks:** if a project's source model files have moved, they were already broken (`AssetManager.cpp:139-142` warns and skips). Unchanged behaviour.

### 8.2 `.lrscn` scene files: one real format change

`MaterialComponent` becomes a slot vector (§5.4), which changes the serialized shape.

**Add a version key.** `Scene.cpp:146-151` currently writes `SceneGuid`, `SceneName`, `SkyboxGuid`, `SkyboxName`, `Entities` — no version. Add as the first key:
```
<< YAML::Key << "SceneVersion" << YAML::Value << 2
```
On load (near `Scene.cpp:431`), read it with default `1` for absent.

**Writer** (`Scene.cpp:205-220`) emits:
```yaml
MaterialComponent:
  Slots:
    - Emission:   [r,g,b,a]
      Color:      [r,g,b,a]
      Metallic:   0.0
      Roughness:  0.5
      AO:         1.0
      NormalScale: 1.0
      BaseColorTex: 0        # LR_GUID as uint64; 0 == none
      NormalTex:    0
      MetalRoughTex: 0
      EmissiveTex:  0
```

**Reader** (`Scene.cpp:465-473`) branches on the presence of the `Slots` key, not on `SceneVersion` — more robust against hand-edited files:
```
if (mnode["Slots"] && mnode["Slots"].IsSequence())  -> read each slot
else                                                -> legacy: read the flat
                                                       Emission/Color/Metallic/Roughness/AO
                                                       keys into slots[0]; NormalScale = 1.0;
                                                       all texture GUIDs INVALID
```
Keep the legacy branch permanently — it is ten lines and the existing `getScalar`/`getVec4` helpers already default-and-warn.

Result: **old `.lrscn` files load unchanged**, their single material landing in slot 0. On first save they are rewritten in the new shape. One-way; that is fine and should be stated in the commit message.

### 8.3 Texture GUID references

Materials now reference texture assets by GUID. A `.lrscn` referencing a texture whose `.lrmeta` is missing must **not** fail the scene load: resolve to `Gpu::INVALID_TEXTURE`, `LOG_ENGINE_WARN` with the GUID, render with the factor only. Same policy as the existing missing-mesh path (`Renderer.cpp:163-166` silently `continue`s — improve that to a warn while you are there).

### 8.4 Textures imported from inside model files

`ImportMaterial` (§4.5) registers textures found inside `.glb`/`.fbx` into the AssetPool under derived GUIDs. These are **not** independent assets with `.lrmeta` sidecars; they are owned by the model. Do not write `.lrmeta` files for them — `SaveAssetPoolToFolder` (`AssetManager.cpp:92-125`) iterates every metadata entry and would emit sidecars pointing at nonexistent paths, then `LoadAssetPoolFromFolder` would warn on every one. Add a `bool ownedByModel` flag to `MetadataExtension` and skip those in the save loop.

### 8.5 Shader binaries

`X3/res/shaders/*.spv` are currently tracked in git (Phase 0 untracks them, `ENGINE_PLAN.md:82`). If Phase 0 has not landed, the three `.spv` files **must** be rebuilt and re-committed with this change, or a stale binary will silently render with the old descriptor layout against the new buffers. Prefer landing Phase 0's `.gitignore` change first.

---

## 9. Implementation order

Each step should build and, from step 5 on, run. Do not reorder 1-4.

1. **`GpuTypes.h` + `static_asserts`** (§1.4, §3.2, §3.3). Delete `Triangle`/`Material` from `AssetTypes.h`; delete `MeshEntityHandle`/`LightData` from `Renderer.h`. Rename `Triangle` → `Gpu::TrianglePositions` across `BVHAccel.h/.cpp`, `AssetManager.h/.cpp`, `Renderer.cpp`, `PhysicsWorld.cpp:1035,1097`. **Pure rename + move. No behaviour change.** Verify the engine still renders identically.
2. **`GpuTypes.glsl` + CMake `-I`** (§3.1). Delete the struct blocks from all three `.comp` files. **Output must be pixel-identical.** Confirm by diffing a rendered frame.
3. **AssetPool buffer renames** (§1.6): `MeshBuffer` → `TriPositionBuffer`, `IndexBuffer` → `BvhPrimIndexBuffer`, and the matching `AssetType` enumerators, SSBO member names, and GLSL identifiers. Still pixel-identical.
4. **`MeshUtils::ComputeTangents`** (§4.4) — standalone, unused yet.
5. **Importer rewrite** (§4.2, §4.3): node walk, vertex/TriRef/TriPosition emission, submesh table. Do **not** import materials or textures yet. Renderer still ignores the new buffers. Verify BVH build results are unchanged (same node counts) for the sample models, and that the node walk did not move anything.
6. **Primitive regeneration** (§4.6).
7. **New SSBOs + `ResolveHit` + smooth normals** (§7.2-7.7, minus normal mapping). Add `TriRefSSBO`/`VertexSSBO` bindings 7/8, upload from `Renderer`. **This is the first visible change: flat shading becomes smooth.** Stop here and confirm before continuing.
8. **Per-submesh materials** (§5): `MeshEntityHandle` 24→32 B, `materialBase`/`materialSlotCount`, `MaterialComponent` slot vector, `.lrscn` versioned (de)serialization (§8.2), inspector UI. No textures yet.
9. **Vulkan texture-array plumbing** (§6.2): device features, `descriptorCount`, array registry, descriptor table entry, pool sizing. Bind a table full of dummy textures and confirm validation is clean under load before any real texture exists.
10. **Texture upload path** (§6.3, §6.4, §6.6, §6.7): per-asset pixel storage, format/colour-space parameter, mip chain, shared sampler. **Remove `stbi_set_flip_vertically_on_load` and fix the skybox `v` in the same commit** (§6.5) — verify the skybox looks right immediately.
11. **Material and texture import** (§4.5) including embedded `.glb` textures, plus `TextureTable` (§6.8).
12. **Shader sampling + normal mapping** (§7.7 tangent block, §7.8), mirrored into `PBR.comp` and `Phong.comp`. Add the occlusion-ray fast path (§7.9).

**Exit criteria** (from `ENGINE_PLAN.md:159`): `SampleModels/stanford_dragon_pbr.glb` renders smooth-shaded, with its embedded base-colour and normal maps applied, in the path-traced view; validation layers clean; a multi-material model shows distinct materials per submesh.

---

## 10. Explicitly out of scope

- Graphics pipeline / vertex input state (Phase 7). The `Vertex` + `TriRef` buffers are built in a form Forward+ can bind, but nothing binds them.
- A separate tightly-packed `uint32` index buffer for `vkCmdBindIndexBuffer` — trivially extracted from `TriRefBuffer.i0/i1/i2` when Phase 7 needs it. Note it in a comment; do not build it now.
- Texture compression / BC7 (Phase 9), meshoptimizer, serialized BVH.
- Attribute quantization (§1.2 — deliberate deferral to Phase 9).
- Any BSDF work. `metallic`/`roughness` are sampled and plumbed; consuming them properly is Phase 6.
- Lightmap UV channel (UV1). The `Vertex` layout has no room for it; adding it is a Phase 10 concern and will require a second attribute stream or a wider vertex — flag it as known, do not pre-build it.
- Alpha cutout / two-sided material flags. `Material` has room (`color.w`, and unused bits) but nothing consumes them.
- Slang. Do not restructure anything *for* Slang; §3.1 and §3.2 happen to make Phase 3's job easier, which is enough.