#include "ExportSettings.h"
#include "yaml-cpp/yaml.h"

namespace Laura
{

    const char* ScreenFitModeToString(ScreenFitMode mode) {
        switch (mode) {
        case ScreenFitMode::OriginalCentered:   return "OriginalCentered";
        case ScreenFitMode::StretchFill:        return "StretchFill";
        case ScreenFitMode::MaxAspectFit:       return "MaxAspectFit";
        default:                                return "MaxAspectFit";
        }
    }

    std::optional<ScreenFitMode> ScreenFitModeFromString(const std::string& str) {
        // compiler converts to std::optional<>
        if (str == "OriginalCentered")  return { ScreenFitMode::OriginalCentered };
        if (str == "StretchFill")       return { ScreenFitMode::StretchFill };
        if (str == "MaxAspectFit")      return { ScreenFitMode::MaxAspectFit };
        return std::nullopt;
    }

    bool SerializeExportSettingsYaml(const std::filesystem::path& folderpath, const ExportSettings& settings) {
        if (!std::filesystem::exists(folderpath)) {
            LOG_ENGINE_WARN("SerializeExportSettingsYaml: folder does not exist: {}", folderpath.string());
            return false;
        }

        YAML::Node node;
        node["fullscreen"] = settings.fullscreen;
        node["vSync"] = settings.vSync;
        node["screenFitMode"] = ScreenFitModeToString(settings.screenFitMode);

        std::filesystem::path filePath = folderpath / EXPORT_SETTINGS_FILENAME;
        std::ofstream fout(filePath);
        if (!fout.is_open()) {
            LOG_ENGINE_ERROR("SerializeExportSettingsYaml: could not open {} for writing", filePath.string());
            return false;
        }

        fout << node;
        LOG_ENGINE_INFO("SerializeExportSettingsYaml: wrote export settings to {}", filePath.string());
        return true;
    }

    std::optional<ExportSettings> DeserializeExportSettingsYaml(const std::filesystem::path& folderpath) {
        std::filesystem::path filePath = folderpath / EXPORT_SETTINGS_FILENAME;
        if (!std::filesystem::exists(filePath)) {
            LOG_ENGINE_WARN("DeserializeExportSettingsYaml: file does not exist: {}", filePath.string());
            return std::nullopt;
        }

        try {
            YAML::Node node = YAML::LoadFile(filePath.string());
            ExportSettings settings;

            settings.fullscreen = node["fullscreen"] ? node["fullscreen"].as<bool>() : false;
            settings.vSync = node["vSync"] ? node["vSync"].as<bool>() : true;
            settings.screenFitMode = node["screenFitMode"] ? 
                ScreenFitModeFromString(node["screenFitMode"].as<std::string>()).value_or(ScreenFitMode::MaxAspectFit) : 
                ScreenFitMode::MaxAspectFit;

            LOG_ENGINE_INFO("DeserializeExportSettingsYaml: successfully loaded export settings from {}", filePath.string());
            return settings;
        }
        catch (const YAML::ParserException& e) {
            LOG_ENGINE_ERROR("DeserializeExportSettingsYaml: YAML parse error reading {}: {}", filePath.string(), e.what());
            return std::nullopt;
        }
    }
}