#pragma once

#include <Laura.h>
#include <functional>
#include <imgui.h>

namespace Laura {

	namespace DNDPayloadTypes {
		inline constexpr const char* MESH = "DND_PAYLOAD_MESH";
		inline constexpr const char* TEXTURE = "DND_PAYLOAD_TEXTURE";
		inline constexpr const char* SCENE = "DND_PAYLOAD_SCENE";
	}

	struct DNDPayload {
		LR_GUID guid;
		char title[256];
	};

	template<typename OnDrop>
	void DragDropWidget(
			const char* label,
			const char* payloadType,
			const std::string& displayValue,
			OnDrop onDrop,
			EditorTheme& theme,
			const char* tooltip = nullptr,
			ImVec2 widgetSize = {0, 0},
			bool selected = true)
	{
		const bool hasLabel = (label && *label);

		// Draw label
		if (hasLabel) {
			ImGui::AlignTextToFramePadding();
			theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
			ImGui::TextUnformatted(label);
			theme.PopColor();
			ImGui::SameLine();
		}

		// Calculate size
		ImVec2 finalSize = widgetSize;
		if (finalSize.x <= 0) {
			finalSize.x = ImGui::GetContentRegionAvail().x;
		} else if (hasLabel) {
			finalSize.x -= ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemSpacing.x;
		}

		if (finalSize.y <= 0) {
			finalSize.y = 0; // default height
		}

		// Push a flat style: no hover/active highlight
		theme.PushColor(ImGuiCol_Text, selected ? EditorCol_Text1 : EditorCol_Text2);
		theme.PushColor(ImGuiCol_Button,        EditorCol_Primary3);
		theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_Primary3);
		theme.PushColor(ImGuiCol_ButtonActive,  EditorCol_Primary3);

		// Build unique ID (text + invisible ID part)
		std::string selectableId = "##" + std::string(label ? label : "DND") + "Selectable";
		ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2{ 0.0f, 0.5f });
		ImGui::Button((displayValue + selectableId).c_str(), finalSize);
		ImGui::PopStyleVar();

		theme.PopColor(4);

		// Tooltip
		if (tooltip && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", tooltip);
		}

		// Drag-drop target
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(payloadType)) {
				IM_ASSERT(payload->DataSize == sizeof(DNDPayload));
				const auto& dndPayload = *static_cast<const DNDPayload*>(payload->Data);
				onDrop(dndPayload);
			}
			ImGui::EndDragDropTarget();
		}
	}
}
