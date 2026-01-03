#pragma once

#include "lrpch.h"
#include "Core/GUID.h"
#include "Project/Assets/AssetTypes.h"

#include <filesystem>
#include <optional>

namespace X3
{
	// ============================================================================
	// MATERIAL ASSET
	// ----------------------------------------------------------------------------
	// Represents a PBR material that can be saved/loaded as MaterialX (.mtlx).
	// Maps to the GPU Material struct but with LR_GUIDs for texture references.
	// ============================================================================

	struct MaterialAsset
	{
		LR_GUID guid = LR_GUID::INVALID;
		std::string name = "Untitled Material";

		// Base color / albedo
		glm::vec4 baseColor = { 1.0f, 1.0f, 1.0f, 1.0f };

		// Emission
		glm::vec3 emissionColor = { 0.0f, 0.0f, 0.0f };
		float emissionStrength = 0.0f;

		// PBR parameters
		float metallic = 0.0f;    // 0.0 = dielectric, 1.0 = metal
		float roughness = 0.5f;   // 0.0 = smooth/mirror, 1.0 = rough/diffuse
		float ao = 1.0f;          // Ambient occlusion multiplier

		// Texture references (LR_GUID::INVALID = use scalar value above)
		LR_GUID albedoTexGuid     = LR_GUID::INVALID;
		LR_GUID normalTexGuid     = LR_GUID::INVALID;
		LR_GUID metallicTexGuid   = LR_GUID::INVALID;
		LR_GUID roughnessTexGuid  = LR_GUID::INVALID;
		LR_GUID aoTexGuid         = LR_GUID::INVALID;
		LR_GUID emissionTexGuid   = LR_GUID::INVALID;

		// Texture file paths (for MaterialX export - paths relative to asset folder)
		std::string albedoTexPath;
		std::string normalTexPath;
		std::string metallicTexPath;
		std::string roughnessTexPath;
		std::string aoTexPath;
		std::string emissionTexPath;

		// Convert to GPU-ready Material struct (texture indices resolved separately)
		Material ToGPUMaterial() const;

		// Create from MaterialComponent values
		static MaterialAsset FromComponent(
			const glm::vec4& color,
			const glm::vec4& emission,
			float metallic,
			float roughness,
			float ao
		);
	};

	// ============================================================================
	// MATERIAL METADATA (for AssetPool registration)
	// ============================================================================

	struct MaterialMetadata : public Metadata
	{
		MaterialAsset asset;
		~MaterialMetadata() override = default;
	};

	struct MaterialMetadataExtension : public MetadataExtension
	{
		~MaterialMetadataExtension() override = default;
	};

	// ============================================================================
	// MATERIAL ASSET I/O FUNCTIONS
	// ============================================================================

	/// Supported material file extension
	constexpr const char* MATERIAL_FILE_EXTENSION = ".mtlx";

	/// Save a MaterialAsset as a MaterialX (.mtlx) file.
	/// Uses the OpenPBR/standard_surface shading model for compatibility.
	/// @param filepath The full path to save the .mtlx file.
	/// @param asset The MaterialAsset to serialize.
	/// @return True on success, false on failure.
	bool SaveMaterialAsset(const std::filesystem::path& filepath, const MaterialAsset& asset);

	/// Load a MaterialAsset from a MaterialX (.mtlx) file.
	/// Parses standard_surface nodes and extracts PBR parameters.
	/// @param filepath The full path to the .mtlx file.
	/// @return The loaded MaterialAsset, or std::nullopt on failure.
	std::optional<MaterialAsset> LoadMaterialAsset(const std::filesystem::path& filepath);

	/// Create a default MaterialX document string for a PBR material.
	/// Useful for debugging or creating new materials programmatically.
	std::string CreateDefaultMaterialXDocument(const MaterialAsset& asset);

} // namespace X3
