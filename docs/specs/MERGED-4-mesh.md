I have everything verified. Here is Part 4.

---

# PART 4 — Phase 2: Mesh format and vertex attributes

**Repo:** `/home/sarah/Coding/Haptixxx/X3`, branch `vulkan-migration`.
**Preconditions:** Parts 1–3 of this merged spec have landed. Concretely that means: OpenGL is gone; `VulkanContext` exposes `beginFrame()/endFrame()/present()` and a `FrameContext`; dynamic rendering is in use (Vulkan 1.3 device, `VK_KHR_dynamic_rendering` also enabled as an extension, no `VkRenderPass`/`VkFramebuffer` anywhere); the resource layer (`VulkanBuffer`, `VulkanRingBuffer`, `VulkanImage`, `VulkanTexture`, `VulkanDescriptorSetLayout`, `VulkanDescriptorSetRing`, `DescriptorWriter`, `VulkanComputePipeline`) exists and `Renderer` owns all GPU resources by value; the descriptor binding tables live as `kSet0[]/kSet1[]/kSet2[]` in `Renderer.cpp`; there is a per-frame staging arena (`ctx.stage()`), a deferred-destruction queue (`ctx.deferDestroy()`), and a sampler cache (`ctx.getSampler(SamplerDesc)`).

**Verification status of this part.** Everything cited below was read in the current working tree. `cmake --preset vulkan-debug && cmake --build build/vulkan-debug -j 14` was run: `0` occurrences of `error:` in the log, `build/vulkan-debug/Debug/X3Editor` (39 MB) produced. Three additional claims were verified by *compiling*, not grepping, and are flagged inline (§3.3, §3.1, §6.2). Do not treat any "unused" claim here as grep-only: each says how it was checked.

---

## 0. Ground truth

Every line number below is from the current working tree.

| Fact | Location |
|---|---|
| `struct Triangle { glm::vec4 v0, v1, v2; }` — positions only, 48 B | `X3/src/Project/Assets/AssetTypes.h:11-14` |
| `struct Material` = 3× `vec4` = 48 B; `pbrParams.w` is dead padding | `X3/src/Project/Assets/AssetTypes.h:17-21` |
| `MeshMetadata` = `firstTriIdx/TriCount/firstNodeIdx/nodeCount` | `X3/src/Project/Assets/AssetTypes.h:27-32` |
| `TextureMetadata::texStartIdx` | `X3/src/Project/Assets/AssetTypes.h:36` |
| AssetPool buffers: `MeshBuffer`, `IndexBuffer`, `NodeBuffer`, `TextureBuffer` | `X3/src/Project/Assets/AssetManager.h:45-48` |
| `AssetPool::AssetType` version-counter enum | `X3/src/Project/Assets/AssetManager.h:62-69` |
| `LoadMesh` | `X3/src/Project/Assets/AssetManager.cpp:181-247` |
| Preset `aiProcessPreset_TargetRealtime_MaxQuality` | `X3/src/Project/Assets/AssetManager.cpp:190` |
| `TriCount` = `sum(mNumFaces)` | `X3/src/Project/Assets/AssetManager.cpp:196-198, 204` |
| Import reads **only** `subMesh->mVertices`; skips non-triangle faces | `X3/src/Project/Assets/AssetManager.cpp:212-227`, skip at `:218` |
| BVH built over the `meshBuffer` slice | `X3/src/Project/Assets/AssetManager.cpp:232-233` |
| `stbi_set_flip_vertically_on_load(1)` — "OpenGL-style orientation" | `X3/src/Project/Assets/AssetManager.cpp:259` |
| Textures appended into one flat `std::vector<unsigned char>` | `X3/src/Project/Assets/AssetManager.cpp:270-279` |
| `CreatePrimitiveMesh(guid, const std::vector<Triangle>&, name)` | `X3/src/Project/Assets/AssetManager.h:161`, `.cpp:320-347` |
| `CreatePrimitiveMeshes` + `makeTri` lambda | `X3/src/Project/Assets/AssetManager.cpp:349-557`, lambda `:351-357` |
| Primitive bodies: cube `:362-386`, sphere `:391-416`, plane `:420-429`, cylinder `:433-461`, capsule `:465-526`, cone `:530-554` | `X3/src/Project/Assets/AssetManager.cpp` |
| `BVHAccel::Node` = 32 B | `X3/src/Project/Assets/BVHAccel.h:13-20` |
| `BVHAccel` ctor takes `const std::vector<Triangle>&` | `X3/src/Project/Assets/BVHAccel.h:50`, `.cpp:7-10` |
| Centroids from `v0+v1+v2` only | `X3/src/Project/Assets/BVHAccel.h:65-73` |
| `m_TriBuff` member | `X3/src/Project/Assets/BVHAccel.h:82` |
| `UpdateAABB` grows over `t.v0/v1/v2` only | `X3/src/Project/Assets/BVHAccel.cpp:42-53`, `Triangle` at `:46` |
| Binning grows over `tri.v0/v1/v2` only | `X3/src/Project/Assets/BVHAccel.cpp:76-84`, `Triangle` at `:78` |
| Prim-index slice aliases the triangle slice: `m_IdxBuff = &indexBuffer[m_FirstTriIdx]` | `X3/src/Project/Assets/BVHAccel.cpp:25` |
| `MeshEntityHandle` = 6× uint = 24 B, **private nested in `Renderer`**, has an explicit ctor | `X3/src/Renderer/Renderer.h:41-56` |
| `LightData` = 4× vec4 = 64 B, private nested | `X3/src/Renderer/Renderer.h:58-64` |
| `ParsedScene` | `X3/src/Renderer/Renderer.h:66-79` |
| `struct Material;` forward declaration | `X3/src/Renderer/Renderer.h:19` |
| One material per entity from `MaterialComponent` | `X3/src/Renderer/Renderer.cpp:171-181` |
| Entity handle emplace (6 args) | `X3/src/Renderer/Renderer.cpp:183-190` |
| `reserve(renderableView.size_hint())` ×3 | `X3/src/Renderer/Renderer.cpp:156-158` |
| Missing-mesh path silently `continue`s | `X3/src/Renderer/Renderer.cpp:163-166` |
| Skybox is the sole reader of `TextureBuffer`/`texStartIdx` | `X3/src/Renderer/Renderer.cpp:237-249`, read at `:243` |
| `MaterialComponent` is one flat struct with `float _padding` | `X3/src/Project/Scene/Components.h:62-71` |
| `.lrscn` material serialize / deserialize | `X3/src/Project/Scene/Scene.cpp:206-220` / `:465-472` |
| `.lrscn` root keys — **no version key** | `X3/src/Project/Scene/Scene.cpp:146-151` |
| `.lrmeta` = `{Guid, SourcePath}` only | `X3/src/Project/Assets/AssetManager.h:87-94`, `.cpp:25-38`, `:50-63` |
| Physics reads `AssetPool::MeshBuffer` as `Triangle` | `X3/src/Physics/PhysicsWorld.cpp:1033-1035`, `:1095-1097` |
| Shader compile rule (globs `*.comp/*.vert/*.frag`, no `-I`, no `--target-env`) | `X3/CMakeLists.txt:87-112`; `glslc` invocation at `:100`, `DEPENDS` at `:101` |
| C++23 project-wide | `CMakeLists.txt:26` (root) |
| Descriptor pool: 1000 of each type, **`maxSets` already 1000**, has `FREE_DESCRIPTOR_SET_BIT` | `X3/src/Platform/Vulkan/VulkanContext.cpp:460-490`; `maxSets` at `:480`, flags at `:479` |
| GLSL struct mirrors (×3 files) | `PathTracing.comp:31-71`, `PBR.comp:25-63`, `Phong.comp:25-63` |
| `vec3 Ng = cross(E1, E2)` — flat shading | `PathTracing.comp:227`, `PBR.comp:141`, `Phong.comp:141` |
| `IntersectTri` | `PathTracing.comp:224-243`, `PBR.comp:139-158`, `Phong.comp:139-158` |
| Leaf loop / per-entity material write | `PathTracing.comp:258-265`, `PBR.comp:172-179`, `Phong.comp:172-179` |
| `CheckRayCollision` | `PathTracing.comp:313-338`, `PBR.comp:225-248`, `Phong.comp:225-248` |
| `IsInShadow` | `PathTracing.comp:341-351`, `PBR.comp:251-260`, `Phong.comp:251-260` |
| Skybox `v` computation | `PathTracing.comp:207`, **`PBR.comp:123`**, **`Phong.comp:123`** |
| Shading uses `mat.color.xyz` and `ray.normal` | `PathTracing.comp:459-477` |
| assimp `aiMesh`: `mPrimitiveTypes` **:645**, `mNumVertices` **:652**, `mTangents` **:707**, `mBitangents` **:718**, `mTextureCoords` **:736**, `mNumUVComponents` **:747**, `mMaterialIndex` **:779**, `mName` **:793**, `HasNormals()` **:916**, `HasTangentsAndBitangents()` **:926**, `HasTextureCoords()` **:943** | `X3/libs/assimp/include/assimp/mesh.h` |
| assimp `CalcTangentSpace` bails without normals **or** UV0 | `X3/libs/assimp/code/PostProcessing/CalcTangentsProcess.cpp:121-123` (normals), `:125-127` (UV) |
| `AI_CONFIG_PP_CT_MAX_SMOOTHING_ANGLE` default 45° | `X3/libs/assimp/code/PostProcessing/CalcTangentsProcess.cpp:75-77` |
| Preset expansion | `X3/libs/assimp/include/assimp/postprocess.h:641-660` (Fast), `:666-680` (Quality), `:696-700` (MaxQuality) |
| `AI_MATKEY_COLOR_DIFFUSE` **:996**, `COLOR_EMISSIVE` **:999**, `BASE_COLOR` **:1022**, `METALLIC_FACTOR` **:1026**, `ROUGHNESS_FACTOR` **:1030**, `EMISSIVE_INTENSITY` **:1090** | `X3/libs/assimp/include/assimp/material.h` |
| `aiTextureType_DIFFUSE=1` **:208**, `EMISSIVE=4` **:225**, `NORMALS=6` **:240**, `BASE_COLOR=12` **:288**, `METALNESS=15` **:291**, `DIFFUSE_ROUGHNESS=16` **:292** | `X3/libs/assimp/include/assimp/material.h` |
| `aiScene::GetEmbeddedTexture()` | `X3/libs/assimp/include/assimp/scene.h:441-444` |
| `aiTexture`: `mWidth` **:139**, `mHeight` **:146**, `pcData` **:179**; `mHeight==0` ⇒ compressed blob of `mWidth` bytes | `X3/libs/assimp/include/assimp/texture.h` |
| `aiTexel` member order is **b, g, r, a** | `X3/libs/assimp/include/assimp/texture.h:95-108` |

### 0.1 Facts about the sample models — verified by reading the `.glb` bytes

`strings` over the glTF JSON chunk of `SampleModels/stanford_dragon_pbr.glb` and `stanford_bunny_pbr.glb`:

- **Both embed all their textures** (`"images":[{"bufferView":N,"mimeType":"image/png"|"image/jpeg"}…]`, **no `"uri"` key on any image**). Dragon: 3 images. Bunny: 4 images. This settles a previously-unconfirmed claim: the `aiTexture::mHeight == 0` compressed-blob path in §4.5 is the path that actually executes for these models. `stbi_load_from_memory` is mandatory, not a nicety.
- **Both have `TANGENT` and `TEXCOORD_0` in `mesh.primitives[0].attributes`**, alongside `NORMAL` and `POSITION`. So assimp hands you tangents directly; the CPU fallback in §4.4 is for other content.
- **Both use `normalTexture`, `occlusionTexture`, `baseColorTexture`, `metallicRoughnessTexture`** (the bunny additionally has `emissiveTexture` and `emissiveFactor`). Occlusion and metallicRoughness point at the **same image index** in both files — the deduplication requirement in §4.5 is load-bearing, not theoretical.
- **Both have a four-level node hierarchy with two non-identity `matrix` entries** (`Sketchfab_model` → `Collada visual scene group` → `StanfordDragon_low` → `defaultMaterial`). The two matrices are ≈ +90° and ≈ −90° about X and **very nearly cancel**. So the node-transform bug below is masked by *cancellation*, not by the models being single-node. Correct claim: any model whose node transforms do not compose to identity imports at the wrong position/orientation/scale today. Useful consequence: after §4.3 lands, these two models must render in **visually the same place**; a position delta larger than ~1e-6 in the imported vertex data means the node walk is wrong.

### 0.2 Latent bugs this phase must fix, because it rewrites the code containing them

