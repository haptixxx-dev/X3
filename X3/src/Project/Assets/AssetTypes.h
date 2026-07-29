#pragma once

#include "lrpch.h"
#include "Core/GUID.h"
#include "Project/Assets/MaterialDesc.h"
#include "Renderer/GpuTypes.h"
#include <filesystem>
#include <string>
#include <vector>

namespace X3
{

	// `Triangle` and `Material` used to live here. Phase 2 moved every
	// GPU-mirrored struct into Renderer/GpuTypes.h so they could carry
	// static_asserts and be mirrored from one place; they are now
	// Gpu::TrianglePositions and Gpu::Material.

	struct Metadata {
        virtual ~Metadata() = default;
    };

    // One contiguous run of an imported mesh inside the AssetPool buffers. A
    // submesh is an assimp aiMesh instance: one material, one range of
    // triangles. Its firstTriIdx is MESH-LOCAL (relative to
    // MeshMetadata::firstTriIdx), because that is the form the shader's
    // per-entity indexing wants.
    //
    // CPU-only -- the GPU learns a triangle's material from TriRef::materialSlot,
    // not from this table. It exists for the editor UI (naming the slots) and
    // for Phase 7's per-submesh draw ranges.
    struct SubmeshInfo {
        uint32_t    firstTriIdx  = 0;
        uint32_t    triCount     = 0;
        uint32_t    materialSlot = 0;   // dense, 0..materialSlotCount-1
        std::string name;               // aiMesh::mName
    };

    struct MeshMetadata : public Metadata {
        uint32_t firstTriIdx    = 0;
        uint32_t TriCount       = 0;
        uint32_t firstNodeIdx   = 0;
        uint32_t nodeCount      = 0;
        uint32_t firstVertexIdx = 0;
        uint32_t vertexCount    = 0;
        uint32_t materialSlotCount = 0;

        std::vector<SubmeshInfo> submeshes;
        // One per material slot, imported from the model file's aiMaterials.
        // Renderer::Parse falls back to these when the entity has no
        // MaterialComponent override for that slot. Textures are held as GUIDs
        // and resolved to table indices there, by the same code that resolves a
        // MaterialComponent's -- there is no second conversion path.
        std::vector<MaterialDesc> importedMaterials;

        ~MeshMetadata() override = default;
    };

    struct TextureMetadata : public Metadata {
        int32_t  width       = 0;
        int32_t  height      = 0;
        int32_t  channels    = 0;
        // sRGB for base colour and emissive; linear for normal / ORM data.
        // Sampling a normal map through an sRGB view applies an EOTF to data
        // that is not colour, which is silently wrong rather than obviously so.
        bool     isSRGB      = true;
        ~TextureMetadata() override = default;
    };

    // extensions with additional metadata of assets
    // renderer is not fed these
    struct MetadataExtension {
        float loadTimeMs = -1;
        std::filesystem::path sourcePath = "";
        uintmax_t fileSizeInBytes = 0;

        // True for textures unpacked from INSIDE a model file (.glb/.fbx
        // embedded images). They are owned by the model, not independent assets,
        // and must be skipped by SaveAssetPoolToFolder -- writing a .lrmeta for
        // one would point at a path that does not exist, and the next
        // LoadAssetPoolFromFolder would warn on every single one.
        bool ownedByModel = false;

        virtual ~MetadataExtension() = default;
    };

    struct MeshMetadataExtension : MetadataExtension {
        /* additional mesh specific fields ... */
        ~MeshMetadataExtension() override = default;
    };

    struct TextureMetadataExtension : MetadataExtension {
        /* additional texture specific fields ... */
        ~TextureMetadataExtension() override = default;
    };
}