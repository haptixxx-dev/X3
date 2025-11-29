#pragma once

#include "Laura.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

namespace Laura
{

	class ProfilerPanel : public IEditorPanel {
	public:
		ProfilerPanel(std::shared_ptr<EditorState> editorState, std::shared_ptr<Profiler> profiler)
			: m_EditorState(editorState), m_Profiler(profiler) {
		}

		~ProfilerPanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}
		
	private:
		
		struct plotLineStyle {
			ImVec4 color;
			int colormapID;
			float thickness;
		};

		std::shared_ptr<EditorState> m_EditorState;
		std::shared_ptr<Profiler> m_Profiler;
	};
}