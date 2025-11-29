#pragma once

#include "Laura.h"
#include <array>
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

namespace Laura
{

	class RenderSettingsPanel : public IEditorPanel {
	public:
		RenderSettingsPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<IEventDispatcher> eventDispatcher, std::shared_ptr<ProjectManager> projectManager)
			: m_EditorState(editorState), m_EventDispatcher(eventDispatcher), m_ProjectManager(projectManager) {
        }

		~RenderSettingsPanel() = default;

        virtual void init() override;
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		struct ResolutionOption {
			glm::uvec2 resolution;
			const char* label;
		};

		const std::array<ResolutionOption, 30> m_ResolutionOptions = {{
			{{0, 0},       "Preview"},
			{{320, 240},   "Preview 4:3"},
			{{320, 180},   "Preview 16:9"},
			{{320, 200},   "Preview 16:10"},
			{{320, 137},   "Preview 21:9"},
			{{180, 320},   "Preview 9:16"},
			{{147, 320},   "Preview 9:19.5"},
			{{108, 320},   "Preview 9:20"},

			{{0, 0},       "4:3 Standard"},
			{{640, 480},   "640x480 (4:3) VGA"},
			{{800, 600},   "800x600 (4:3) SVGA"},
			{{1024, 768},  "1024x768 (4:3) XGA"},

			{{0, 0},       "16:10 Standard"},
			{{1280, 800},  "1280x800 (16:10) WXGA"},
			{{1440, 900},  "1440x900 (16:10) WXGA+"},
			{{1680, 1050}, "1680x1050 (16:10) WSXGA+"},
			{{1920, 1200}, "1920x1200 (16:10) WUXGA"},

			{{0, 0},       "16:9 Standard"},
			{{1280, 720},  "1280x720 (16:9) HD"},
			{{1920, 1080}, "1920x1080 (16:9) FHD"},
			{{2560, 1440}, "2560x1440 (16:9) QHD"},
			{{3840, 2160}, "3840x2160 (16:9) 4K"},

			{{0, 0},       "21:9 Ultra-wide"},
			{{3440, 1440}, "3440x1440 (21:9) UWQHD"},
			{{5120, 2160}, "5120x2160 (21:9) 5K UW"},

			{{0, 0},       "9:16+ Vertical"},
			{{720, 1280},  "720x1280 (9:16) V-HD"},
			{{1080, 1920}, "1080x1920 (9:16) V-FHD"},
			{{1080, 2340}, "1080x2340 (9:19.5) V-FHD+"},
			{{1440, 3200}, "1440x3200 (9:20) V-WQHD+"}
		}};

		std::shared_ptr<EditorState> m_EditorState;
        std::shared_ptr<IEventDispatcher> m_EventDispatcher;
        std::shared_ptr<ProjectManager> m_ProjectManager;
	};
}