#pragma once

#include "Laura.h"
#include <imgui.h>
#include <filesystem> 
#include <yaml-cpp/yaml.h>
#include <deque>
#include <array>
#include <cassert>

#define EDITOR_THEME_FILE_EXTENSION ".lrtheme"

namespace Laura
{
	
	enum EditorCol_ {
		EditorCol_Primary1,
		EditorCol_Primary2,
		EditorCol_Primary3,
		EditorCol_Secondary1,
		EditorCol_Secondary2,
		EditorCol_Accent1,
		EditorCol_Accent2,
		EditorCol_Text1,
		EditorCol_Text2,
		EditorCol_Background1,
		EditorCol_Background2,
		EditorCol_Background3,
		EditorCol_Background4,
		EditorCol_Error,
		EditorCol_Warning,
		EditorCol_Success,
		EditorCol_X,
		EditorCol_Y,
		EditorCol_Z,
		EditorCol_COUNT
	};
	
	constexpr const char* EditorColStrings[] = {
		"EditorCol_Primary1",
		"EditorCol_Primary2",
		"EditorCol_Primary3",
		"EditorCol_Secondary1",
		"EditorCol_Secondary2",
		"EditorCol_Accent1",
		"EditorCol_Accent2",
		"EditorCol_Text1",
		"EditorCol_Text2",
		"EditorCol_Background1",
		"EditorCol_Background2",
		"EditorCol_Background3",
		"EditorCol_Background4",
		"EditorCol_Error",
		"EditorCol_Warning",
		"EditorCol_Success",
		"EditorCol_X",
		"EditorCol_Y",
		"EditorCol_Z"
	};

	static_assert(std::size(EditorColStrings) == EditorCol_COUNT, "Mismatch in enum and string array size");

	class EditorTheme {
	public: 
		EditorTheme() {
			LoadDefaultDark();
			ApplyAllToImgui();
		}

		void LoadDefaultDark(); // main default
		void LoadDefaultLight(); // not used (exported in repo as DefaultLight.lrtheme)

		inline std::pair<bool, std::string> LoadFromFile(const std::filesystem::path& filepath) {
			auto [success, errMsg] = DeserializeFromYAML(filepath);
			if (success) {
				ApplyAllToImgui();
			}
			return { success, errMsg };
		}

		inline std::pair<bool, std::string> SaveToFile(const std::filesystem::path& filepath) {
			return SerializeToYAML(filepath);
		}
			
		inline void PushColor(ImGuiCol_ widget, EditorCol_ col, float alpha = 1.0f) {
			ImVec4 color = m_ColorPallete[col];
			color.w = alpha;
			ImGui::PushStyleColor(widget, color);
		}

		inline void PopColor(size_t count = 1) {
			ImGui::PopStyleColor(count);
		}

		inline ImVec4& operator[](EditorCol_ col) {
			assert(0 <= col && col < EditorCol_COUNT && "Invalid EditorCol_ passed");
			return m_ColorPallete[col];
		}

		inline const ImVec4& operator[](EditorCol_ col) const {
			assert(0 <= col && col < EditorCol_COUNT && "Invalid EditorCol_ passed");
			return m_ColorPallete[col];
		}

		inline static ImVec4 RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
            constexpr float inv255 = 1.0f / 255.0f;
            return ImVec4{ r * inv255, g * inv255, b * inv255, a * inv255 };
        };

		void ApplyAllToImgui();

	private:
		std::array<ImVec4, EditorCol_COUNT> m_ColorPallete; // stores the actual colors of the theme
		
		std::pair<bool, std::string> SerializeToYAML(const std::filesystem::path& filepath);
		std::pair<bool, std::string> DeserializeFromYAML(const std::filesystem::path& filepath);
	};
};

template <>
struct YAML::convert<ImVec4> {
	static YAML::Node encode(const ImVec4& rhs) {
		YAML::Node node;
		node.SetStyle(YAML::EmitterStyle::Flow);  // Force inline [x, y, z, w] format
		node.push_back(rhs.x);
		node.push_back(rhs.y);
		node.push_back(rhs.z);
		node.push_back(rhs.w);
		return node;
	}

	static bool decode(const YAML::Node& node, ImVec4& rhs) {
		if (!node.IsSequence() || node.size() != 4) {
			return false;
		}
		rhs.x = node[0].as<float>();
		rhs.y = node[1].as<float>();
		rhs.z = node[2].as<float>();
		rhs.w = node[3].as<float>();
		return true;
	}
};

template <>
struct YAML::convert<Laura::EditorTheme> {
	static YAML::Node encode(const Laura::EditorTheme& rhs) {
		YAML::Node node;
		for (size_t i = 0; i < Laura::EditorCol_COUNT; i++) {
			node[Laura::EditorColStrings[i]] = rhs[static_cast<Laura::EditorCol_>(i)];
		}
		return node;
	}

	static bool decode(const YAML::Node& node, Laura::EditorTheme& rhs) {
		if (!node.IsMap()) return false;
		for (size_t i = 0; i < Laura::EditorCol_COUNT; i++) { 
			if (node[Laura::EditorColStrings[i]]) { 
				rhs[static_cast<Laura::EditorCol_>(i)] = node[Laura::EditorColStrings[i]].as<ImVec4>();
			}
		}
		return true;
	}
};
