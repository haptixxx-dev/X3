#pragma once

#include "lrpch.h"
#include "Core/GUID.h"
#include <filesystem>

namespace Laura
{

	// According to std430 - 48 bytes
	// For simplicity - storing one extra padding float per vertex ( not ideal but might use the space for some other data in the future)
	struct Triangle {
		glm::vec4 v0 = {}, v1 = {}, v2 = {};
	};

    struct Material { // default - bright green
        glm::vec4 emission = { 0.0f, 1.0f, 0.0f, 1.0f };
        glm::vec4 color = { 0.0f, 0.0f, 0.0f, 1.0f };
    };

	struct Metadata {
        virtual ~Metadata() = default;
    };

    struct MeshMetadata : public Metadata {
        uint32_t firstTriIdx  = 0;
        uint32_t TriCount     = 0;
        uint32_t firstNodeIdx = 0;
        uint32_t nodeCount    = 0;
        ~MeshMetadata() override = default;
    };

    struct TextureMetadata : public Metadata {
        uint32_t texStartIdx = 0;
        int32_t  width       = 0;
        int32_t  height      = 0;
        int32_t  channels    = 0;
        ~TextureMetadata() override = default;
    };

    // extensions with additional metadata of assets
    // renderer is not fed these
    struct MetadataExtension {
        float loadTimeMs = -1;
        std::filesystem::path sourcePath = "";
        uintmax_t fileSizeInBytes = 0;
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