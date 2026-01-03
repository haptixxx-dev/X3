#include "MaterialAsset.h"
#include "Core/Log.h"

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/XmlIo.h>

namespace mx = MaterialX;

namespace X3
{
	// ============================================================================
	// MaterialAsset Implementation
	// ============================================================================

	Material MaterialAsset::ToGPUMaterial() const
	{
		Material mat;
		mat.emission = glm::vec4(emissionColor, emissionStrength);
		mat.color = baseColor;
		mat.pbrParams = glm::vec4(metallic, roughness, ao, 0.0f);

		// Texture indices are set externally by the renderer after resolving GUIDs
		mat.albedoTexIdx = -1;
		mat.normalTexIdx = -1;
		mat.metallicTexIdx = -1;
		mat.roughnessTexIdx = -1;
		mat.aoTexIdx = -1;
		mat.emissionTexIdx = -1;

		return mat;
	}

	MaterialAsset MaterialAsset::FromComponent(
		const glm::vec4& color,
		const glm::vec4& emission,
		float metallic,
		float roughness,
		float ao)
	{
		MaterialAsset asset;
		asset.baseColor = color;
		asset.emissionColor = glm::vec3(emission);
		asset.emissionStrength = emission.w;
		asset.metallic = metallic;
		asset.roughness = roughness;
		asset.ao = ao;
		return asset;
	}

	// ============================================================================
	// Helper: Extract float value from MaterialX input
	// ============================================================================

	static float GetInputFloat(mx::NodePtr node, const std::string& inputName, float defaultValue)
	{
		if (auto input = node->getInput(inputName))
		{
			if (input->hasValue())
			{
				return input->getValue()->asA<float>();
			}
		}
		return defaultValue;
	}

	static mx::Color3 GetInputColor3(mx::NodePtr node, const std::string& inputName, mx::Color3 defaultValue)
	{
		if (auto input = node->getInput(inputName))
		{
			if (input->hasValue())
			{
				return input->getValue()->asA<mx::Color3>();
			}
		}
		return defaultValue;
	}

	static std::string GetInputTexturePath(mx::NodePtr node, const std::string& inputName)
	{
		if (auto input = node->getInput(inputName))
		{
			// Check if connected to an image node
			if (input->hasNodeName())
			{
				auto graph = node->getParent();
				if (auto imageNode = graph->getChildOfType<mx::Node>(input->getNodeName()))
				{
					if (imageNode->getCategory() == "image" || imageNode->getCategory() == "tiledimage")
					{
						if (auto fileInput = imageNode->getInput("file"))
						{
							if (fileInput->hasValueString())
							{
								return fileInput->getValueString();
							}
						}
					}
				}
			}
		}
		return "";
	}

	// ============================================================================
	// Load MaterialAsset from .mtlx
	// ============================================================================

