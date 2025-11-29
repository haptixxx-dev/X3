#include "EditorState.h"
#include "EditorCfg.h"

namespace Laura
{

    bool serializeState(const std::shared_ptr<const EditorState>& state) {
		std::string editorStateFilePath = (EditorCfg::RESOURCES_PATH / EDITOR_STATE_FILENAME).string();
		try {
			YAML::Node node;
			node["ViewportMode"]        = ScreenFitModeToString(state->persistent.viewportMode);
			node["EditorThemeFilepath"] = state->persistent.editorThemeFilepath.string();
			YAML::Node rsNode = node["EditorRenderSettings"];
			state->persistent.editorRenderSettings.SerializeToYamlNode(rsNode);

			std::ofstream fout(editorStateFilePath);
			if (!fout.is_open()) {
				LOG_EDITOR_CRITICAL("Could not open file for writing: {0}", editorStateFilePath);
				return false;
			}

			fout << node;
			if (!fout) { // check write errors
				LOG_EDITOR_CRITICAL("Error occurred while writing file: {0}", editorStateFilePath);
				return false;
			}

			return true;
		}
		catch (const YAML::RepresentationException& e) {
			LOG_EDITOR_CRITICAL("YAML representation error (invalid syntax?): {0}, error: {1}", editorStateFilePath, e.what());
			return false;
		}
		catch (const std::exception& e) {
			LOG_EDITOR_CRITICAL("Unknown error occurred while saving file: {0}, error: {1}", editorStateFilePath, e.what());
			return false;
		}
	}


	bool deserializeState(const std::shared_ptr<EditorState>& state) {
        std::string filepath = (EditorCfg::RESOURCES_PATH / EDITOR_STATE_FILENAME).string();

        try {
			YAML::Node node = YAML::LoadFile(filepath); 
            state->persistent.viewportMode = node["ViewportMode"] ? 
                ScreenFitModeFromString(node["ViewportMode"].as<std::string>()).value_or(ScreenFitMode::MaxAspectFit) : 
                ScreenFitMode::MaxAspectFit;

            state->persistent.editorThemeFilepath = node["EditorThemeFilepath"] ? 
				std::filesystem::path{node["EditorThemeFilepath"].as<std::string>()} : 
				"";

			YAML::Node rsNode = node["EditorRenderSettings"];
			state->persistent.editorRenderSettings.DeserializeFromYamlNode(rsNode);
		}
		catch (const YAML::RepresentationException& e) {
			LOG_EDITOR_ERROR("YAML representation error (make sure the file is valid): {0}, error: {1}", filepath, e.what());
			return false;
		}
		catch (const std::exception& e) {
			LOG_EDITOR_ERROR("Unknown error occurred while loading file: {0}, error: {1}", filepath, e.what());
			return false;
		}
		
		// Load theme based on the filepath
        auto [status, errMsg] = state->temp.editorTheme.LoadFromFile(state->persistent.editorThemeFilepath); // deserialize derived state
        if (state->persistent.editorThemeFilepath == "") {
            LOG_EDITOR_INFO("Using default theme");
        }
        else if (!status) {
            LOG_EDITOR_WARN("Unable to deserialize theme: {0} [Using Default Theme instead]", state->persistent.editorThemeFilepath.string());
        }
        else {
            LOG_EDITOR_INFO("Successfully loaded theme {0}", state->persistent.editorThemeFilepath.string());
        }
		return true;
	}
}