1. **Node transforms are discarded.** `AssetManager.cpp:212` iterates `scene->mMeshes[i]` flat and never touches `scene->mRootNode` / `aiNode::mTransformation`. Fixed by §4.3 step 1.
2. **`TriCount` over-counts.** `AssetManager.cpp:196-198` sums `mNumFaces`, but the emit loop skips faces with `mNumIndices != 3` (`:218`). `metadata->TriCount` (`:204`) can therefore exceed the number of triangles actually appended, and `BVHAccel` is constructed with that count at `:232` — so the BVH build reads `meshBuffer[firstTriIdx + i]` past the end of this mesh's slice and into the *next* mesh's triangles (or past `.end()` for the last mesh). `aiProcess_Triangulate` normally makes this a no-op, but `aiProcess_SortByPType` can leave a point/line mesh whose faces have 1 or 2 indices, and `FindDegenerates` can too. **The two-pass sizing in §4.3 must count EMITTED triangles.** The same wrong `TriCount` also propagates to `PhysicsWorld.cpp:1031/1033` and `:1093/1095`, which index `MeshBuffer[firstTriIdx + i]` over the same range.
3. **`Phong.comp:253` reads an uninitialized local**: `shadowRay.origin = origin + shadowRay.normal * SURFACE_BIAS;` uses `shadowRay.normal` before anything writes it. Fixed as a side effect of §7.9.
4. **Embedded textures are unreachable.** `LoadTexture` (`AssetManager.cpp:250`) only takes a filesystem path. Per §0.1 that is every texture in both sample models. Fixed by §4.5.

---

## 1. The new mesh representation

### 1.1 Three buffers, deliberately

| Buffer | Contents | Consumer | Why |
|---|---|---|---|
| `TriPositionBuffer` | de-referenced 3× `vec4` positions per triangle, **48 B/tri** | BVH build + BVH traversal **only** | Keeps the traversal inner loop at one contiguous 48-byte load per triangle test — byte-identical to today's layout, so `IntersectTri` and the whole build path are untouched |
| `TriRefBuffer` | 3 global vertex indices + material slot, **16 B/tri** | attribute fetch at a hit; the source of the Phase 7 index buffer | One aligned 16-byte load gives everything needed to resolve a hit |
| `VertexBuffer` | de-duplicated `Vertex`, **48 B/vertex** | attribute fetch at a hit; the Phase 7 vertex buffer (§8) | assimp's `JoinIdenticalVertices` already de-dups |

**Indexed *and* de-referenced, both, on purpose.** Traversal tests hundreds of triangles per ray and needs only positions; indexing them would cost 3 scattered index loads then 3 dependent scattered position loads per triangle test. Attributes are fetched **once per ray**, at the winning hit, where indexing is free and saves ~3× memory. The redundancy is bounded at ~48 B/tri; at 1 M triangles that is +48 MB against `BVHAccel`'s own ~64 MB (`nodeBuffer.resize(… + 2N-1)`, `BVHAccel.cpp:21`). Revisit in Phase 9's cook step (`ENGINE_PLAN.md:310`, meshoptimizer), not here.

**All three buffers hold data in ASSET SPACE**: node transforms from inside the model file are baked in (§4.3); the entity's `TransformComponent` is **not** — that stays in `TransformBuffer` and is applied per-entity in the shader exactly as today.

### 1.2 Do not pack attributes

No octahedral normals, no half-float UVs, no quantized tangents. The 48 B `Vertex` puts UVs in `.w` lanes that would otherwise be pure padding — exactly the "might use the space for some other data in the future" that `AssetTypes.h:11` anticipated. A packed vertex would be 24 B before std430 padding: a 2× win on a buffer that is *not* the traversal bottleneck. Meanwhile this phase's exit criterion (`ENGINE_PLAN.md:170`) is *"smooth-shaded, textured, normal-mapped geometry"*; adding an encode/decode layer in the same commit that introduces normals makes every wrong-looking pixel ambiguous between "my interpolation is wrong" and "my octahedral decode is wrong". Phase 3 (Slang layout pinning, `ENGINE_PLAN.md:198`) and Phase 9 re-lay-out this data anyway.

Three `vec4`s also means std430, std140 and scalar layouts all agree — verified: see §3.3.

**Escape hatch:** every shader read of a vertex goes through one function, `Vertex FetchVertex(uint idx)`. Swapping to a packed representation later touches that function, the C++ struct, and the `static_assert`s. Nothing else.

### 1.3 Tangents: full `vec4` with a handedness sign, stored not reconstructed

- `w` costs zero bytes here.
- Bitangent is **not** stored: `B = cross(N, T) * tangent.w`. Matches glTF; saves 12 B/vertex.
- Reconstructing tangents per-hit from UV/position derivatives yields the *per-face* tangent. assimp's `CalcTangentSpace` smooths across the 45° default (`CalcTangentsProcess.cpp:75-77`). Faceted tangents on smooth-shaded curved surfaces produce visible normal-map seams.
- **`w == 0.0` is the invalid-tangent sentinel** (no UVs ⇒ assimp produced no tangents, `CalcTangentsProcess.cpp:125-127`). The shader branches on it. Do **not** test for a zero-length tangent vector instead — floating point.

### 1.4 C++ definitions — new file `X3/src/Renderer/GpuTypes.h`

This is the single C++ home for every GPU-mirrored struct. It collapses today's spread across `AssetTypes.h`, `BVHAccel.h` and `Renderer.h`, and becomes the obvious target for Phase 3's reflection codegen.

```cpp
#pragma once
#include "lrpch.h"          // glm
#include <cstddef>          // offsetof
#include <cstdint>
#include <type_traits>

namespace X3::Gpu
{
    // ---- 48 B. BVH-only. Byte-identical to the old X3::Triangle. ----
    struct TrianglePositions {
        glm::vec4 v0{}, v1{}, v2{};
    };

    // ---- 16 B. i0/i1/i2 are GLOBAL indices into AssetPool::VertexBuffer.
    //      The shader indexes VertexBuffer[tr.i0] DIRECTLY and adds nothing.
    //      See §5.9-equivalent note in §7.7. materialSlot is mesh-local, dense. ----
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
    // EXPLICIT CONSTRUCTOR, deliberately. The old type at Renderer.h:50-55 had one and
    // Renderer.cpp:183-190 relies on emplace_back forwarding. A pure aggregate would work
    // only via C++20 parenthesised aggregate init; the project is C++23 today
    // (root CMakeLists.txt:26) so it would compile, but it is a silent trap if the
    // standard is ever lowered, and it produces unreadable diagnostics on arity mismatch.
    struct MeshEntityHandle {
        uint32_t firstTriIdx  = 0;
        uint32_t triCount     = 0;
        uint32_t firstNodeIdx = 0;
        uint32_t nodeCount    = 0;
        uint32_t transformIdx = 0;
        uint32_t materialBase = 0;      // index of this entity's slot 0 in MaterialBuffer
        uint32_t materialSlotCount = 0;
        uint32_t _pad0 = 0;

        MeshEntityHandle() = default;
        MeshEntityHandle(uint32_t firstTriIdx_, uint32_t triCount_,
                         uint32_t firstNodeIdx_, uint32_t nodeCount_,
                         uint32_t transformIdx_, uint32_t materialBase_,
                         uint32_t materialSlotCount_)
            : firstTriIdx(firstTriIdx_), triCount(triCount_),
              firstNodeIdx(firstNodeIdx_), nodeCount(nodeCount_),
              transformIdx(transformIdx_), materialBase(materialBase_),
              materialSlotCount(materialSlotCount_), _pad0(0) {}
    };

    // ---- 64 B. Moved verbatim out of Renderer.h:58-64. ----
    struct LightData {
        glm::vec4 position{};    // xyz position, w type (0=dir,1=point,2=spot)
        glm::vec4 direction{};   // xyz direction, w intensity
        glm::vec4 color{};       // xyz colour, w range
        glm::vec4 params{};      // x attenuation, y innerCone, z outerCone, w padding
    };
}
```

`pbrParams.w` was dead padding (`AssetTypes.h:20`); it now carries `normalScale`. `emission`, `color` and `pbrParams.xyz` are unchanged, so the existing reads at `PathTracing.comp:462`, `:467`, `:470` need no edit for those fields.

**Note the defaulted ctor.** `MeshEntityHandle() = default` is required because declaring any constructor suppresses the implicit default one, and `std::vector<MeshEntityHandle>::resize` needs it. Verified by compiling (§3.3).

### 1.5 GLSL definitions — new file `X3/res/shaders/GpuTypes.glsl`

`#include`d by all three `.comp` files. This deletes the three-way duplication at `PathTracing.comp:31-71`, `PBR.comp:25-63`, `Phong.comp:25-63`.

```glsl
#ifndef X3_GPU_TYPES_GLSL
#define X3_GPU_TYPES_GLSL

// !!! MIRRORED. C++ side: X3/src/Renderer/GpuTypes.h and Project/Assets/BVHAccel.h.
// !!! Any edit here MUST be matched there, and the static_asserts updated.
// !!! Phase 3 (Slang reflection codegen) replaces this file with generated code.

// std430 - 32 B (CPU: BVHAccel::Node, Project/Assets/BVHAccel.h:13-20)
struct BVHNode { vec3 min; uint leftChild_Or_FirstTri; vec3 max; uint triCount; };

// std430 - 48 B (CPU: Gpu::TrianglePositions)
struct TrianglePositions { vec4 v0, v1, v2; };

// std430 - 16 B (CPU: Gpu::TriRef). i0/i1/i2 are GLOBAL VertexBuffer indices.
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
#define X3_MAX_MATERIAL_TEXTURES 128
#endif
```

Keep the member names `min` / `max` on `BVHNode` (as at `PathTracing.comp:34,36`) — renaming them is churn for nothing. **Note:** `PBR.comp:58-63` and `Phong.comp:58-63` currently name the light struct `Light`, not `LightData`, and declare `Light LightBuffer[];` at `PBR.comp:117-119` / `Phong.comp:117-119`. Unifying on `LightData` means editing those two SSBO declarations too.

### 1.6 AssetPool and metadata changes

`X3/src/Project/Assets/AssetManager.h:45-48` becomes:

```cpp
std::vector<Gpu::TrianglePositions> TriPositionBuffer;  // was MeshBuffer
std::vector<Gpu::TriRef>            TriRefBuffer;       // NEW
std::vector<Gpu::Vertex>            VertexBuffer;       // NEW
std::vector<uint32_t>               TriIndexBuffer;     // NEW - 3*triCount, tightly packed, for Phase 7 (§8)
std::vector<uint32_t>               BvhPrimIndexBuffer; // was IndexBuffer (RENAME - see below)
std::vector<BVHAccel::Node>         NodeBuffer;
std::unordered_map<LR_GUID, TexturePixels> TexturePixelsByGuid; // replaces the flat TextureBuffer, §6.4
```

**Why `IndexBuffer` → `BvhPrimIndexBuffer`:** the existing `IndexBuffer` is the BVH's *primitive permutation* (`BVHAccel.cpp:25-28` fills it with `0..N-1` then permutes it during `SubDivide`). It is not, and never was, a graphics index buffer. Phase 2 introduces an actual graphics index buffer (`TriIndexBuffer`, §8) and having two things called "index buffer" is how someone binds the wrong one to `vkCmdBindIndexBuffer`.

`AssetPool::AssetType` (`AssetManager.h:62-69`) becomes:

```cpp
enum struct AssetType {
    Metadata,
    TriPositionBuffer,   // was MeshBuffer
    TriRefBuffer,        // NEW
    VertexBuffer,        // NEW
    TriIndexBuffer,      // NEW
    BvhPrimIndexBuffer,  // was IndexBuffer
    NodeBuffer,
    Textures,            // was TextureBuffer
    COUNT
};
```

`MeshMetadata` (`AssetTypes.h:27-32`) becomes:

```cpp
struct SubmeshInfo {
    uint32_t firstTriIdx  = 0;    // MESH-LOCAL, relative to MeshMetadata::firstTriIdx
    uint32_t triCount     = 0;
    uint32_t materialSlot = 0;    // dense 0..materialSlotCount-1
    std::string name;             // aiMesh::mName (mesh.h:793), for editor UI
};

struct MeshMetadata : public Metadata {
    uint32_t firstTriIdx    = 0;
    uint32_t TriCount       = 0;   // EMITTED triangles. See §0.2 item 2.
    uint32_t firstNodeIdx   = 0;
    uint32_t nodeCount      = 0;
    uint32_t firstVertexIdx = 0;   // NEW - CPU-only bookkeeping, never uploaded, never added in-shader
    uint32_t vertexCount    = 0;   // NEW - CPU-only
    uint32_t firstIndex     = 0;   // NEW - CPU-only, into AssetPool::TriIndexBuffer (== 3*firstTriIdx)
    uint32_t indexCount     = 0;   // NEW - CPU-only (== 3*TriCount)
    uint32_t materialSlotCount = 0;// NEW
    std::vector<SubmeshInfo>   submeshes;         // NEW, CPU-only
    std::vector<Gpu::Material> importedMaterials; // NEW, one per slot, from aiMaterial
    ~MeshMetadata() override = default;
};
```

Delete `TextureMetadata::texStartIdx` (`AssetTypes.h:36`). **Verification of "unused":** `grep -rn texStartIdx X3/src X3-Editor/src X3-Runtime/src` returns exactly three hits — the declaration (`AssetTypes.h:36`), the single write (`AssetManager.cpp:276`) and the single read (`Renderer.cpp:243`). It is a plain struct field with no macro, no build-system, and no `#ifdef` indirection, so grep is sufficient here; the sites are all edited by this phase anyway.

---

## 2. BVH impact

### 2.1 What changes: essentially only a type name

`BVHAccel` consumes positions and nothing else. Every read is `.v0/.v1/.v2`:

- `PrecomputeCentroids` — `BVHAccel.h:65-73`
- `UpdateAABB` — `BVHAccel.cpp:42-53`
- `FindBestSplitPlane` binning — `BVHAccel.cpp:76-84`
- `SubDivide` partition (centroids only) — `BVHAccel.cpp:117-165`

