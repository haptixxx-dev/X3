#pragma once 

#include "X3.h"
#include "EditorState.h"
#include "Panels/IEditorPanel.h"

namespace X3
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