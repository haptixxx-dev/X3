#pragma once

#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <cassert>
#include "X3.h"
#include "EditorTheme.h"
#include "Export/ExportSettings.h"

#define EDITOR_STATE_FILENAME "EditorState.yaml" 

namespace X3
{

	struct EditorState {
		struct {
			bool useEditorCamera = true; // Toggle between editor camera and scene's main camera
			entt::entity selectedEntity = entt::null;
			EditorTheme editorTheme;
			bool isInRuntimeSimulation = false;
			bool vSync = false; // Current VSync state

			// panels
			bool isViewportSettingsPanelOpen = false;
			bool isThemePanelOpen = false;
			bool isProfilerPanelOpen = true;

			// dialogs
			bool isCreateProjectDialogOpen = false;
			bool shouldOpenExportPanel = false;

			// clipboard
			MaterialDesc copiedMaterial;   // ONE SLOT, not the whole component -- copy/paste is per slot now
			bool hasCopiedMaterial = false;
			entt::entity copiedEntity = entt::null;
			bool isCutOperation = false; // true if Ctrl+X was used

			// grid snapping
			bool snapToGrid = false;
			float snapPositionValue = 0.5f;
			float snapRotationValue = 15.0f;
			float snapScaleValue = 0.1f;

			// physics debug visualization
			bool showPhysicsDebug = false;
			bool showColliderWireframes = true;
			bool showColliderAABBs = false;
			bool showContactPoints = false;
			bool showVelocityVectors = false;
			bool showConstraints = false;
			glm::vec4 colliderWireframeColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f); // Green
			glm::vec4 triggerWireframeColor = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f);  // Blue
			glm::vec4 contactPointColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);      // Red
			glm::vec4 velocityVectorColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);    // Yellow
			glm::vec4 constraintColor = glm::vec4(1.0f, 0.5f, 0.0f, 1.0f);        // Orange
			bool isPhysicsSettingsPanelOpen = false;

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