Since `Gpu::TrianglePositions` is byte- and member-identical to today's `Triangle`, the required edits are exactly:

1. `BVHAccel.h:50` — `BVHAccel(const std::vector<Gpu::TrianglePositions>& triPositions, const uint32_t firstTriIdx, const uint32_t triCount);`
2. `BVHAccel.h:82` — `const std::vector<Gpu::TrianglePositions>& m_TriBuff;`
3. `BVHAccel.h:69` — `const Gpu::TrianglePositions& t = m_TriBuff[m_FirstTriIdx + i];`
4. `BVHAccel.cpp:46` — `const Gpu::TrianglePositions& t = …`
5. `BVHAccel.cpp:78` — `const Gpu::TrianglePositions& tri = …`
6. `BVHAccel.cpp:7-8` — ctor signature
7. `BVHAccel.h:5` — `#include "Project/Assets/AssetTypes.h"` stays (it will transitively include `GpuTypes.h`), or add `#include "Renderer/GpuTypes.h"` directly. Prefer the direct include: it makes `BVHAccel.h` independent of `AssetTypes.h`.

**Nothing else in BVHAccel changes.** `Node` layout unchanged (32 B). Split heuristic, binning, SAH, partition, node-count math: unchanged. Build time and traversal cost: unchanged. The `2N-1` node reserve at `BVHAccel.cpp:21` and the `m_IdxBuff = &indexBuffer[m_FirstTriIdx]` aliasing at `:25` are unchanged — the latter is why the BVH's prim-index array must remain exactly `TriCount` entries per mesh and why fixing the `TriCount` over-count (§0.2 item 2) matters here specifically.

### 2.2 Traversal needs only positions — confirmed

`TraverseBVH` (`PathTracing.comp:245-311`) reads exactly `NodeBuffer[...].min/.max/.leftChild_Or_FirstTri/.triCount`, `IndexBuffer[...]` (the BVH permutation) and `MeshBuffer[...]` passed to `IntersectTri`. `IntersectTri` (`:224-243`) reads only `tri.v0/v1/v2`. There is no attribute access in the traversal path, and there must not be one after this phase.

### 2.3 Where the attribute fetch happens — three distinct points, do not conflate

**(a) Inside `IntersectTri` — record, do not fetch.** Three scalar writes on the accepted-hit path only. Full code in §7.4.

**Barycentric convention, derived from this exact code — do not guess.** With `E1 = v1-v0`, `E2 = v2-v0`, `AO = origin-v0`, `DAO = cross(AO,dir)`, `det = -dot(dir,Ng)`, the code's `u = dot(E2,DAO)*invdet` (`:235`) is the weight of **v1**, and `v = -dot(E1,DAO)*invdet` (`:236`) is the weight of **v2**. Therefore:

```
A_interp = A0*(1.0 - u - v) + A1*u + A2*v
```

**(b) Inside the leaf loop (`PathTracing.comp:258-265`) — pass the index, drop the material write.** `triIndex` at `:259` is the **mesh-local** triangle index (the BVH permutation value). Pass it to `IntersectTri`. Delete `ray.materialIdx = entityHandle.materialIdx;` (`:263`) — the material now comes from `TriRef.materialSlot` at resolve time.

**(c) In `CheckRayCollision` (`:313-338`) — record the winner; resolve once, after the loop.** Cost of the resolve: one 16-byte load + three 48-byte loads + ~30 ALU, **per ray**, not per triangle test. Do not fetch attributes inside `TraverseBVH`. Do not fetch them inside the entity loop.

---

## 3. The struct-sync problem — interim measures

Phase 3 fixes this with Slang reflection (`ENGINE_PLAN.md:190-192`). Until then, three cheap measures. All three, not a subset.

### 3.1 Collapse the GLSL side from three copies to one

Create `X3/res/shaders/GpuTypes.glsl` (§1.5). In all three `.comp` files, delete the struct block (`PathTracing.comp:31-71`, `PBR.comp:25-63`, `Phong.comp:25-63`) and replace with, immediately after the `#version 460 core` line (`PathTracing.comp:6`, `PBR.comp:4`, `Phong.comp:4`):

```glsl
#extension GL_GOOGLE_include_directive : require
#include "GpuTypes.glsl"
```

CMake changes, in `X3/CMakeLists.txt`. **Locate these by text, not by line number** — Phase 1a deletes the `if(X3_GRAPHICS_API STREQUAL "Vulkan")` blocks at `:27-31`, `:56-61`, `:71-85` and `:125-130`, which shifts everything after them upward. In the current tree the shader rule is `:87-112`:

- `:100` `COMMAND ${GLSLC_EXECUTABLE} ${GLSL} -o ${SPIRV}` becomes
  `COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.3 -I ${CMAKE_CURRENT_SOURCE_DIR}/res/shaders ${GLSL} -o ${SPIRV}`
- `:101` `DEPENDS ${GLSL}` becomes
  `DEPENDS ${GLSL} ${CMAKE_CURRENT_SOURCE_DIR}/res/shaders/GpuTypes.glsl`
- The `file(GLOB_RECURSE …)` at `:89-93` globs `*.comp/*.vert/*.frag` only, so `GpuTypes.glsl` will not be compiled standalone. Correct as-is; **do not add `*.glsl`**.

**`--target-env=vulkan1.3` is required, not cosmetic, and this was verified by compiling.** I built a probe shader containing `sampler2D u_MaterialTextures[128]` indexed with `nonuniformEXT()` and disassembled the output with `spirv-dis`:

- With the current command (no `--target-env`, i.e. the glslc default of vulkan1.0) it emits **SPIR-V 1.0** carrying `OpCapability ShaderNonUniform`, `OpCapability SampledImageArrayNonUniformIndexing` **and `OpExtension "SPV_EXT_descriptor_indexing"`**. A module with that `OpExtension` requires `VK_EXT_descriptor_indexing` to be an *enabled device extension*; on a Vulkan 1.2+ device where the feature is core-promoted, the extension string is normally not enabled, and validation rejects the module at `vkCreateShaderModule`/pipeline creation.
- With `--target-env=vulkan1.2` it emits SPIR-V 1.5 and with `--target-env=vulkan1.3` SPIR-V 1.6; in both cases the capabilities are core and **no `OpExtension` is emitted**.
- The three `.spv` files currently in `X3/res/shaders/` are SPIR-V 1.0 (`spirv-dis` header), confirming the default is what is in use today.

Since Part 2 already raises the device to Vulkan 1.3, `--target-env=vulkan1.3` is the right choice. `vulkan1.2` (SPIR-V 1.5) is the conservative fallback if a target platform ever reports only 1.2.

This is the highest-value part of §3: three hand-maintained copies is where the drift actually happens.

### 3.2 Collapse the C++ side into one file

Create `X3/src/Renderer/GpuTypes.h` (§1.4). Then:

- Delete `struct Triangle` (`AssetTypes.h:10-14`) and `struct Material` (`AssetTypes.h:16-21`). Add `#include "Renderer/GpuTypes.h"` to `AssetTypes.h`.
- Delete `struct MeshEntityHandle` (`Renderer.h:41-56`) and `struct LightData` (`Renderer.h:58-64`); `#include "Renderer/GpuTypes.h"` in `Renderer.h` and use `Gpu::MeshEntityHandle` / `Gpu::LightData` in `ParsedScene` (`Renderer.h:66-79`). Delete the `struct Material;` forward declaration at `Renderer.h:19` and use `Gpu::Material` in `ParsedScene::MaterialBuffer` (`Renderer.h:70`).
- `BVHAccel::Node` stays where it is (`BVHAccel.h:13-20`) — it is algorithm-owned, and moving it would drag `BVHAccel.h` into the renderer's include graph.
- Rename `Triangle` → `Gpu::TrianglePositions` at its remaining use sites: `PhysicsWorld.cpp:1035`, `:1097`.

Note that `MeshEntityHandle` and `LightData` are currently **private nested types of `class Renderer`** (`Renderer.h:27` opens the private section), so `static_assert`s on them cannot be written outside the class today. Moving them out is what makes §3.3 possible.

### 3.3 The exact `static_asserts` — CORRECTED, and verified by compiling

**Read this paragraph before writing the asserts.** The obvious formulation `static_assert(sizeof(glm::vec4) == 16 && alignof(glm::vec4) == 16)` **does not compile in this repo**. I compiled a probe against `X3/libs/glm` with `g++ -std=c++23`:

```
vec4  size=16 align=4
uvec4 size=16 align=4
vec3  size=12 align=4
mat4  size=64 align=4
```

This glm is built without `GLM_FORCE_DEFAULT_ALIGNED_GENTYPES` (nothing in `X3/src/lrpch.h:21-24` or anywhere outside `libs/` defines any `GLM_FORCE_*`), so gentypes have `alignof == 4`. **Sizes and offsets are correct; alignment is not 16.** That is fine for GPU mirroring — std430 cares about member offsets and struct size, and `std::vector`'s allocator returns 16-byte-aligned storage regardless — but an `alignof == 16` assertion is a build break. Assert sizes and offsets only.

**At the bottom of `X3/src/Renderer/GpuTypes.h`, inside `namespace X3::Gpu`:**

```cpp
// These verify the C++ side only. They CANNOT verify GpuTypes.glsl - §3.1 is what
// reduces that risk and Phase 3 (Slang reflection) is what eliminates it.
// NOTE: alignof(glm::vec4) is 4, not 16, in this build (glm without
// GLM_FORCE_DEFAULT_ALIGNED_GENTYPES). Do not assert on alignment.
static_assert(sizeof(glm::vec4)  == 16);
static_assert(sizeof(glm::uvec4) == 16);
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

// --- Vertex : std430 48 B. Also the Phase 7 vertex-buffer stride (§8). ---
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
static_assert(offsetof(BVHAccel::Node, min)                   ==  0);
static_assert(offsetof(BVHAccel::Node, leftChild_Or_FirstTri) == 12);
static_assert(offsetof(BVHAccel::Node, max)                   == 16);
static_assert(offsetof(BVHAccel::Node, triCount)              == 28);
```

`BVHAccel.h` needs `#include <cstddef>` and `#include <type_traits>` for these.

**All of the above compiled clean** (`g++ -std=c++23 -I X3/libs/glm`), including `std::vector<MeshEntityHandle>::emplace_back(1u,…,7u)` through the explicit ctor.

**And the GLSL side was verified independently** — I compiled `GpuTypes.glsl` as written in §1.5 inside a probe shader with `glslc --target-env=vulkan1.3 -I <dir>` and read the resulting layout decorations with `spirv-dis`:

```
OpDecorate %_runtimearr_Vertex             ArrayStride 48     Offsets  0 / 16 / 32
OpDecorate %_runtimearr_TriRef             ArrayStride 16     Offsets  0 / 4 / 8 / 12
OpDecorate %_runtimearr_Material           ArrayStride 64     Offsets  0 / 16 / 32 / 48
OpDecorate %_runtimearr_TrianglePositions  ArrayStride 48
OpDecorate %_runtimearr_EntityHandle       ArrayStride 32
OpDecorate %_runtimearr_BVHNode            ArrayStride 32
OpDecorate %_runtimearr_LightData          ArrayStride 64
```

Both sides agree on every struct. Put a comment recording that above the assert block.

---

## 4. Assimp import changes

### 4.1 Preset: keep it, change nothing mandatory

`aiProcessPreset_TargetRealtime_MaxQuality` (`AssetManager.cpp:190`) already supplies everything needed — verified by expanding `postprocess.h:641-660` (Fast) → `:666-680` (Quality) → `:696-700` (MaxQuality):
`CalcTangentSpace | GenSmoothNormals | JoinIdenticalVertices | ImproveCacheLocality | LimitBoneWeights | RemoveRedundantMaterials | SplitLargeMeshes | Triangulate | GenUVCoords | SortByPType | FindDegenerates | FindInvalidData | FindInstances | ValidateDataStructure | OptimizeMeshes`.

Two facts to internalise rather than assume:

- **`aiProcess_CalcTangentSpace` requires UV0 *and* normals.** `CalcTangentsProcess.cpp:121-123` returns `false` and logs *"Failed to compute tangents; need normals"* if `mNormals == nullptr`; `:125-127` returns `false` and logs *"Failed to compute tangents; need UV data in channel"* if `mTextureCoords[0] == nullptr`. `mTangents` (`mesh.h:707`) will then be `nullptr`. **Do not assume it is non-null.**
- **`aiProcess_GenUVCoords` does not invent UVs.** It only converts non-UV mapping modes (spherical/cylindrical/planar, declared in the material) into a UV channel. A mesh authored with no texture coordinates comes out with none.

**Do not add `aiProcess_PreTransformVertices`.** It would incidentally fix the node-transform bug, but it destroys the node graph and merges meshes by material, which fights the per-submesh material design in §5. Fix the transform bug directly (§4.3).

Optional, only if profiling shows submesh explosion: `importer.SetPropertyInteger(AI_CONFIG_PP_SLM_TRIANGLE_LIMIT, …)` to tune `SplitLargeMeshes`. Leave at default in this phase.

### 4.2 Exact `aiMesh` members to read