	std::optional<MaterialAsset> LoadMaterialAsset(const std::filesystem::path& filepath)
	{
		if (!std::filesystem::exists(filepath))
		{
			LOG_ENGINE_ERROR("MaterialAsset file not found: {}", filepath.string());
			return std::nullopt;
		}

		try
		{
			// Create and read document
			mx::DocumentPtr doc = mx::createDocument();
			mx::readFromXmlFile(doc, filepath.string());

			MaterialAsset asset;
			asset.name = filepath.stem().string();

			// Look for standard_surface or open_pbr_surface nodes
			auto nodes = doc->getNodes();
			mx::NodePtr surfaceNode = nullptr;

			for (const auto& node : nodes)
			{
				const std::string& category = node->getCategory();
				if (category == "standard_surface" ||
				    category == "open_pbr_surface" ||
				    category == "gltf_pbr")
				{
					surfaceNode = node;
					break;
				}
			}

			// Also search in nodegraphs
			if (!surfaceNode)
			{
				for (const auto& nodeGraph : doc->getNodeGraphs())
				{
					for (const auto& node : nodeGraph->getNodes())
					{
						const std::string& category = node->getCategory();
						if (category == "standard_surface" ||
						    category == "open_pbr_surface" ||
						    category == "gltf_pbr")
						{
							surfaceNode = node;
							break;
						}
					}
					if (surfaceNode) break;
				}
			}

			if (!surfaceNode)
			{
				LOG_ENGINE_WARN("No PBR surface node found in MaterialX file: {}", filepath.string());
				// Return default material
				return asset;
			}

			const std::string& category = surfaceNode->getCategory();

			// Parse based on shader type
			if (category == "standard_surface")
			{
				// AutoDesk Standard Surface parameters
				auto baseColor = GetInputColor3(surfaceNode, "base_color", mx::Color3(1.0f, 1.0f, 1.0f));
				asset.baseColor = glm::vec4(baseColor[0], baseColor[1], baseColor[2], 1.0f);
				asset.metallic = GetInputFloat(surfaceNode, "metalness", 0.0f);
				asset.roughness = GetInputFloat(surfaceNode, "specular_roughness", 0.5f);

				// Emission
				auto emissionColor = GetInputColor3(surfaceNode, "emission_color", mx::Color3(0.0f, 0.0f, 0.0f));
				asset.emissionColor = glm::vec3(emissionColor[0], emissionColor[1], emissionColor[2]);
				asset.emissionStrength = GetInputFloat(surfaceNode, "emission", 0.0f);

				// Texture paths
				asset.albedoTexPath = GetInputTexturePath(surfaceNode, "base_color");
				asset.normalTexPath = GetInputTexturePath(surfaceNode, "normal");
				asset.metallicTexPath = GetInputTexturePath(surfaceNode, "metalness");
				asset.roughnessTexPath = GetInputTexturePath(surfaceNode, "specular_roughness");
				asset.aoTexPath = GetInputTexturePath(surfaceNode, "coat"); // AO often in coat or separate
				asset.emissionTexPath = GetInputTexturePath(surfaceNode, "emission_color");
			}
			else if (category == "gltf_pbr")
			{
				// glTF PBR parameters
				auto baseColor = GetInputColor3(surfaceNode, "base_color", mx::Color3(1.0f, 1.0f, 1.0f));
				asset.baseColor = glm::vec4(baseColor[0], baseColor[1], baseColor[2], 1.0f);
				asset.metallic = GetInputFloat(surfaceNode, "metallic", 0.0f);
				asset.roughness = GetInputFloat(surfaceNode, "roughness", 0.5f);

				auto emissive = GetInputColor3(surfaceNode, "emissive", mx::Color3(0.0f, 0.0f, 0.0f));
				asset.emissionColor = glm::vec3(emissive[0], emissive[1], emissive[2]);
				asset.emissionStrength = glm::length(asset.emissionColor) > 0.0f ? 1.0f : 0.0f;

				asset.albedoTexPath = GetInputTexturePath(surfaceNode, "base_color");
				asset.normalTexPath = GetInputTexturePath(surfaceNode, "normal");
				asset.metallicTexPath = GetInputTexturePath(surfaceNode, "metallic");
				asset.roughnessTexPath = GetInputTexturePath(surfaceNode, "roughness");
				asset.aoTexPath = GetInputTexturePath(surfaceNode, "occlusion");
				asset.emissionTexPath = GetInputTexturePath(surfaceNode, "emissive");
			}
			else if (category == "open_pbr_surface")
			{
				// OpenPBR parameters
				auto baseColor = GetInputColor3(surfaceNode, "base_color", mx::Color3(1.0f, 1.0f, 1.0f));
				asset.baseColor = glm::vec4(baseColor[0], baseColor[1], baseColor[2], 1.0f);
				asset.metallic = GetInputFloat(surfaceNode, "base_metalness", 0.0f);
				asset.roughness = GetInputFloat(surfaceNode, "specular_roughness", 0.5f);

				auto emissionColor = GetInputColor3(surfaceNode, "emission_color", mx::Color3(0.0f, 0.0f, 0.0f));
				asset.emissionColor = glm::vec3(emissionColor[0], emissionColor[1], emissionColor[2]);
				asset.emissionStrength = GetInputFloat(surfaceNode, "emission_luminance", 0.0f);

				asset.albedoTexPath = GetInputTexturePath(surfaceNode, "base_color");
				asset.normalTexPath = GetInputTexturePath(surfaceNode, "geometry_normal");
				asset.metallicTexPath = GetInputTexturePath(surfaceNode, "base_metalness");
				asset.roughnessTexPath = GetInputTexturePath(surfaceNode, "specular_roughness");
			}

			LOG_ENGINE_INFO("Loaded MaterialX asset: {} (shader: {})", asset.name, category);
			return asset;
		}
		catch (const mx::ExceptionParseError& e)
		{
			LOG_ENGINE_ERROR("Failed to parse MaterialX file {}: {}", filepath.string(), e.what());
			return std::nullopt;
		}
		catch (const mx::ExceptionFileMissing& e)
		{
			LOG_ENGINE_ERROR("MaterialX file missing: {}", e.what());
			return std::nullopt;
		}
		catch (const std::exception& e)
		{
			LOG_ENGINE_ERROR("Error loading MaterialX file {}: {}", filepath.string(), e.what());
			return std::nullopt;
		}
	}

