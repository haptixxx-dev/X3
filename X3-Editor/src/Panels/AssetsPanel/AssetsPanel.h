#pragma once

#include <Laura.h>
#include "Panels/IEditorPanel.h"
#include "EditorState.h"
#include "IconsFontAwesome6.h"
#include "Panels/DNDPayloads.h"

namespace Laura 
{

	class AssetsPanel : public IEditorPanel {
	public:
		AssetsPanel(std::shared_ptr<EditorState> editorState,
			std::shared_ptr<ProjectManager> projectManager)
			: m_EditorState(editorState),
			m_ProjectManager(projectManager) {
		}
		~AssetsPanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		void DrawAssetTile(LR_GUID guid, const char* title);
		void DrawSceneTile(LR_GUID guid, const char* title);
		void DrawGenericTile(LR_GUID guid, const char* title, const char* icon, const char* dndPayloadType);
		void DrawTileInfo();
		
		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<ProjectManager> m_ProjectManager;

		// UI related
		const float BASE_TILE_WH_RATIO = 0.75f;
		const float BASE_TILE_ICON_FONT_SIZE = 0.2f;
		const float BASE_TILE_TITLE_FONT_SIZE = 16.0f;
		const float TILE_SCALAR_MIN = 75.0f;
		const float TILE_SCALAR_MAX = 110.0f;

		LR_GUID m_SelectedTileGuid = LR_GUID::INVALID;
		float m_TileScalar = TILE_SCALAR_MAX;

		DNDPayload m_DNDPayload;
	};
}