Per `X3/libs/assimp/include/assimp/mesh.h` — **these are the `aiMesh` line numbers, not `aiAnimMesh`'s.** (`aiAnimMesh` is declared at `:473` and has confusingly similar members at `:489/:492/:557/:567/:588`; `aiMesh` is declared at `:638`.)

| Member | Line | Guard | Fallback |
|---|---|---|---|
| `mPrimitiveTypes` | `:645` | `if ((m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) continue;` | skip the mesh entirely — `SortByPType` has already split points/lines out |
| `mNumVertices` | `:652` | — | — |
| `mVertices[v]` | — | always non-null | — |
| `mNormals[v]` | — | `m->HasNormals()` **`:916`** | compute face normals on CPU and average by position; `LOG_ENGINE_WARN` naming `m->mName.C_Str()` |
| `mTangents[v]` | **`:707`** | `m->HasTangentsAndBitangents()` **`:926`** | see §4.4 |
| `mBitangents[v]` | **`:718`** | same | used only to derive the handedness sign, then discarded |
| `mTextureCoords[0][v].x/.y` | **`:736`** | `m->HasTextureCoords(0)` **`:943`** | `uv = vec2(0,0)`; warn once per submesh |
| `mNumUVComponents[0]` | **`:747`** | — | use `.x`,`.y` only; ignore a 3rd component |
| `mFaces[f].mNumIndices == 3` | — | already checked at `AssetManager.cpp:218` | keep the `continue`, and **count only faces that pass it** (§0.2 item 2) |
| `mMaterialIndex` | **`:779`** | always valid; index `scene->mMaterials[...]` | — |
| `mName` | **`:793`** | — | store into `SubmeshInfo::name` |

Handedness: `w = (dot(cross(N, T), B) < 0.0f) ? -1.0f : 1.0f`, using assimp's `mBitangents` as `B`.

### 4.3 The rewritten `LoadMesh` — replaces `AssetManager.cpp:196-233`

Ordering matters.

**1. Recursive node walk.** New private helper on `AssetManager`:

```cpp
// Walks the aiNode graph, accumulating world = parent * node->mTransformation, and
// emits (meshIndex, world) for every mesh index on every node. A mesh referenced by
// two nodes is emitted TWICE - that is correct, it is two instances.
void CollectMeshInstances(const aiScene* scene, const aiNode* node,
                          const glm::mat4& parent,
                          std::vector<std::pair<uint32_t, glm::mat4>>& out) const;
```

Seed with `scene->mRootNode` and `glm::mat4(1.0f)`. **assimp matrices are row-major; transpose on conversion**: `glm::mat4 m = glm::transpose(glm::make_mat4(&node->mTransformation.a1));`. This fixes the bug at `AssetManager.cpp:212`.

*Regression check specific to this repo* (from §0.1): `SampleModels/stanford_dragon_pbr.glb` and `stanford_bunny_pbr.glb` have a 4-level node chain whose two non-identity matrices nearly cancel. After this change their imported vertex positions must match the pre-change positions to within ~1e-6. If they move visibly, the transpose or the multiplication order is wrong.

**2. Two passes for sizing, counting EMITTED triangles.** First pass over the instance list accumulates:
```cpp
size_t vertexTotal = 0, triTotal = 0;
for (auto& [meshIdx, world] : instances) {
    const aiMesh* m = scene->mMeshes[meshIdx];
    if ((m->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0) continue;
    vertexTotal += m->mNumVertices;
    for (unsigned f = 0; f < m->mNumFaces; ++f)
        if (m->mFaces[f].mNumIndices == 3) ++triTotal;   // NOT mNumFaces
}
```
`reserve()` `TriPositionBuffer`, `TriRefBuffer`, `VertexBuffer`, `TriIndexBuffer` (`3*triTotal`) with these. Set `metadata->TriCount = triTotal;` and `metadata->vertexCount = vertexTotal;`. **`metadata->TriCount` must equal the number of triangles actually appended in step 5** — assert it.

**3. Material slot table.** Build a dense map `aiMaterialIndex → slot` in first-encounter order across the instance list. Populate `metadata->importedMaterials[slot]` from each `aiMaterial` (§4.5). Set `metadata->materialSlotCount = slotTable.size()`.

**4. Per instance, append vertices.** Record `const uint32_t instanceFirstVertex = uint32_t(VertexBuffer.size());` before appending. For each source vertex:
- position: `glm::vec3 p = glm::vec3(world * glm::vec4(v.x, v.y, v.z, 1.0f));`
- normal: `glm::mat3 nrmMat = glm::mat3(glm::transpose(glm::inverse(world)));` then `normalize(nrmMat * n)`
- tangent: `normalize(glm::mat3(world) * t)` — **`mat3(world)`, NOT the inverse-transpose.** Under non-uniform scale, using the same matrix for both produces a subtly skewed TBN that only shows up on scaled objects.
- write `positionU = vec4(p, uv.x)`, `normalV = vec4(n, uv.y)`, `tangent = vec4(t, handedness)`.

**5. Per instance, append triangles.** For each face with `mNumIndices == 3`:
```cpp
const uint32_t g0 = instanceFirstVertex + face.mIndices[0];   // GLOBAL indices
const uint32_t g1 = instanceFirstVertex + face.mIndices[1];
const uint32_t g2 = instanceFirstVertex + face.mIndices[2];
TriRefBuffer.push_back(Gpu::TriRef{ g0, g1, g2, slot });
TriPositionBuffer.push_back(Gpu::TrianglePositions{
    VertexBuffer[g0].positionU * glm::vec4(1,1,1,0),          // w must be 0, not uv.x
    VertexBuffer[g1].positionU * glm::vec4(1,1,1,0),
    VertexBuffer[g2].positionU * glm::vec4(1,1,1,0) });
TriIndexBuffer.push_back(g0); TriIndexBuffer.push_back(g1); TriIndexBuffer.push_back(g2);
```
**The `.w` lane of `TrianglePositions` must be zeroed.** `IntersectTri` only reads `.xyz`, but `BVHAccel::PrecomputeCentroids` (`BVHAccel.h:70`) sums the full `vec4` — a stray `uv.x` in `.w` would not change the `.xyz` centroid but would produce garbage in `centroids[i].w`… which is then narrowed to `glm::vec3` on assignment, so it is harmless *today*. Zero it anyway; relying on that narrowing is exactly the kind of thing that breaks silently when someone widens `m_Centroids`.

`TriPositionBuffer`, `TriRefBuffer` must be appended **in lockstep** and end at the same length; `TriIndexBuffer` at exactly 3× that. Assert all three.

**6. Submesh ranges.** After each instance, push `SubmeshInfo{ localFirstTri, localTriCount, slot, m->mName.C_Str() }` with **mesh-local** `firstTriIdx` (i.e. relative to `metadata->firstTriIdx`).

**7. Tangent fallback** for instances where `mTangents == nullptr` but UVs exist — call the shared CPU helper (§4.4) over that instance's range.

**8. Build the BVH** exactly as today (`AssetManager.cpp:232-233`), now over `TriPositionBuffer` and `BvhPrimIndexBuffer`.

**9. Fill the remaining metadata:** `firstVertexIdx`, `vertexCount`, `firstIndex = 3 * firstTriIdx`, `indexCount = 3 * TriCount`.

**10. `MarkUpdated`** for `TriPositionBuffer`, `TriRefBuffer`, `VertexBuffer`, `TriIndexBuffer`, `BvhPrimIndexBuffer`, `NodeBuffer`, `Metadata`.

### 4.4 Meshes lacking UVs / tangents

New shared CPU helper, used by both the importer fallback and the primitive generators (§4.6). New files `X3/src/Project/Assets/MeshUtils.h/.cpp` (no CMake edit needed — `X3/CMakeLists.txt:54` globs `src/*.cpp` recursively):

```cpp
namespace X3 {
    // Per-triangle tangents from UV derivatives, accumulated per vertex, then
    // Gram-Schmidt orthogonalised against the vertex normal. Sets tangent.w to the
    // handedness sign, or 0.0 where the UV parameterisation is degenerate.
    // vertices[i].normalV.xyz must already be valid. Indices in `tris` are GLOBAL.
    void ComputeTangents(std::vector<Gpu::Vertex>& vertices,
                         const std::vector<Gpu::TriRef>& tris,
                         uint32_t firstTri, uint32_t triCount);

    // Face normals averaged per vertex, written into vertices[i].normalV.xyz.
    void ComputeNormals(std::vector<Gpu::Vertex>& vertices,
                        const std::vector<Gpu::TriRef>& tris,
                        uint32_t firstTri, uint32_t triCount);
}
```

Policy matrix:

