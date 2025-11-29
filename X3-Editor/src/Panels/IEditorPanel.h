#pragma once

namespace Laura
{

	class IEditorPanel {
	public:
		virtual ~IEditorPanel() = default;
		virtual void init() = 0;
		virtual void OnImGuiRender() = 0;
		virtual void onEvent(std::shared_ptr<IEvent> event) = 0;
	};
}