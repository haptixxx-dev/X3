#pragma once

#include "X3.h"
#include "ImGuiContextFontRegistry.h"

namespace X3 
{
	class ImGuiContext {
	public:
		ImGuiContext(std::shared_ptr<IWindow> window);
		virtual ~ImGuiContext();

		void Init();
		void BeginFrame();
		void EndFrame();
		void Shutdown();

		void LoadDefaultLayout();

	private:
		std::shared_ptr<ImGuiContextFontRegistry> m_FontRegistry;
		std::shared_ptr<IWindow> m_Window;

		const std::filesystem::path m_ImGuiIniPath; // actually used for loading / saving
		const std::filesystem::path m_DefaultImGuiIniPath; // fallback for when the above is missing

		bool m_LoadDefaultLayoutBeforeNewFrame;
	};
}