| assimp gave | Action |
|---|---|
| normals + UVs + tangents | use directly |
| normals + UVs, no tangents | `ComputeTangents()` |
| normals, **no UVs** | `uv = (0,0)`, `tangent = vec4(0)` ⇒ `w == 0` sentinel. `LOG_ENGINE_WARN` once per submesh naming the mesh. Normal mapping is skipped in-shader. |
| no normals (shouldn't happen under the preset) | `ComputeNormals()`, then as above |

### 4.5 Material import from `aiMaterial`

New private helper:

```cpp
Gpu::Material AssetManager::ImportMaterial(const aiScene* scene, const aiMaterial* mat,
                                           const std::filesystem::path& modelDir,
                                           LR_GUID meshGuid);
```

Scalar/colour reads, falling back left to right:
- base colour: `AI_MATKEY_BASE_COLOR` (`material.h:1022`) → `AI_MATKEY_COLOR_DIFFUSE` (`:996`) → `vec4(1)`
- metallic: `AI_MATKEY_METALLIC_FACTOR` (`:1026`) → `0.0`
- roughness: `AI_MATKEY_ROUGHNESS_FACTOR` (`:1030`) → `0.5`
- emissive: `AI_MATKEY_COLOR_EMISSIVE` (`:999`) → `vec3(0)`; strength from `AI_MATKEY_EMISSIVE_INTENSITY` (`:1090`) → `1.0`
- `normalScale` (`pbrParams.w`) → `1.0`

Textures via `mat->GetTexture(type, 0, &path)` for:
- `aiTextureType_BASE_COLOR` (12, `material.h:288`) → fall back to `aiTextureType_DIFFUSE` (1, `:208`)
- `aiTextureType_NORMALS` (6, `:240`)
- `aiTextureType_METALNESS` (15, `:291`) → fall back to `aiTextureType_DIFFUSE_ROUGHNESS` (16, `:292`) — in glTF these resolve to the same ORM image
- `aiTextureType_EMISSIVE` (4, `:225`)

**Embedded textures.** Per §0.1 this is the path both sample models take. If the returned path starts with `'*'`, or `scene->GetEmbeddedTexture(path.C_Str())` (`scene.h:441`) returns non-null:
- `aiTexture::mHeight == 0` (`texture.h:146`) ⇒ `pcData` (`:179`) is a **compressed blob of `mWidth` bytes** (`:139`) ⇒ `stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(tex->pcData), int(tex->mWidth), &w, &h, &c, 4)`. Both sample models are `image/png` and `image/jpeg`, both of which stb_image decodes.
- `mHeight != 0` ⇒ raw `aiTexel` array of `mWidth * mHeight` texels. **`aiTexel` member order is `b, g, r, a`** (`texture.h:95-108`) — swizzle to RGBA on copy.

Otherwise resolve the path relative to `modelDir` and go through the existing `stbi_load` path.

**Deduplicate by key `(embedded index | absolute source path)`** so an image shared by several materials uploads once. Both sample models point `occlusionTexture` and `metallicRoughnessTexture` at the same image (§0.1); without dedup you upload it twice and burn a texture-table slot.

Each imported texture is registered in the AssetPool under a derived GUID and its index in the GPU texture table is written into `Gpu::Material::textures` (§6.6).

### 4.6 Primitive meshes

`CreatePrimitiveMeshes` (`AssetManager.cpp:349-557`) and the `makeTri` lambda (`:351-357`) must produce attributes. Replace `makeTri(vec3,vec3,vec3)` with an emit-vertex helper taking `(position, normal, uv)` plus an emit-triangle helper taking three vertex indices; run `ComputeTangents()` (§4.4) over each primitive's finished vertex/TriRef arrays.

Analytic normals and UVs:
- **Cube** (`:362-386`): per-face constant normal; UV = the two in-plane axes remapped to `[0,1]`. Vertices must **not** be shared across faces (normals differ).
- **Sphere** (`:391-416`): `N = normalize(p)`; `uv = (phi/2π, theta/π)`.
- **Plane** (`:420-429`): `N = (0,1,0)`; `uv = (x+0.5, z+0.5)`.
- **Cylinder** (`:433-461`): side `N = normalize(x,0,z)`, `uv = (angle/2π, (y+h)/2h)`; caps `N = ±Y`, planar UV.
- **Capsule** (`:465-526`): body as cylinder; hemispheres as sphere about the cap centre.
- **Cone** (`:530-554`): side normal `normalize(vec3(cos a, r/h, sin a))` — **not** the radial normal; cap `N = -Y`.

`CreatePrimitiveMesh` (`AssetManager.h:161`, `.cpp:320-347`) changes signature:

```cpp
void CreatePrimitiveMesh(LR_GUID guid,
                         const std::vector<Gpu::Vertex>& vertices,
                         const std::vector<Gpu::TriRef>& tris,   // indices are LOCAL to `vertices`
                         const char* name);
```

It rebases the local indices onto `AssetPool::VertexBuffer` (adding `firstVertexIdx`), de-references positions into `TriPositionBuffer`, appends `TriIndexBuffer`, and fills the same metadata fields as §4.3 step 9 — so the generators never build those arrays by hand. `materialSlotCount = 1`, one default `Gpu::Material` in `importedMaterials`, one `SubmeshInfo` covering the whole primitive.

---

## 5. Per-submesh materials

### 5.1 Design: one BVH per asset, material slot per triangle

**Do not split the BVH per submesh.** `CheckRayCollision` (`PathTracing.comp:315`) already loops over every entity linearly; multiplying entities by submesh count multiplies that cost. Keep one BVH per mesh asset spanning all submeshes — the SAH build handles the concatenation fine.

The material index at a hit becomes:

```
materialIdx = entity.materialBase + TriRefBuffer[entity.rootTriIdx + triIdxLocal].materialSlot
```

`materialSlot` rides free in `TriRef.w` — the fetch is already happening to get the vertex indices, so **per-submesh materials cost zero extra loads.**

### 5.2 Entity lookup table

`Gpu::MeshEntityHandle` (§1.4) replaces the single `MaterialIdx` (`Renderer.h:48`) with `materialBase` + `materialSlotCount`. 24 B → 32 B. `MaterialBuffer` is no longer one entry per entity; it is a **flattened, variable-stride** array: entity *i*'s materials occupy `[materialBase, materialBase + materialSlotCount)`.

### 5.3 `Renderer::Parse` — replaces `Renderer.cpp:171-190`

```cpp
// (Renderer.cpp:163-166) missing-mesh path: keep the `continue`, but upgrade the
// silent skip to LOG_ENGINE_WARN naming the GUID while you are here.

const uint32_t slotCount = std::max(1u, metadata->materialSlotCount);
const uint32_t base      = uint32_t(pScene->MaterialBuffer.size());

const MaterialComponent* mc = e.HasComponent<MaterialComponent>()
                            ? &e.GetComponent<MaterialComponent>() : nullptr;

for (uint32_t slot = 0; slot < slotCount; ++slot) {
    if (mc && slot < mc->slots.size()) {
        pScene->MaterialBuffer.emplace_back(ToGpuMaterial(mc->slots[slot], *m_TextureTable));
    } else if (slot < metadata->importedMaterials.size()) {
        pScene->MaterialBuffer.emplace_back(metadata->importedMaterials[slot]);
    } else {
        pScene->MaterialBuffer.emplace_back();          // default Gpu::Material
    }
}

pScene->MeshEntityLookupTable.emplace_back(
    metadata->firstTriIdx, metadata->TriCount,
    metadata->firstNodeIdx, metadata->nodeCount,
    uint32_t(pScene->TransformBuffer.size() - 1),
    base, slotCount);
```

The `max(1u, …)` clamp is what makes the whole design survivable when `materialSlotCount` is stale relative to `MaterialComponent::slots` (see §5.5) — keep it.

`Renderer.cpp:158` `pScene->MaterialBuffer.reserve(renderableView.size_hint())` becomes an under-estimate. Change to `renderableView.size_hint() * 2`. `Renderer.cpp:156-157` are unchanged.

`ToGpuMaterial(const MaterialDesc&, TextureTable&)` is a free function in `Renderer.cpp`; it resolves the four texture GUIDs through the table (§6.6) and packs `pbrParams = vec4(metallic, roughness, ao, normalScale)`.

The MaterialSSBO upload path already keys on element count via `ensureCapacity`/`write` on `m_MaterialSSBO`, so the variable stride needs no change there.

### 5.4 `MaterialComponent` becomes a slot vector

`X3/src/Project/Scene/Components.h:62-71`:

```cpp
struct MaterialDesc {
    glm::vec4 emission = {0.0f, 0.0f, 0.0f, 0.0f};   // xyz colour, w strength
    glm::vec4 color    = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic    = 0.0f;
    float roughness   = 0.5f;
    float ao          = 1.0f;
    float normalScale = 1.0f;                        // was `float _padding`
    LR_GUID baseColorTex  = LR_GUID::INVALID;
    LR_GUID normalTex     = LR_GUID::INVALID;
    LR_GUID metalRoughTex = LR_GUID::INVALID;
    LR_GUID emissiveTex   = LR_GUID::INVALID;
};

struct MaterialComponent {
    std::vector<MaterialDesc> slots{ MaterialDesc{} };   // NEVER empty
};
```

Touch points, all of which currently assume a flat struct:

| Site | Change |
|---|---|
| `Scene.cpp:42-43` (duplicate), `Scene.cpp:111-112` (registry copy) | value-copy of a vector; works as written, no edit |
| `Scene.cpp:206-220` serialize / `Scene.cpp:465-472` deserialize | §9.2 |
| `InspectorPanel.cpp:174-323` (the whole `DrawComponent<MaterialComponent>` lambda) | wrap the body in `for (size_t s = 0; s < materialComponent.slots.size(); ++s)` with an `ImGui::PushID(int(s))` and a per-slot `CollapsingHeader` labelled from `MeshMetadata::SubmeshInfo::name`. The preset buttons (`:208-250`), colour edit (`:262`), metallic/roughness/AO sliders (`:278`/`:287`/`:296`) and emission controls (`:313`/`:321`) all become `slots[s].…`. |
| `InspectorPanel.cpp:184-186, 191` (material copy/paste) and `EditorState.h:33 copiedMaterial` | becomes `MaterialDesc` — copy/paste is per-slot |
| `SceneHierarchyPanel.cpp:53` `GetOrAddComponent<MaterialComponent>()` | see §5.5 |
| `AssetsPanel.cpp:435-443` (the `MeshMetadata` info block) | add rows for `materialSlotCount` and `submeshes.size()` next to `"Triangle Count"` (`:439`) |

### 5.5 Slot-count reconciliation — **three** entry points, not one

When an entity's mesh changes, `MaterialComponent::slots` must be resized to the new `MeshMetadata::materialSlotCount`, preserving overlapping entries and seeding new ones from `metadata->importedMaterials`. Put the logic in one free function so all three call sites share it:

```cpp
// X3-Editor/src/Panels/.../MaterialReconcile.h  (or a small shared editor util TU)
void ReconcileMaterialSlots(EntityHandle& entity, const AssetPool& pool);
```

Call it from all three:

1. **`InspectorPanel.cpp:139-150`** — the primitive-mesh `ImGui::Combo`; `meshComponent.guid` is written at `:142` (cleared to `INVALID`) and `:146` (set to a primitive).
2. **`InspectorPanel.cpp:157-170`** — the `DragDropWidget` callback; `meshComponent.guid = payload.guid;` at `:163`.
3. **`SceneHierarchyPanel.cpp:53`** — `entity.GetOrAddComponent<MaterialComponent>();` in the "Add → Mesh" menu item (`:50-54`). Here the freshly added `MeshComponent` has `guid == INVALID`, so reconciliation is a no-op *today*; call it anyway so the site does not silently rot when the menu item starts assigning a default mesh.

`Renderer::Parse` must **not** do this — it takes `const Scene*`. Its `max(1u, materialSlotCount)` clamp in §5.3 is the safety net that keeps a stale component from producing out-of-range material indices.

---

## 6. Texture management — written against the resource layer

Everything in this section targets `VulkanBuffer` / `VulkanTexture` / `VulkanDescriptorSetLayout` / `DescriptorWriter` from Part 3. `IShaderStorageBuffer`, `ITexture2D`, `VulkanComputeShader`, `VulkanTexture2D` and `VulkanContext`'s global binding registry do not exist at this point; do not write code against them.

### 6.1 Binding model: a fixed-size array of combined image samplers

Rejected alternatives:
- **`sampler2DArray`** — requires every layer to share dimensions and format. Real material textures are heterogeneous. No.
- **Full bindless** (`VARIABLE_DESCRIPTOR_COUNT` + `UPDATE_AFTER_BIND` + `PARTIALLY_BOUND`) — more feature surface than needed and the weakest part of MoltenVK's descriptor-indexing support. `ENGINE_PLAN.md` commits to continuous macOS testing; do not take that risk for a win this phase does not need.

**Chosen:** `layout(SET(0) binding = 2) uniform sampler2D u_MaterialTextures[X3_MAX_MATERIAL_TEXTURES];` with `X3_MAX_MATERIAL_TEXTURES = 128`, **every slot always written** (unused slots point at a shared 1×1 dummy), indexed with `nonuniformEXT()` because the material index is divergent across lanes in a path tracer.

**Why this does not need redoing in Phase 7:** Forward+ wants exactly the same thing — one bound texture table, index sourced from the material struct. Phase 9's BC7 changes the *format* of the images, not the binding model. The only thing that would force a change is exceeding 128 textures, at which point bump the constant in **two** places: `GpuTypes.glsl` and `kSet0[]` in `Renderer.cpp` (§6.3).

### 6.2 Device features

Part 2 already sets, in `VulkanContext::pickPhysicalDevice`, `.set_minimum_version(1, 3)`, `.set_required_features_13(features13)` with `dynamicRendering` + `synchronization2`, and `.add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)`. Phase 2 **adds to the same call chain** — it does not replace it:

```cpp
VkPhysicalDeviceFeatures features10{};
features10.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

VkPhysicalDeviceVulkan12Features features12{};
features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
features12.descriptorIndexing                       = VK_TRUE;
features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

selector.set_surface(m_Surface)
    .set_minimum_version(1, 3)
    .set_required_features(features10)          // VkBootstrap.h:667  (merges, does not clobber)
    .set_required_features_12(features12)       // VkBootstrap.h:674
    .set_required_features_13(features13)       // VkBootstrap.h:679  (from Part 2)
    .add_required_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
```

**Verified against the vendored vk-bootstrap:** `set_required_features` calls `detail::merge_VkPhysicalDeviceFeatures(criteria.required_features, features)` (`X3/libs/vk-bootstrap/src/VkBootstrap.cpp:1405-1408`) — it merges, so ordering with respect to any other `set_required_features` call is irrelevant. `set_required_features_12` (`:1419-1425`) and `set_required_features_13` (`:1427-1433`) both funnel into `add_required_extension_features` (`VkBootstrap.h:661-664`), which appends to `criteria.extended_features_chain`; they append **distinct `sType`s** and therefore coexist legally in one `pNext` chain. `createLogicalDevice` needs **no** edit — `DeviceBuilder` reads the selected physical device's chain.

**Not needed, deliberately:** `runtimeDescriptorArray`, `descriptorBindingVariableDescriptorCount`, `descriptorBindingPartiallyBound`, any `*UpdateAfterBind`. Keeping the feature set this small is the entire point of the fixed-size choice. **Do not** hand-build a `VkPhysicalDeviceFeatures2` and pass it via `DeviceBuilder::add_pNext` — vk-bootstrap errors on that combination (`DeviceError::VkPhysicalDeviceFeatures2_in_pNext_chain_while_using_add_required_extension_features`, `VkBootstrap.h:215`).

**Local support confirmed** by `vulkaninfo` on this machine: `descriptorIndexing = true`, `shaderSampledImageArrayDynamicIndexing = true`, `shaderSampledImageArrayNonUniformIndexing = true`, `shaderSampledImageArrayNonUniformIndexingNative = true`, `maxPerStageDescriptorSampledImages = 1048576`, `maxDescriptorSetSampledImages = 1048576`. 128 is not close to any limit.

**macOS check-item:** verify `shaderSampledImageArrayNonUniformIndexing` is reported under MoltenVK on the target machine before relying on it — older MoltenVK requires Metal argument buffers for it. One command: `vulkaninfo | grep -i shaderSampledImageArrayNonUniformIndexing`.

### 6.3 The descriptor table and the array write

**(a) `kSet0[]` in `Renderer.cpp` gains one entry.** The table Part 3 introduced becomes:

```cpp
inline constexpr uint32_t MAX_MATERIAL_TEXTURES = 128;   // MUST equal X3_MAX_MATERIAL_TEXTURES
                                                          // in res/shaders/GpuTypes.glsl

constexpr X3::DescriptorBindingDesc kSet0[] = {
    {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1,                     VK_SHADER_STAGE_COMPUTE_BIT}, // rayTracingTexture
    {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,                     VK_SHADER_STAGE_COMPUTE_BIT}, // skyboxTexture
    {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_MATERIAL_TEXTURES, VK_SHADER_STAGE_COMPUTE_BIT}, // u_MaterialTextures[]
};
```

`DescriptorBindingDesc::count` already exists in Part 3's layer and is already honoured by `VulkanDescriptorSetLayout` when it builds `VkDescriptorSetLayoutBinding::descriptorCount`. Verify that when you get there; if it is being ignored, that is a Part 3 bug to fix, not something to work around here.

**(b) `kSet2[]` gains two entries** for the new SSBOs (§7.2):

```cpp
    {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // TriRefSSBO
    {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}, // VertexSSBO
```
and its binding-3 comment changes from `MeshBuffer` to `TriPositionBuffer`, binding 5 from `IndexBuffer` to `BvhPrimIndexBuffer`.

**(c) `DescriptorWriter` gains an array write.** Part 3's `DescriptorWriter` has no method writing more than one descriptor. Add exactly one:

```cpp
// VulkanDescriptors.h, alongside the existing sampledImage overload
// Writes `textures.size()` consecutive descriptors starting at (binding, arrayElement 0).
// Every element MUST be valid - pass ctx.dummyTexture() for unused slots.
// The VkDescriptorImageInfo block is allocated CONTIGUOUSLY inside m_ImageInfos,
// which is why maxWrites is not a sufficient reservation on its own; see below.
DescriptorWriter& sampledImageArray(uint32_t binding,
                                    std::span<const VulkanTexture* const> textures);
```

Implementation constraints, in order of how easy they are to get wrong:

1. **`m_ImageInfos` must not reallocate.** `VkWriteDescriptorSet::pImageInfo` holds a raw pointer into it. Part 3 reserves `m_ImageInfos` by `maxWrites` (default 16); a 128-element array blows that instantly and every previously-recorded write dangles. Change `DescriptorWriter`'s constructor to `DescriptorWriter(VulkanContext& ctx, VkDescriptorSet dst, uint32_t maxWrites = 16, uint32_t maxImageInfos = 16)` and construct set 0's writer with `maxImageInfos = 2 + MAX_MATERIAL_TEXTURES`. Assert on overflow in every add method — do not silently `push_back`.
2. **The block must be contiguous.** `sampledImageArray` records `const size_t first = m_ImageInfos.size();`, appends all N infos, then pushes one `VkWriteDescriptorSet` with `descriptorCount = N`, `dstArrayElement = 0`, `pImageInfo = &m_ImageInfos[first]`. Because of (1), `&m_ImageInfos[first]` stays valid.
3. **`flush()`'s completeness assert** must count an array binding as satisfied by exactly one `sampledImageArray` call whose `N` equals the layout's `DescriptorBindingDesc::count`. A short write is a hard error, not a warning — a partially-written `COMBINED_IMAGE_SAMPLER` array is undefined behaviour on read even if the shader never indexes the unwritten elements.

**(d) Descriptor pool: the existing sizing is already sufficient — do not shrink it.** `VulkanContext::createDescriptorPool` (`VulkanContext.cpp:460-490`) already sets `maxSets = 1000` (`:480`) and 1000 descriptors of every type (`:463-475`), with `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` (`:479`). Budget after this phase: 3 shaders × 3 sets × `FRAMES_IN_FLIGHT`(2) = **18 sets**, and combined-image-samplers = 3 shaders × 2 frames × (1 skybox + 128 material) = **774**, plus ImGui's pool usage and `ImGui_ImplVulkan_AddTexture` from `ViewportPanel`. 774 is uncomfortably close to 1000. **Raise only `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` (`:465`) from 1000 to 4096. Leave `maxSets` at 1000 — it is already 1000 and lowering it would break ImGui's per-texture allocations.** Under-sizing fails as a `vkAllocateDescriptorSets` error at pipeline creation, which is at least loud.

### 6.4 CPU-side storage: kill the flat buffer

`AssetPool::TextureBuffer` (`AssetManager.h:48`) is a single growing `std::vector<unsigned char>` with `texStartIdx` offsets (`AssetManager.cpp:270-279`). A single 4K RGBA8 image is 64 MB; a handful makes every subsequent `insert()` copy hundreds of megabytes. Replace with:

```cpp
// AssetTypes.h
struct TexturePixels {
    std::vector<unsigned char> data;
    int32_t width = 0, height = 0, channels = 0;
    bool    isSRGB = false;          // decided at import from the texture's ROLE, §6.5
    bool    ownedByModel = false;    // §9.4
};
// AssetManager.h, in AssetPool
std::unordered_map<LR_GUID, TexturePixels> TexturePixelsByGuid;
```

Delete `TextureMetadata::texStartIdx` (`AssetTypes.h:36`) — see the verification note in §1.6. `LoadTexture` (`AssetManager.cpp:250-295`) writes into the map instead of the flat vector; `metadata->width/height/channels` stay as they are (`:277-279`).

### 6.5 Format and colour space — get this right or normal maps are silently wrong

Sampling a normal map or an ORM map through an sRGB view applies an EOTF to data that is not colour. Part 3's `TextureDesc` already carries `VkFormat format` (default `VK_FORMAT_R8G8B8A8_SRGB`), so no API change is needed — only correct call sites:

- baseColor, emissive, skybox → `VK_FORMAT_R8G8B8A8_SRGB`
- normal, metalRough, AO → `VK_FORMAT_R8G8B8A8_UNORM`

The colour space is a property of the texture's **role in a material**, not of the file, which is why `TexturePixels::isSRGB` is set by `ImportMaterial` (§4.5) at the point where the role is known, and why the same image referenced as both baseColor and ORM would need two entries (rare; log a warning if it happens rather than silently picking one).

### 6.6 The texture table

New files `X3/src/Renderer/TextureTable.h/.cpp`, owned by `Renderer` **by value**:

```cpp
namespace X3 {
class TextureTable {
public:
    TextureTable() = default;
    // Creates the 1x1 opaque-white dummy in slot 0. Needs a frame because
    // VulkanTexture's ctor records its upload into frame.cmd().
    void Init(VulkanContext& ctx, const FrameContext& frame);

    // Uploads on first request. Returns Gpu::INVALID_TEXTURE if the GUID has no
    // pixels in the pool (logs a warning once per GUID; never fails the frame).
    uint32_t GetOrCreate(const FrameContext& frame, const AssetPool& pool, LR_GUID guid);

    // MAX_MATERIAL_TEXTURES pointers; unused slots point at slot 0's dummy.
    std::span<const VulkanTexture* const> descriptorView() const;

    // Drops every VulkanTexture (each defer-destroys) and reseeds slot 0.
    void Reset(const FrameContext& frame);
    uint32_t count() const;

private:
    VulkanContext* m_Ctx = nullptr;
    std::vector<VulkanTexture> m_Textures;              // index == table slot
    std::unordered_map<LR_GUID, uint32_t> m_SlotByGuid;
    std::array<const VulkanTexture*, MAX_MATERIAL_TEXTURES> m_View{};
};
}
```

- Slot 0 is a shared 1×1 opaque-white `VK_FORMAT_R8G8B8A8_UNORM` dummy, so an unresolved baseColor multiplies by white and an unresolved ORM multiplies by 1.
- `VulkanTexture` already borrows its sampler from `ctx.getSampler(SamplerDesc)` (Part 3), so 128 textures do **not** produce 128 `VkSampler` objects. That matters: `vulkaninfo` on this machine reports `maxSamplerAllocationCount = 4000`, and MoltenVK is lower. A combined-image-sampler descriptor may legally repeat the same `VkSampler` handle across every array element.
- Table is rebuilt (`Reset` + re-resolve) when `AssetPool::GetUpdateVersion(AssetType::Textures)` changes, following the version-gated pattern already used for the static buffers.
- `MaterialDesc`'s texture GUIDs are resolved to slot indices in `Renderer::Parse` (via `ToGpuMaterial`, §5.3) and written into `Gpu::Material::textures`. Unresolvable GUIDs become `Gpu::INVALID_TEXTURE` (`0xFFFFFFFF`) with a warning, never a hard failure.
- **The skybox stays exactly where it is** — set 0 binding 1, its own `VulkanTexture m_SkyboxTexture` on `Renderer`. It is not a material texture and does not belong in the table.

`Renderer::Draw`'s set-0 writer becomes:

```cpp
DescriptorWriter(*m_Ctx, rings[0].get(frame), /*maxWrites*/ 8,
                 /*maxImageInfos*/ 2 + MAX_MATERIAL_TEXTURES)
    .storageImage(0, out)
    .sampledImage(1, m_SkyboxTexture.valid() ? m_SkyboxTexture : m_Ctx->dummyTexture())
    .sampledImageArray(2, m_TextureTable.descriptorView())
    .flush(m_CurrentShader->setLayout(0));
```

### 6.7 Mips

Required, not optional: Phase 7 needs them regardless, adding them later means reopening the upload path *and* the sampler, and without them a path tracer aliases badly on minified surfaces.

Part 3's `ImageDesc` already has `uint32_t mipLevels`. `TextureDesc` needs the same field added (it does not have one). Then, in `VulkanTexture`'s constructor:

1. `mipLevels = uint32_t(std::floor(std::log2(std::max(w, h)))) + 1`, unless the caller passes 1.
2. `usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT` when `mipLevels > 1` (blit source for the next level).
3. After the level-0 `vkCmdCopyBufferToImage` from `ctx.stage()`, a `vkCmdBlitImage` chain recorded into `frame.cmd()`: for each level *i*, barrier level *i-1* `TRANSFER_DST → TRANSFER_SRC`, blit with `VK_FILTER_LINEAR`, halving extents with `max(1u, dim/2)`.
4. Final barrier transitions **all** levels to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` with `dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` (compute today, fragment from Phase 7).
5. Image view `subresourceRange.levelCount = mipLevels`.
6. **Guard:** `vkGetPhysicalDeviceFormatProperties(...).optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`. If absent, fall back to `mipLevels = 1` and log once.

The shared sampler from `ctx.getSampler` must use `mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR`, `minLod = 0.0f`, `maxLod = VK_LOD_CLAMP_NONE`. `anisotropyEnable = VK_FALSE` — enabling it requires the `samplerAnisotropy` device feature; defer to Phase 7. If Part 3's `SamplerDesc` does not already carry mip fields, add `VkSamplerMipmapMode mipmapMode` and `float maxLod` to it; the `operator==` is already `= default` so the cache key updates for free.

### 6.8 UV origin — one convention, both ends in one commit

`AssetManager.cpp:259` calls `stbi_set_flip_vertically_on_load(1)` with the comment *"OpenGL-style orientation"*. glTF/assimp UVs use a top-left origin (V down), which is also Vulkan's convention. Flipping the image **and** using assimp UVs unchanged gives vertically mirrored textures on every model.

**Decision: stop flipping. Delete `AssetManager.cpp:259`.** Use assimp UVs verbatim.

This inverts the skybox, which today is compensated by the flip. **The paired shader fix must land in the SAME COMMIT, in all three shaders:**

| File:line | From | To |
|---|---|---|
| `X3/res/shaders/PathTracing.comp:207` | `float v = 0.5f + asin(ray.dir.y) * INV_PI;` | `float v = 0.5f - asin(ray.dir.y) * INV_PI;` |
| **`X3/res/shaders/PBR.comp:123`** | `float v = 0.5f + asin(dir.y) * INV_PI;` | `float v = 0.5f - asin(dir.y) * INV_PI;` |
| **`X3/res/shaders/Phong.comp:123`** | `float v = 0.5f + asin(dir.y) * INV_PI;` | `float v = 0.5f - asin(dir.y) * INV_PI;` |

Note the PBR/Phong line numbers are **123**, not 207 — their `GetSkyboxLight` takes a bare `vec3 dir` (`PBR.comp:121-125`, `Phong.comp:121-125`) rather than a `Ray`, and sits far earlier in the file. `PBR.comp` calls it three times (`:396`, `:402`, `:469`), so a mistake there shows up in IBL as well as the background.

Verify visually against `SampleSkyboxes/*.hdr` before moving on. Splitting this across two commits is the single most likely way to lose a day in this phase.

Unrelated but worth one line in the commit message: `.hdr` skyboxes go through `stbi_load` (`AssetManager.cpp:260`), an 8-bit path, so HDR range is being crushed today. Out of scope for Phase 2; note it for Phase 11.

---

## 7. Shader changes

Apply to `PathTracing.comp` first. `PBR.comp` and `Phong.comp` get the identical traversal/resolve changes — their `IntersectTri` (`:139-158`), leaf loop (`:172-179`) and `CheckRayCollision` (`:225-248`) are byte-for-byte copies of PathTracing's, differing only in line offset. Hoist everything shareable into `GpuTypes.glsl` per §3.1 to avoid a fourth divergence.

### 7.1 Preamble

Immediately after `#version 460 core` (`PathTracing.comp:6`, `PBR.comp:4`, `Phong.comp:4`):

```glsl
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "GpuTypes.glsl"
```

Both extensions were verified to compile under `glslc --target-env=vulkan1.3 -I …` (§3.1, §3.3).

### 7.2 New / changed bindings

```glsl
layout(SET(0) binding = 2) uniform sampler2D u_MaterialTextures[X3_MAX_MATERIAL_TEXTURES]; // NEW

layout(std430, SET(2) binding = 3) readonly buffer TriPositionSSBO {   // was MeshBufferSSBO
    TrianglePositions TriPositionBuffer[];
};
layout(std430, SET(2) binding = 5) readonly buffer BvhPrimIndexSSBO {  // was IndexBufferSSBO
    uint BvhPrimIndex[];
};
layout(std430, SET(2) binding = 7) readonly buffer TriRefSSBO {        // NEW
    TriRef TriRefBuffer[];
};
layout(std430, SET(2) binding = 8) readonly buffer VertexSSBO {        // NEW
    Vertex VertexBuffer[];
};
```

Bindings 7 and 8 in set 2 are free — the current highest is 6 (`PathTracing.comp:127`).

**Corresponding C++ (`Renderer`, resource-layer form):** two new `VulkanBuffer` members alongside the existing `m_MeshBufferSSBO`/`m_NodeBufferSSBO`/`m_IndexBufferSSBO`:

```cpp
VulkanBuffer m_TriPositionSSBO;   // renamed from m_MeshBufferSSBO
VulkanBuffer m_BvhPrimIndexSSBO;  // renamed from m_IndexBufferSSBO
VulkanBuffer m_NodeBufferSSBO;    // unchanged
VulkanBuffer m_TriRefSSBO;        // NEW
VulkanBuffer m_VertexSSBO;        // NEW
VulkanBuffer m_TriIndexSSBO;      // NEW - §8, not read by any compute shader
uint32_t m_PrevTriPosVersion = 0, m_PrevTriRefVersion = 0, m_PrevVertexVersion = 0,
         m_PrevTriIndexVersion = 0, m_PrevNodeVersion = 0, m_PrevBvhPrimVersion = 0;
```

Uploaded in `SetupGPUResources` in the same version-gated pattern Part 3 established for the static buffers, e.g.:

```cpp
{
    const uint32_t v = assetPool->GetUpdateVersion(AssetPool::AssetType::TriRefBuffer);
    if (v != m_PrevTriRefVersion) {
        m_PrevTriRefVersion = v;
        const VkDeviceSize bytes = std::max<VkDeviceSize>(
            sizeof(Gpu::TriRef) * assetPool->TriRefBuffer.size(), sizeof(Gpu::TriRef));
        m_TriRefSSBO.ensureCapacity(frame, bytes);
        if (!assetPool->TriRefBuffer.empty())
            m_TriRefSSBO.upload(frame, assetPool->TriRefBuffer.data(),
                                sizeof(Gpu::TriRef) * assetPool->TriRefBuffer.size());
    }
}
```

The `std::max(..., sizeof(one element))` is the same always-write rule Part 3 applied to the light buffer: an empty asset pool must still produce a valid, non-zero-size buffer so `DescriptorWriter::flush`'s completeness assert passes. Do **not** skip the binding when the buffer is empty.

Then in `Draw`'s set-2 writer add:

```cpp
    .storageBuffer(3, m_TriPositionSSBO)
    .storageBuffer(5, m_BvhPrimIndexSSBO)
    .storageBuffer(7, m_TriRefSSBO)
    .storageBuffer(8, m_VertexSSBO)
```

### 7.3 `Ray` gains hit state

`PathTracing.comp:75-81` (`PBR.comp:66-72`, `Phong.comp:66-72`) becomes:

```glsl
struct Ray {
    vec3  origin;
    float t;
    vec3  dir;
    uint  entityIdx;
    vec3  normalGeom;   // geometric, from cross(E1,E2), world space after resolve
    vec3  normalShade;  // interpolated + normal-mapped, world space
    vec2  bary;         // (u,v) = weights of v1, v2
    uint  triIdxLocal;  // mesh-local triangle index
    uint  materialIdx;
    vec2  uv;
    bool  frontFacing;
};
```

`Ray` has no CPU counterpart, so it stays in the `.comp` files (or, better, move it into `GpuTypes.glsl` too under a clearly-labelled "shader-only" section so all three shaders share it).

### 7.4 `IntersectTri` — record barycentrics and index

`PathTracing.comp:224-243`. Signature gains a parameter:

```glsl
bool IntersectTri(inout Ray r, const TrianglePositions tri, uint meshLocalTriIdx) {
    // :225-239 unchanged - E1, E2, Ng, det, invdet, AO, DAO, t, u, v and both
    // rejection tests are exactly what is needed. Do not recompute anything.
    r.t           = t;
    r.bary        = vec2(u, v);       // NEW - weights of v1 and v2 respectively (§2.3a)
    r.triIdxLocal = meshLocalTriIdx;  // NEW
    r.normalGeom  = normalize(Ng);    // was r.normal (:241)
    return true;
}
```

### 7.5 Leaf loop

`PathTracing.comp:258-265` becomes:

```glsl
for (uint i = 0; i < count; i++) {
    uint triIndex = BvhPrimIndex[entityHandle.rootTriIdx + first + i];
    const TrianglePositions tri = TriPositionBuffer[entityHandle.rootTriIdx + triIndex];
    if (IntersectTri(ray, tri, triIndex)) {
        g_TriIntersectionCount++;
    }
}
```

`ray.materialIdx = entityHandle.materialIdx;` (`:263`) is **deleted**. Note `PBR.comp:172-179` and `Phong.comp:172-179` have the same loop but no `g_TriIntersectionCount++`.

### 7.6 `CheckRayCollision` — carry the winner, resolve once, stop clobbering `t`

`PathTracing.comp:313-338`. Two changes, and the second one fixes a real bug.

**(a) Delete `ray.t = INF_T;` at `:314`.** That line unconditionally overwrites the caller's search limit, which defeats `IsInShadow`'s `shadowRay.t = distToLight - SURFACE_BIAS` (`:345`). The result is still *correct* — the post-hoc test at `:350` catches it — but every shadow ray traverses to the nearest hit instead of early-outing at the light, and `IntersectTri`'s `if (t >= r.t) return false` (`:238`) never prunes. Shadow rays are the majority of `CheckRayCollision` calls (one per light per bounce, `:429-443`). The same clobber exists at `PBR.comp:226` and `Phong.comp:226`.

The new contract: **the caller sets `ray.t` to the search limit before calling; `INF_T` means unbounded.** Consequently:
- `TraceRay` (`:452-483`) must set `ray.t = INF_T;` at the top of the loop body, before `CheckRayCollision(ray)` at `:453`. Note `main()` at `:496-510` never initialises `ray.t` at all today — it works only because `:314` overwrote it. Do not rely on that; set it in the loop.
- `IsInShadow` already sets it correctly (`:345`).

**(b) Bound the local-space ray and carry the winner.** Inside the entity loop, replace `rayLocal.t = INF_T;` (`:322`) with `rayLocal.t = ray.t;`. This is exact, not an approximation: `rayLocal.dir = M⁻¹·d_world` with `d_world` normalised, so `|mat3(M)·(rayLocal.dir · t)| = t` — which is precisely what `:333` computes. Local `t` and world `t` are the same number whenever the world direction is normalised, and it is at every call site (`:361` `-normalize(light.direction.xyz)`; `:381`/`:413` `toLight / distToLight`; `main:504` `normalize(...)`; `TraceRay:477` `normalize(...)`).

Inside the `if (rayLocal.t < ray.t)` block (`:328-336`), additionally carry:

```glsl
ray.t           = rayLocal.t;                 // was length(mat3(model)*(rayLocal.dir*rayLocal.t)) - same value
ray.bary        = rayLocal.bary;
ray.triIdxLocal = rayLocal.triIdxLocal;
ray.entityIdx   = uint(i);
ray.normalGeom  = normalize(mat3(transpose(invTransform)) * rayLocal.normalGeom);
ray.frontFacing = dot(ray.normalGeom, ray.dir) < 0.0;
if (!ray.frontFacing) ray.normalGeom = -ray.normalGeom;
```

**Do not `faceforward` blindly** as `:334` does. Recording `frontFacing` and flipping explicitly is what lets §7.7 flip the *shading* normal consistently with the geometric one rather than independently. (If you prefer to keep `length(mat3(model) * (rayLocal.dir * rayLocal.t))` at `:333` rather than the equality argument above, that is fine and produces the same number — but then keep it in *both* the bounded and unbounded paths.)

Then, **after the entity loop closes** (after `:337`), call `ResolveHit(ray)` once, guarded on `ray.t < INF_T`.

### 7.7 `ResolveHit` — the entire attribute path, in one function

**Index convention, stated so nobody double-adds:** `TriRef::i0/i1/i2` are **GLOBAL indices into `AssetPool::VertexBuffer`**. The importer adds `firstVertexIdx` once, at import time (§4.3 step 5). The shader indexes `VertexBuffer[tr.i0]` **directly and adds nothing**. `MeshMetadata::firstVertexIdx` and `vertexCount` are **CPU-only bookkeeping** — they are never uploaded to the GPU, they are not fields of `Gpu::MeshEntityHandle`, and the shader has no access to them. This costs one add at import and saves one add per hit.

```glsl
Vertex FetchVertex(uint idx) { return VertexBuffer[idx]; }   // sole packing chokepoint (§1.2)

void ResolveHit(inout Ray ray) {
    EntityHandle e = EntityLookupTable[ray.entityIdx];
    mat4 model     = TransformBuffer[e.transformIdx];
    mat3 normalMat = mat3(transpose(inverse(model)));

    TriRef tr = TriRefBuffer[e.rootTriIdx + ray.triIdxLocal];
    ray.materialIdx = e.materialBase + min(tr.materialSlot, e.materialSlotCount - 1u);

    Vertex a = FetchVertex(tr.i0);   // GLOBAL indices - nothing is added
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
    float hand = a.tangent.w;                    // constant across a triangle (§1.3)
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

**Transform rules, stated because they are the classic silent bug:** normals transform by `mat3(transpose(inverse(model)))` (as already done at `PathTracing.comp:330`); **tangents transform by `mat3(model)`**. Under non-uniform scale, using the same matrix for both produces a subtly skewed TBN that only shows up on scaled objects. The same rule was applied CPU-side in §4.3 step 4 for the node transform.

### 7.8 Shading (`PathTracing.comp:452-483`)

Replace the material read at `:459` with:

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
- `:462-463` → `brightness_score += emitted * rayColor;`
- `:466` → `SampleLights(hitPoint + ray.normalGeom * SURFACE_BIAS, ray.normalShade)`
- `:467` → `brightness_score += directLight * albedo * rayColor;`
- `:470` → `rayColor *= albedo;`
- `:473` → `ray.origin = hitPoint + ray.normalGeom * SURFACE_BIAS;`
- `:477` → `ray.dir = normalize(ray.normalShade + RandomDirection(state));`

**Offsetting along the geometric normal while shading along the shading normal is deliberate**: shading-normal offsets self-intersect on grazing normal-mapped surfaces.

`metallic` and `roughness` are sampled and available but not consumed by `PathTracing.comp`'s pure-diffuse loop. That is correct for this phase — BSDF work is Phase 6. `PBR.comp` (`:463` onwards) already consumes `pbrParams` and **should** be wired to the sampled values.

### 7.9 Occlusion fast path

`CheckRayCollision` now calls `ResolveHit`, which is pure waste for occlusion queries. Add a dedicated function rather than a flag on `Ray` — a flag means the resolve branch is still in the shadow-ray's instruction stream:

```glsl
// Returns true on the FIRST hit inside tMax. Never resolves attributes.
bool CheckRayOcclusion(vec3 origin, vec3 dir, float tMax) {
    for (int i = 0; i < u_EntityCount; i++) {
        EntityHandle e = EntityLookupTable[i];
        mat4 invTransform = inverse(TransformBuffer[e.transformIdx]);

        Ray rayLocal;
        rayLocal.t      = tMax;                       // real bound - this is the whole point
        rayLocal.origin = (invTransform * vec4(origin, 1.0)).xyz;
        rayLocal.dir    = (invTransform * vec4(dir, 0.0)).xyz;

        TraverseBVH(rayLocal, e);
        if (rayLocal.t < tMax) return true;           // early out across entities too
    }
    return false;
}
```

`IsInShadow` (`PathTracing.comp:341-351`) becomes:

```glsl
bool IsInShadow(vec3 origin, vec3 dirToLight, float distToLight) {
    return CheckRayOcclusion(origin, dirToLight, distToLight - SURFACE_BIAS);
}
```

Mirror into `PBR.comp:251-260` and `Phong.comp:251-260`. **While you are in `Phong.comp:253`, note that `shadowRay.origin = origin + shadowRay.normal * SURFACE_BIAS;` reads `shadowRay.normal` before anything writes it** — an uninitialised read that the rewrite deletes. `PBR.comp:253` correctly uses the passed-in `normal` parameter; keep that behaviour by having Phong's `IsInShadow` gain a `normal` parameter or by having the caller pre-offset the origin.

This is not optional. It is the single largest performance change in the phase.

### 7.10 Optional, explicitly out of the required path

Ray-cone texture LOD (`textureLod(..., lod)` with `lod` derived from triangle UV area vs world area plus cone spread) removes the aliasing that mip-0 sampling causes on minified surfaces. Mips are generated in §6.7 so the data is there. Do this only after the required path renders correctly, and only if aliasing is visible.

---

## 8. Real vertex and index buffers for Phase 7

`ENGINE_PLAN.md:162` is explicit: *"You will need real vertex and index buffers regardless. Forward+ rasterizes actual geometry through a graphics pipeline. The engine has none today. Build the buffers now in a form both consumers can use."* This section is what discharges that. **Nothing binds these in Phase 2** — the point is that Phase 7 does not have to re-import anything.

### 8.1 The same allocation serves compute and raster

`AssetPool::VertexBuffer` and `AssetPool::TriIndexBuffer` (§1.6) are uploaded into two `VulkanBuffer`s that carry both usages:

| Buffer | Usage flags | Read as |
|---|---|---|
| `m_VertexSSBO` | `STORAGE_BUFFER \| VERTEX_BUFFER \| TRANSFER_DST` | set 2 binding 8 (compute, today) **and** `vkCmdBindVertexBuffers` (Phase 7) |
| `m_TriIndexSSBO` | `STORAGE_BUFFER \| INDEX_BUFFER \| TRANSFER_DST` | `vkCmdBindIndexBuffer(..., VK_INDEX_TYPE_UINT32)` (Phase 7). Not bound to any descriptor in Phase 2 — upload it and leave it. |

Part 3's `VulkanBuffer` constructor takes `BufferKind { Uniform, Storage }`, which cannot express this. **Minimal additive change to Part 3's API** — do not restructure it:

```cpp
// VulkanBuffer.h
VulkanBuffer(VulkanContext& ctx, BufferKind kind, VkDeviceSize size,
             const char* debugName, VkBufferUsageFlags extraUsage = 0);
```
The implementation ORs `extraUsage` into the flags it already computes. Call sites:
```cpp
m_VertexSSBO   = VulkanBuffer(ctx, BufferKind::Storage, 0, "VertexBuffer",
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
m_TriIndexSSBO = VulkanBuffer(ctx, BufferKind::Storage, 0, "TriIndexBuffer",
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
```
`ensureCapacity` must carry `extraUsage` through on reallocation — store it in the member alongside `m_Kind`.

**Do not** build a separate, second copy of the vertex data for raster. One allocation, two usages, is the entire reason the `Vertex` layout was chosen to be raster-friendly in §1.2.

### 8.2 The vertex input state Phase 7 will write

Record this in a comment on `Gpu::Vertex` in `GpuTypes.h`, so Phase 7 does not have to re-derive it:

```cpp
// Phase 7 vertex input (do not build this now, just don't break it):
//   VkVertexInputBindingDescription   { binding=0, stride=48, VK_VERTEX_INPUT_RATE_VERTEX }
//   VkVertexInputAttributeDescription {loc=0, binding=0, VK_FORMAT_R32G32B32A32_SFLOAT, offset= 0} // pos.xyz, uv.x
//   VkVertexInputAttributeDescription {loc=1, binding=0, VK_FORMAT_R32G32B32A32_SFLOAT, offset=16} // nrm.xyz, uv.y
//   VkVertexInputAttributeDescription {loc=2, binding=0, VK_FORMAT_R32G32B32A32_SFLOAT, offset=32} // tan.xyz, handedness
// R32G32B32A32_SFLOAT is a mandatory VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT format - no query needed.
// stride 48 == sizeof(Gpu::Vertex), enforced by the static_assert below.
```

### 8.3 Draw ranges, and why the indices are global

`MeshMetadata::firstIndex` and `indexCount` (§1.6) are exactly the arguments Phase 7 needs:

```
vkCmdDrawIndexed(cmd, metadata->indexCount, 1, metadata->firstIndex, /*vertexOffset=*/0, 0);
```

`vertexOffset` is **0** because `TriIndexBuffer` holds **global** indices into `AssetPool::VertexBuffer`, consistent with `TriRef` (§7.7). Do not switch one of the two to mesh-relative indices — having them disagree is exactly the double-add trap that §7.7 exists to prevent.

Per-submesh draws use `SubmeshInfo`: `firstIndex = 3 * (metadata->firstTriIdx + submesh.firstTriIdx)`, `indexCount = 3 * submesh.triCount`, material `= materialBase + submesh.materialSlot`.

### 8.4 What is explicitly NOT built in Phase 2

- No `VkPipeline` of any kind, no `VkPipelineVertexInputStateCreateInfo` object, no depth buffer, no `VkRenderingAttachmentInfo` for depth. Phase 7 owns all of it.
- No per-submesh draw-call sorting, no indirect draw buffer, no meshlets.
- `m_TriIndexSSBO` is uploaded but not bound anywhere. If that bothers a reviewer, the alternative is deriving it from `TriRefBuffer` at Phase 7 time — which is trivial but means walking the entire asset pool on a code path that will already be complicated. Building it at import costs 12 bytes per triangle of GPU memory and zero complexity.

---

## 9. Migration and compatibility

### 9.1 Mesh data: no migration needed. State this clearly.

Verified from source:
- **`.lrmeta` contains only `{Guid, SourcePath}`.** Struct at `AssetManager.h:87-94`; writer `AssetManager.cpp:25-38`; reader `:50-63`. There is **no cached geometry, no cached BVH, no vertex data** on disk. It is a GUID sidecar, not an asset cache.
- **`.lrproj` contains only `{bootSceneGuid, RenderSettings}`** (`ProjectManager.h:24-29`).
- **Every asset is re-imported from `sourcePath` on every project open** — `LoadAssetPoolFromFolder` (`AssetManager.cpp:128-152`) → `LoadAssetFile` → `LoadMesh`, with a full BVH rebuild each time (`:232`). `ENGINE_PLAN.md:305` calls this out as a Phase 9 problem.
- `ProjectExporter` copies source asset files and rewrites `.lrmeta::sourcePath` to the copied location, and copies `engine_res` including shaders. Exported projects also re-import from source.

**Consequence: the mesh format change is invisible to on-disk project data.** Existing projects open, re-import from their `.fbx`/`.glb`/`.obj` sources under the new importer, and get normals, UVs and tangents for free. **No version bump, no converter, no `.lrmeta` migration. Do not write one.**

The one thing that breaks: a project whose source model files have moved was already broken (`AssetManager.cpp:139-142` warns and skips). Unchanged behaviour.

### 9.2 `.lrscn` scene files: one real format change

`MaterialComponent` becomes a slot vector (§5.4), which changes the serialized shape.

**Add a version key.** `Scene.cpp:146-151` writes `SceneGuid`, `SceneName`, `SkyboxGuid`, `SkyboxName`, `Entities` — no version. Add as the first key at `:147`:

```cpp
<< YAML::Key << "SceneVersion" << YAML::Value << 2
```

On load, read it with default `1` when absent.

**Writer** (`Scene.cpp:206-220`) emits:

```yaml
MaterialComponent:
  Slots:
    - Emission:      [r,g,b,a]
      Color:         [r,g,b,a]
      Metallic:      0.0
      Roughness:     0.5
      AO:            1.0
      NormalScale:   1.0
      BaseColorTex:  0        # LR_GUID as uint64; 0 == none
      NormalTex:     0
      MetalRoughTex: 0
      EmissiveTex:   0
