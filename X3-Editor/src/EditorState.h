#pragma once

#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <cassert>
#include "Laura.h"
#include "EditorTheme.h"
#include "Export/ExportSettings.h"

#define EDITOR_STATE_FILENAME "EditorState.yaml" 

namespace Laura
{

	struct EditorState {
		struct {
			entt::entity selectedEntity = entt::null;
			EditorTheme editorTheme;
			bool isInRuntimeSimulation = false;

			// panels
			bool isViewportSettingsPanelOpen = false;
			bool isThemePanelOpen = false;
			bool isProfilerPanelOpen = true;

			// dialogs
			bool isCreateProjectDialogOpen = false;
			bool shouldOpenExportPanel = false;

		} temp;

		// TO ADD new persistent entries, add them here and update the SERIALIZE and DESERIALIZE functions
		// (if the type is custom, also create a YAML::convert template specialization)
		struct {
			std::filesystem::path editorThemeFilepath = "";
			RenderSettings editorRenderSettings{};
			ScreenFitMode viewportMode = ScreenFitMode::MaxAspectFit;
		} persistent;
	};

	bool serializeState(const std::shared_ptr<const EditorState>& state);
	// also deserializes derived state (EditorTheme from editorThemeFilepath)
	bool deserializeState(const std::shared_ptr<EditorState>& state); 
}


