#pragma once 

#include "Laura.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

namespace Laura
{

	class ThemePanel : public IEditorPanel {
	public:
		inline ThemePanel(std::shared_ptr<EditorState> editorState)
			: m_EditorState(editorState) {
		}

		~ThemePanel() = default;

		virtual inline void init() override {}
		virtual void OnImGuiRender() override;
		virtual inline void onEvent(std::shared_ptr<IEvent> event) override {}

	private:
		std::shared_ptr<EditorState> m_EditorState;
	};
}