	// ============================================================================
	// Save MaterialAsset to .mtlx
	// ============================================================================

	std::string CreateDefaultMaterialXDocument(const MaterialAsset& asset)
	{
		// Create MaterialX document using standard_surface (most compatible)
		std::ostringstream ss;
		ss << R"(<?xml version="1.0"?>)" << "\n";
		ss << R"(<materialx version="1.39">)" << "\n";

		// Create a nodegraph for the material
		ss << R"(  <nodegraph name=")" << asset.name << R"(_graph">)" << "\n";

		// Add image nodes for textures if paths are specified
		if (!asset.albedoTexPath.empty())
		{
			ss << R"(    <image name="albedo_tex" type="color3">)" << "\n";
			ss << R"(      <input name="file" type="filename" value=")" << asset.albedoTexPath << R"("/>)" << "\n";
			ss << R"(    </image>)" << "\n";
		}
		if (!asset.normalTexPath.empty())
		{
			ss << R"(    <image name="normal_tex" type="vector3">)" << "\n";
			ss << R"(      <input name="file" type="filename" value=")" << asset.normalTexPath << R"("/>)" << "\n";
			ss << R"(    </image>)" << "\n";
		}
		if (!asset.metallicTexPath.empty())
		{
			ss << R"(    <image name="metallic_tex" type="float">)" << "\n";
			ss << R"(      <input name="file" type="filename" value=")" << asset.metallicTexPath << R"("/>)" << "\n";
			ss << R"(    </image>)" << "\n";
		}
		if (!asset.roughnessTexPath.empty())
		{
			ss << R"(    <image name="roughness_tex" type="float">)" << "\n";
			ss << R"(      <input name="file" type="filename" value=")" << asset.roughnessTexPath << R"("/>)" << "\n";
			ss << R"(    </image>)" << "\n";
		}
		if (!asset.aoTexPath.empty())
		{
			ss << R"(    <image name="ao_tex" type="float">)" << "\n";
			ss << R"(      <input name="file" type="filename" value=")" << asset.aoTexPath << R"("/>)" << "\n";
			ss << R"(    </image>)" << "\n";
		}
		if (!asset.emissionTexPath.empty())
		{
			ss << R"(    <image name="emission_tex" type="color3">)" << "\n";
			ss << R"(      <input name="file" type="filename" value=")" << asset.emissionTexPath << R"("/>)" << "\n";
			ss << R"(    </image>)" << "\n";
		}

		// Standard Surface shader node
		ss << R"(    <standard_surface name=")" << asset.name << R"(" type="surfaceshader">)" << "\n";

		// Base color
		if (!asset.albedoTexPath.empty())
		{
			ss << R"(      <input name="base_color" type="color3" nodename="albedo_tex"/>)" << "\n";
		}
		else
		{
			ss << R"(      <input name="base_color" type="color3" value=")"
			   << asset.baseColor.r << ", " << asset.baseColor.g << ", " << asset.baseColor.b << R"("/>)" << "\n";
		}
		ss << R"(      <input name="base" type="float" value="1.0"/>)" << "\n";

		// Metalness
		if (!asset.metallicTexPath.empty())
		{
			ss << R"(      <input name="metalness" type="float" nodename="metallic_tex"/>)" << "\n";
		}
		else
		{
			ss << R"(      <input name="metalness" type="float" value=")" << asset.metallic << R"("/>)" << "\n";
		}

		// Roughness
		if (!asset.roughnessTexPath.empty())
		{
			ss << R"(      <input name="specular_roughness" type="float" nodename="roughness_tex"/>)" << "\n";
		}
		else
		{
			ss << R"(      <input name="specular_roughness" type="float" value=")" << asset.roughness << R"("/>)" << "\n";
		}

		// Normal map
		if (!asset.normalTexPath.empty())
		{
			ss << R"(      <input name="normal" type="vector3" nodename="normal_tex"/>)" << "\n";
		}

		// Emission
		if (!asset.emissionTexPath.empty())
		{
			ss << R"(      <input name="emission_color" type="color3" nodename="emission_tex"/>)" << "\n";
		}
		else if (asset.emissionStrength > 0.0f)
		{
			ss << R"(      <input name="emission_color" type="color3" value=")"
			   << asset.emissionColor.r << ", " << asset.emissionColor.g << ", " << asset.emissionColor.b << R"("/>)" << "\n";
		}
		ss << R"(      <input name="emission" type="float" value=")" << asset.emissionStrength << R"("/>)" << "\n";

		// Specular settings
		ss << R"(      <input name="specular" type="float" value="1.0"/>)" << "\n";
		ss << R"(      <input name="specular_IOR" type="float" value="1.5"/>)" << "\n";

		// Output
		ss << R"(      <output name="out" type="surfaceshader"/>)" << "\n";
		ss << R"(    </standard_surface>)" << "\n";

		// Surface material output
		ss << R"(    <output name="surface_output" type="surfaceshader" nodename=")" << asset.name << R"("/>)" << "\n";
		ss << R"(  </nodegraph>)" << "\n";

		// Create a material that references the nodegraph
		ss << R"(  <surfacematerial name=")" << asset.name << R"(_material" type="material">)" << "\n";
		ss << R"(    <input name="surfaceshader" type="surfaceshader" nodegraph=")" << asset.name << R"(_graph" output="surface_output"/>)" << "\n";
		ss << R"(  </surfacematerial>)" << "\n";

		ss << R"(</materialx>)" << "\n";
		return ss.str();
	}

	bool SaveMaterialAsset(const std::filesystem::path& filepath, const MaterialAsset& asset)
	{
		try
		{
			std::string xmlContent = CreateDefaultMaterialXDocument(asset);

			// Write to file
			std::ofstream file(filepath);
			if (!file.is_open())
			{
				LOG_ENGINE_ERROR("Failed to open file for writing: {}", filepath.string());
				return false;
			}

			file << xmlContent;
			file.close();

			LOG_ENGINE_INFO("Saved MaterialX asset: {}", filepath.string());
			return true;
		}
		catch (const std::exception& e)
		{
			LOG_ENGINE_ERROR("Error saving MaterialX file {}: {}", filepath.string(), e.what());
			return false;
		}
	}

} // namespace X3
