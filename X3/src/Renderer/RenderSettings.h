#pragma once

#include "lrpch.h"
#include <yaml-cpp/yaml.h>

namespace X3
{
	enum class ShaderType {
		PATH_TRACING = 0,
		PHONG = 1,
		PBR = 2,
		/// Numeric BSDF validation, not a way to draw a scene. It ignores the
		/// camera and the scene entirely and writes a table of directional
		/// albedos -- see res/shaders/FurnaceTest.slang. Deliberately NOT offered
		/// in the editor's shader dropdown; X3RenderTest selects it.
		FURNACE_TEST = 3,
		/// Bakes the BSDF energy LUT. Not a way to draw a scene either -- the
		/// Renderer dispatches it once, before the first shading pass.
		BSDF_LUT_BAKE = 4,
		/// Clustered Forward+ light culling, in two passes. Neither draws
		/// anything and neither is offered in the shader dropdown: the Renderer
		/// dispatches both every frame ahead of shading.
		CLUSTER_BUILD = 5,
		LIGHT_CULL = 6,
		/// Fills the background for the raster path. Not selectable either.
		SKYBOX_FILL = 7,
		/// CLUSTERED FORWARD+ -- the real renderer, and the only entry here that
		/// rasterizes. Selecting it skips the compute shading dispatch entirely
		/// and runs depth prepass -> cluster build -> light cull -> skybox fill
		/// -> forward opaque instead.
		FORWARD = 8
	};

	struct RenderSettings {
        // Editor-only: not meant to be other than default during runtime
        // 0 off, 1 AABB heatmap, 2 triangle heatmap, 3 depth prepass,
        // 4 lights-per-cluster heatmap, 5 cluster culling correctness
        // (green correct / red a light was wrongly culled / blue no cluster).
        int debugMode = 0;
        int aabbHeatmapCutoff = 5000;
		int triangleHeatmapCutoff = 100;
		bool useDoubleBuffering = false; // DEAD since Phase 1: Renderer::Render returns the image it just wrote

		// Logs the render graph's passes, resources and derived lifetimes once per
		// frame. Deliberately NOT serialized -- it is a debugging toggle, not a
		// project setting, and building the string is not free.
		bool dumpRenderGraph = false;

		ShaderType shaderType = ShaderType::PATH_TRACING;

        glm::uvec2 resolution{ 400, 300 };
        int raysPerPixel = 1;
        int bouncesPerRay = 5;
        bool accumulate = false;
        bool vSync = true;
        
        inline void SerializeToYamlNode(YAML::Node& rsNode) const {
			rsNode["debugMode"] = debugMode;
			rsNode["aabbHeatmapCutoff"] = aabbHeatmapCutoff;
			rsNode["triangleHeatmapCutoff"] = triangleHeatmapCutoff;
			rsNode["shaderType"] = static_cast<int>(shaderType);

			rsNode["resolution"] = YAML::Load("[" + std::to_string(resolution.x) + ", " + std::to_string(resolution.y) + "]");
			rsNode["raysPerPixel"] = raysPerPixel;
			rsNode["bouncesPerRay"] = bouncesPerRay;
			rsNode["accumulate"] = accumulate;
			rsNode["vSync"] = vSync;
        }

        inline bool DeserializeFromYamlNode(YAML::Node& rsNode) {
			*this = RenderSettings{}; // reset to defaults
			try {
				if (auto n = rsNode["debugMode"]) debugMode = n.as<int>();
				if (auto n = rsNode["aabbHeatmapCutoff"]) aabbHeatmapCutoff = n.as<int>();
				if (auto n = rsNode["triangleHeatmapCutoff"]) triangleHeatmapCutoff = n.as<int>();
				if (auto n = rsNode["shaderType"]) shaderType = static_cast<ShaderType>(n.as<int>());

				if (auto n = rsNode["resolution"]; n && n.IsSequence() && n.size() == 2) {
					resolution.x = n[0].as<uint32_t>();
					resolution.y = n[1].as<uint32_t>();
				}
				if (auto n = rsNode["raysPerPixel"])  raysPerPixel = n.as<uint32_t>();
				if (auto n = rsNode["bouncesPerRay"]) bouncesPerRay = n.as<uint32_t>();
				if (auto n = rsNode["accumulate"])    accumulate = n.as<bool>();
				if (auto n = rsNode["vSync"])         vSync = n.as<bool>();

				return true;
			}
			catch (const std::exception&) {
				return false;
			}
        }
    };
}
