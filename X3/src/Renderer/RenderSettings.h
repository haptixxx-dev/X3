#pragma once

#include "lrpch.h"
#include <yaml-cpp/yaml.h>

namespace X3
{
	enum class ShaderType {
		PATH_TRACING = 0,
		PHONG = 1,
		PBR = 2,
		DENOISE = 3  // Internal shader - not user-selectable
	};

	enum class RendererAPI {
		OpenGL = 0,
		Vulkan = 1
	};

	struct RenderSettings {
        // Editor-only: not meant to be other than default during runtime
        int debugMode = 0; // 0 = off, 1 = aabb heatmap, 2 = triangle heatmap
        int aabbHeatmapCutoff = 5000;
		int triangleHeatmapCutoff = 100;
		bool useDoubleBuffering = false; // Enable for runtime/playing mode to prevent GPU sync stalls

		ShaderType shaderType = ShaderType::PATH_TRACING;
		// Use Vulkan on macOS (OpenGL 4.1 lacks compute shaders), OpenGL elsewhere
		#ifdef __APPLE__
		RendererAPI rendererAPI = RendererAPI::Vulkan;
		#else
		RendererAPI rendererAPI = RendererAPI::OpenGL;
		#endif

        glm::uvec2 resolution{ 400, 300 };
        int raysPerPixel = 1;
        int bouncesPerRay = 5;
        bool accumulate = false;
        bool vSync = true;

        // Denoising settings
        bool enableDenoise = true;
        int denoiseQuality = 1;                // 0 = Fast (temporal only), 1 = High (SVGF with à-trous)
        float denoiseTemporalAlpha = 0.1f;     // 0.1 = 90% history (smooth), higher = favor current frame
        float denoiseSigmaColor = 0.5f;        // Color edge sensitivity
        float denoiseSigmaNormal = 0.3f;       // Normal edge sensitivity (radians)
        float denoiseSigmaDepth = 0.1f;        // Depth edge sensitivity (relative)
        int denoiseFilterRadius = 2;           // Spatial filter radius (1-3 recommended)
        float denoiseMotionScale = 0.05f;      // How much pixel motion increases temporal alpha
        float denoiseVarianceClipGamma = 1.5f; // Variance clipping aggressiveness (1.0-2.0)
        int denoiseAtrousPasses = 4;           // Number of à-trous passes (1-5, only used when quality=1)
        
        inline void SerializeToYamlNode(YAML::Node& rsNode) const {
			rsNode["debugMode"] = debugMode;
			rsNode["aabbHeatmapCutoff"] = aabbHeatmapCutoff;
			rsNode["triangleHeatmapCutoff"] = triangleHeatmapCutoff;
			rsNode["shaderType"] = static_cast<int>(shaderType);
			rsNode["rendererAPI"] = static_cast<int>(rendererAPI);

			rsNode["resolution"] = YAML::Load("[" + std::to_string(resolution.x) + ", " + std::to_string(resolution.y) + "]");
			rsNode["raysPerPixel"] = raysPerPixel;
			rsNode["bouncesPerRay"] = bouncesPerRay;
			rsNode["accumulate"] = accumulate;
			rsNode["vSync"] = vSync;

			// Denoise settings
			rsNode["enableDenoise"] = enableDenoise;
			rsNode["denoiseQuality"] = denoiseQuality;
			rsNode["denoiseTemporalAlpha"] = denoiseTemporalAlpha;
			rsNode["denoiseSigmaColor"] = denoiseSigmaColor;
			rsNode["denoiseSigmaNormal"] = denoiseSigmaNormal;
			rsNode["denoiseSigmaDepth"] = denoiseSigmaDepth;
			rsNode["denoiseFilterRadius"] = denoiseFilterRadius;
			rsNode["denoiseMotionScale"] = denoiseMotionScale;
			rsNode["denoiseVarianceClipGamma"] = denoiseVarianceClipGamma;
			rsNode["denoiseAtrousPasses"] = denoiseAtrousPasses;
        }

        inline bool DeserializeFromYamlNode(YAML::Node& rsNode) {
			*this = RenderSettings{}; // reset to defaults
			try {
				if (auto n = rsNode["debugMode"]) debugMode = n.as<int>();
				if (auto n = rsNode["aabbHeatmapCutoff"]) aabbHeatmapCutoff = n.as<int>();
				if (auto n = rsNode["triangleHeatmapCutoff"]) triangleHeatmapCutoff = n.as<int>();
				if (auto n = rsNode["shaderType"]) shaderType = static_cast<ShaderType>(n.as<int>());
				if (auto n = rsNode["rendererAPI"]) rendererAPI = static_cast<RendererAPI>(n.as<int>());

				if (auto n = rsNode["resolution"]; n && n.IsSequence() && n.size() == 2) {
					resolution.x = n[0].as<uint32_t>();
					resolution.y = n[1].as<uint32_t>();
				}
				if (auto n = rsNode["raysPerPixel"])  raysPerPixel = n.as<uint32_t>();
				if (auto n = rsNode["bouncesPerRay"]) bouncesPerRay = n.as<uint32_t>();
				if (auto n = rsNode["accumulate"])    accumulate = n.as<bool>();
				if (auto n = rsNode["vSync"])         vSync = n.as<bool>();

				// Denoise settings
				if (auto n = rsNode["enableDenoise"]) enableDenoise = n.as<bool>();
				if (auto n = rsNode["denoiseQuality"]) denoiseQuality = n.as<int>();
				if (auto n = rsNode["denoiseTemporalAlpha"]) denoiseTemporalAlpha = n.as<float>();
				if (auto n = rsNode["denoiseSigmaColor"]) denoiseSigmaColor = n.as<float>();
				if (auto n = rsNode["denoiseSigmaNormal"]) denoiseSigmaNormal = n.as<float>();
				if (auto n = rsNode["denoiseSigmaDepth"]) denoiseSigmaDepth = n.as<float>();
				if (auto n = rsNode["denoiseFilterRadius"]) denoiseFilterRadius = n.as<int>();
				if (auto n = rsNode["denoiseMotionScale"]) denoiseMotionScale = n.as<float>();
				if (auto n = rsNode["denoiseVarianceClipGamma"]) denoiseVarianceClipGamma = n.as<float>();
				if (auto n = rsNode["denoiseAtrousPasses"]) denoiseAtrousPasses = n.as<int>();

				return true;
			}
			catch (const std::exception&) {
				return false;
			}
        }
    };
}
