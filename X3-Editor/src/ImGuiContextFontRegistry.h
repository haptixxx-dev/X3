#pragma once

#include <imgui.h>

namespace X3
{
	struct ImGuiContextFontRegistry {
		ImFont* highResIcons = nullptr;
		ImFont* notoSansRegular = nullptr;
		ImFont* notoSansBold = nullptr;
		ImFont* codicon = nullptr;
		ImFont* X3BrandIcons = nullptr;
	};

	inline ImGuiContextFontRegistry* Fonts() {
		return static_cast<ImGuiContextFontRegistry*>(ImGui::GetIO().UserData);
	}
}
