#pragma once

#include "lrpch.h"
#include <yaml-cpp/yaml.h>

namespace Laura
{

	struct RenderSettings {
        // Editor-only: not meant to be other than default during runtime
        int debugMode = 0; // 0 = off, 1 = aabb heatmap, 2 = triangle heatmap
        int aabbHeatmapCutoff = 5000;
		int triangleHeatmapCutoff = 100;

        glm::uvec2 resolution{ 400, 300 };
        int raysPerPixel = 1;
        int bouncesPerRay = 5;
        bool accumulate = false;
        bool vSync = true;
        
        inline void SerializeToYamlNode(YAML::Node& rsNode) const {
			rsNode["debugMode"] = debugMode;
			rsNode["aabbHeatmapCutoff"] = aabbHeatmapCutoff;
			rsNode["triangleHeatmapCutoff"] = triangleHeatmapCutoff;

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