```

**Reader** (`Scene.cpp:465-472`) branches on the presence of the `Slots` key, **not** on `SceneVersion` — more robust against hand-edited files:

```
if (mnode["Slots"] && mnode["Slots"].IsSequence())
    -> read each slot with the existing getVec4/getScalar helpers
else
    -> legacy: read the flat Emission / Color / Metallic / Roughness / AO keys
       (exactly the five reads at :469-472 today) into slots[0];
       NormalScale = 1.0; all four texture GUIDs = LR_GUID::INVALID
```

Keep the legacy branch permanently — it is ten lines and the existing `getScalar`/`getVec4` helpers already default-and-warn. Result: **old `.lrscn` files load unchanged**, their single material landing in slot 0; on first save they are rewritten in the new shape. One-way, and that is fine — say so in the commit message.

### 9.3 Texture GUID references

Materials now reference texture assets by GUID. A `.lrscn` referencing a texture whose `.lrmeta` is missing must **not** fail the scene load: resolve to `Gpu::INVALID_TEXTURE`, `LOG_ENGINE_WARN` with the GUID, render with the factor only. Same policy as the existing missing-mesh path at `Renderer.cpp:163-166` — which silently `continue`s today; upgrade it to a warn while you are there (§5.3).

### 9.4 Textures imported from inside model files

`ImportMaterial` (§4.5) registers textures found inside `.glb`/`.fbx` into the AssetPool under derived GUIDs. Per §0.1 that is every texture in both sample models. These are **not** independent assets with `.lrmeta` sidecars; they are owned by the model. Do **not** write `.lrmeta` files for them: `SaveAssetPoolToFolder` (`AssetManager.cpp:92-125`) iterates every metadata entry and would emit sidecars pointing at nonexistent paths (`"…glb/*0"`), after which `LoadAssetPoolFromFolder` (`:128-152`) would warn on every one, every open. Add `bool ownedByModel` to `MetadataExtension` (`AssetTypes.h:42-48`) and skip those entries in the save loop.

### 9.5 Shader binaries

`X3/res/shaders/*.spv` are covered by `.gitignore:18 *.spv`, so the stale-binary hazard is already handled — no caveat needed. The CMake `add_custom_command` `DEPENDS` change in §3.1 is what makes edits to `GpuTypes.glsl` retrigger compilation; without it a change to the shared struct file silently produces stale SPIR-V, which is the same failure in a new place.

---

## 10. Implementation order

Each step should build and, from step 5 on, run. Do not reorder 1-4.

1. **`GpuTypes.h` + `static_assert`s** (§1.4, §3.2, §3.3 — note the corrected `alignof` asserts). Delete `Triangle`/`Material` from `AssetTypes.h`; delete `MeshEntityHandle`/`LightData` from `Renderer.h`. Rename `Triangle` → `Gpu::TrianglePositions` across `BVHAccel.h/.cpp`, `AssetManager.h/.cpp`, `Renderer.cpp`, `PhysicsWorld.cpp:1035,1097`. **Pure rename + move. No behaviour change.** Verify the engine renders identically.
2. **`GpuTypes.glsl` + CMake `-I` and `--target-env=vulkan1.3`** (§3.1). Delete the struct blocks from all three `.comp` files. **Output must be pixel-identical.** Confirm by diffing a rendered frame. Confirm the `.spv` header now reads `Version: 1.6` (`spirv-dis`).
3. **AssetPool buffer renames** (§1.6): `MeshBuffer` → `TriPositionBuffer`, `IndexBuffer` → `BvhPrimIndexBuffer`, the matching `AssetType` enumerators, the `Renderer` member names, the `kSet2[]` comments, and the GLSL identifiers. Still pixel-identical.
4. **`MeshUtils::ComputeTangents` / `ComputeNormals`** (§4.4) — standalone, unused yet.
5. **Importer rewrite** (§4.2, §4.3): node walk, two-pass **emitted**-triangle sizing, vertex/TriRef/TriPosition/TriIndex emission, submesh table. Do **not** import materials or textures yet; the renderer still ignores the new buffers. **Verify:** BVH node counts unchanged for both sample models, and imported vertex positions within ~1e-6 of the pre-change values (the node matrices nearly cancel — §0.1).
6. **Primitive regeneration** (§4.6).
7. **New SSBOs + `ResolveHit` + smooth normals** (§7.2-7.7, minus the normal-mapping block). **This is the first visible change: flat shading becomes smooth.** Stop and confirm before continuing.
8. **Occlusion fast path + the `ray.t = INF_T` fix** (§7.6a, §7.9), mirrored into all three shaders. Measurable speedup in any scene with lights; no visual change.
9. **Per-submesh materials** (§5): `MeshEntityHandle` 24→32 B, `materialBase`/`materialSlotCount`, `MaterialComponent` slot vector, `.lrscn` versioned (de)serialization (§9.2), inspector UI, all three reconciliation entry points (§5.5). No textures yet.
10. **Vulkan texture-array plumbing** (§6.2, §6.3): device features, `kSet0` entry, `DescriptorWriter::sampledImageArray` + the `maxImageInfos` reservation, `COMBINED_IMAGE_SAMPLER` pool count → 4096. Bind a table full of dummy textures and confirm validation is clean under load before any real texture exists.
11. **Texture upload path** (§6.4, §6.5, §6.6 mips, sampler): per-asset pixel storage, role-driven format, mip chain. **Remove `stbi_set_flip_vertically_on_load` and fix the skybox `v` in all three shaders in the same commit** (§6.8) — verify the skybox immediately.
12. **Material and texture import** (§4.5) including embedded `.glb` textures and dedup, plus `TextureTable` (§6.6).
13. **Shader sampling + normal mapping** (§7.7 tangent block, §7.8), mirrored into `PBR.comp` and `Phong.comp`.
14. **Phase 7 buffer usage flags** (§8.1) — `extraUsage` on `VulkanBuffer`, `VERTEX_BUFFER`/`INDEX_BUFFER` bits, `TriIndexBuffer` upload. Purely additive; nothing consumes it yet.

**Exit criteria** (`ENGINE_PLAN.md:170`): `SampleModels/stanford_dragon_pbr.glb` renders smooth-shaded, with its embedded base-colour and normal maps applied, in the path-traced view; a multi-material model shows distinct materials per submesh; validation layers (including synchronization validation) clean across a resolution change, a shader-type switch, a skybox change, a zero-light scene, and accumulate on/off.

---

## 11. Explicitly out of scope

- Graphics pipeline creation, vertex input state objects, depth attachments (Phase 7). §8 builds the *buffers* in a bindable form and records the intended vertex input layout in a comment; it creates no pipeline.
- Texture compression / BC7, meshoptimizer, serialized BVH (Phase 9).
- Attribute quantization (§1.2 — deliberate deferral to Phase 9, behind `FetchVertex`).
- Any BSDF work. `metallic`/`roughness` are sampled and plumbed; consuming them properly is Phase 6.
- Lightmap UV channel (UV1). The `Vertex` layout has no room; adding it is a Phase 10 concern requiring a second attribute stream or a wider vertex. Flag it as known; do not pre-build it.
- Alpha cutout / two-sided material flags. `Material` has room (`color.w`, and unused bits in `textures`) but nothing consumes them.
- Slang. §3.1 and §3.2 happen to make Phase 3's job easier; do not restructure anything *for* Slang beyond that.
- HDR skybox pipeline (`.hdr` currently goes through the 8-bit `stbi_load` path, `AssetManager.cpp:260`) — noted in §6.8, owned by Phase 11.