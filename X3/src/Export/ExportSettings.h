#pragma once

#include <filesystem>
#include <optional>
#include "lrpch.h"
#include "Core/GUID.h"

#define EXPORT_SETTINGS_FILENAME "ExportSettings.yaml"

namespace Laura
{
    enum class ScreenFitMode {
        OriginalCentered,   // Keep original size, centered
        StretchFill,        // Stretch to completely fill the screen
        MaxAspectFit,        // Scale to largest possible size while keeping aspect ratio
        _COUNT
    };
	inline constexpr const char* ScreenFitModeStr[] = { 
		"OriginalCentered", 
		"StretchFill", 
		"MaxAspectFit" 
	};

    const char* ScreenFitModeToString(ScreenFitMode mode);
    std::optional<ScreenFitMode> ScreenFitModeFromString(const std::string& str);

    struct ExportSettings {
        bool fullscreen = false;
        bool vSync = true;
        ScreenFitMode screenFitMode = ScreenFitMode::MaxAspectFit;
    };

    bool SerializeExportSettingsYaml(const std::filesystem::path& folderpath, const ExportSettings& settings);
    std::optional<ExportSettings> DeserializeExportSettingsYaml(const std::filesystem::path& folderpath);
}
