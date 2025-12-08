#pragma once

#include "X3.h"
#include "EditorState.h"
#include <imgui_internal.h>
#include "ImGuiContextFontRegistry.h"

namespace X3
{

	// Since the transform component can only get and set values through its own functions, we need to pass the set function as a lambda
	template <typename T>
	void TransformVec3Slider(std::shared_ptr<EditorState> editorState, 
							 const char* label, 
							 glm::vec3 vector, 
							 const T& setVector,
							 float resetVal = 0.0f) {

		EditorTheme& theme = editorState->temp.editorTheme;

		ImGui::AlignTextToFramePadding();
		ImGui::Columns(2);
		// width of the 1st column (labels)
		ImGui::SetColumnWidth(0, 100.0f);
		float lineheight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
		ImVec2 btnSize = ImVec2(lineheight * 0.7, lineheight);

		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		ImGui::Text(label);
		theme.PopColor();

		ImGui::NextColumn();
		ImGui::PushMultiItemsWidths(3, ImGui::GetContentRegionAvail().x - 30);

		ImGui::PushID(label);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 0, 0 });

		theme.PushColor(ImGuiCol_ButtonActive, EditorCol_Secondary2);
		{
			theme.PushColor(ImGuiCol_Button, EditorCol_X);
			theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_X);
			{
				ImGui::SetNextItemWidth(lineheight);
				theme.PushColor(ImGuiCol_Text, EditorCol_Secondary2);
				ImGui::PushFont(Fonts()->notoSansBold);
				if (ImGui::Button("X", btnSize)) {
					setVector(glm::vec3(resetVal, vector.y, vector.z));
				}
				ImGui::PopFont();
				theme.PopColor();
				ImGui::SameLine();
				if (ImGui::DragFloat("##X", &vector.x, 0.5f)) {
					setVector(vector);
				}
				ImGui::SameLine();
				ImGui::PopItemWidth();
			}
			theme.PopColor(2); // Button, ButtonHovered
			theme.PushColor(ImGuiCol_Button, EditorCol_Y);
			theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_Y);
			{
				ImGui::SetNextItemWidth(lineheight);
				theme.PushColor(ImGuiCol_Text, EditorCol_Secondary2);
				ImGui::PushFont(Fonts()->notoSansBold);
				if (ImGui::Button("Y", btnSize)) {
					setVector(glm::vec3(vector.x, resetVal, vector.z));
				}
				ImGui::PopFont();
				theme.PopColor();
				ImGui::SameLine();
				if (ImGui::DragFloat("##Y", &vector.y, 0.5f)) {
					setVector(vector);
				}
				ImGui::SameLine();
				ImGui::PopItemWidth();
			}
			theme.PopColor(2); // Button, ButtonHovered
			theme.PushColor(ImGuiCol_Button, EditorCol_Z);
			theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_Z);
			{
				ImGui::SetNextItemWidth(lineheight);
				theme.PushColor(ImGuiCol_Text, EditorCol_Secondary2);
				ImGui::PushFont(Fonts()->notoSansBold);
				if (ImGui::Button("Z", btnSize)) {
					setVector(glm::vec3(vector.x, vector.y, resetVal));
				}
				ImGui::PopFont();
				theme.PopColor();
				ImGui::SameLine();
				if (ImGui::DragFloat("##Z", &vector.z, 0.5f)) {
					setVector(vector);
				}
				ImGui::PopItemWidth();
			}
			theme.PopColor(2); // Button, ButtonHovered
		}
		theme.PopColor(); // ButtonActive

		ImGui::PopStyleVar();
		ImGui::PopID();
		ImGui::Columns(1);
	}

	inline void DrawTransformSliders(std::shared_ptr<EditorState> editorState, EntityHandle entity) {
		EditorTheme& theme = editorState->temp.editorTheme;

		// Grid Snapping Controls
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
		theme.PushColor(ImGuiCol_Button, editorState->temp.snapToGrid ? EditorCol_Accent1 : EditorCol_Primary2);
		if (ImGui::Button(editorState->temp.snapToGrid ? ICON_FA_MAGNET " Snap: ON" : ICON_FA_MAGNET " Snap: OFF")) {
			editorState->temp.snapToGrid = !editorState->temp.snapToGrid;
		}
		theme.PopColor();

		if (editorState->temp.snapToGrid) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(60.0f);
			ImGui::DragFloat("##SnapPos", &editorState->temp.snapPositionValue, 0.05f, 0.01f, 10.0f, "%.2f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Position snap value");

			ImGui::SameLine();
			ImGui::SetNextItemWidth(60.0f);
			ImGui::DragFloat("##SnapRot", &editorState->temp.snapRotationValue, 1.0f, 1.0f, 90.0f, "%.0f°");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotation snap value (degrees)");

			ImGui::SameLine();
			ImGui::SetNextItemWidth(60.0f);
			ImGui::DragFloat("##SnapScale", &editorState->temp.snapScaleValue, 0.01f, 0.01f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale snap value");
		}
		ImGui::PopStyleVar();
		ImGui::Dummy({ 0.0f, 3.0f });

		// Snapping helper
		auto snapValue = [](float value, float snapSize) -> float {
			if (snapSize <= 0.0f) return value;
			return std::round(value / snapSize) * snapSize;
		};

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 5, 2 });
		auto& transform = entity.GetComponent<TransformComponent>();

		TransformVec3Slider(editorState, "Position", transform.GetTranslation(), [&](glm::vec3 vector) {
				if (editorState->temp.snapToGrid) {
					vector.x = snapValue(vector.x, editorState->temp.snapPositionValue);
					vector.y = snapValue(vector.y, editorState->temp.snapPositionValue);
					vector.z = snapValue(vector.z, editorState->temp.snapPositionValue);
				}
				transform.SetTranslation(vector);
			}
		);

		TransformVec3Slider(editorState, "Rotation", transform.GetRotation(), [&](glm::vec3 vector) {
				if (editorState->temp.snapToGrid) {
					vector.x = snapValue(vector.x, editorState->temp.snapRotationValue);
					vector.y = snapValue(vector.y, editorState->temp.snapRotationValue);
					vector.z = snapValue(vector.z, editorState->temp.snapRotationValue);
				}
				transform.SetRotation(vector);
			}
		);

		TransformVec3Slider(editorState, "Scale", transform.GetScale(), [&](glm::vec3 vector) {
				if (editorState->temp.snapToGrid) {
					vector.x = snapValue(vector.x, editorState->temp.snapScaleValue);
					vector.y = snapValue(vector.y, editorState->temp.snapScaleValue);
					vector.z = snapValue(vector.z, editorState->temp.snapScaleValue);
				}
				transform.SetScale(vector);
			},
			1.0f
		);
		ImGui::PopStyleVar();
	}
}#pragma once

#include "X3.h"
#include "EditorState.h"
#include <imgui_internal.h>
#include "ImGuiContextFontRegistry.h"

namespace X3
{

	// Since the transform component can only get and set values through its own functions, we need to pass the set function as a lambda
	template <typename T>
	void TransformVec3Slider(std::shared_ptr<EditorState> editorState, 
							 const char* label, 
							 glm::vec3 vector, 
							 const T& setVector,
							 float resetVal = 0.0f) {

		EditorTheme& theme = editorState->temp.editorTheme;

		ImGui::AlignTextToFramePadding();
		ImGui::Columns(2);
		// width of the 1st column (labels)
		ImGui::SetColumnWidth(0, 100.0f);
		float lineheight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
		ImVec2 btnSize = ImVec2(lineheight * 0.7, lineheight);

		theme.PushColor(ImGuiCol_Text, EditorCol_Text2);
		ImGui::Text(label);
		theme.PopColor();

		ImGui::NextColumn();
		ImGui::PushMultiItemsWidths(3, ImGui::GetContentRegionAvail().x - 30);

		ImGui::PushID(label);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 0, 0 });

		theme.PushColor(ImGuiCol_ButtonActive, EditorCol_Secondary2);
		{
			theme.PushColor(ImGuiCol_Button, EditorCol_X);
			theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_X);
			{
				ImGui::SetNextItemWidth(lineheight);
				theme.PushColor(ImGuiCol_Text, EditorCol_Secondary2);
				ImGui::PushFont(Fonts()->notoSansBold);
				if (ImGui::Button("X", btnSize)) {
					setVector(glm::vec3(resetVal, vector.y, vector.z));
				}
				ImGui::PopFont();
				theme.PopColor();
				ImGui::SameLine();
				if (ImGui::DragFloat("##X", &vector.x, 0.5f)) {
					setVector(vector);
				}
				ImGui::SameLine();
				ImGui::PopItemWidth();
			}
			theme.PopColor(2); // Button, ButtonHovered
			theme.PushColor(ImGuiCol_Button, EditorCol_Y);
			theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_Y);
			{
				ImGui::SetNextItemWidth(lineheight);
				theme.PushColor(ImGuiCol_Text, EditorCol_Secondary2);
				ImGui::PushFont(Fonts()->notoSansBold);
				if (ImGui::Button("Y", btnSize)) {
					setVector(glm::vec3(vector.x, resetVal, vector.z));
				}
				ImGui::PopFont();
				theme.PopColor();
				ImGui::SameLine();
				if (ImGui::DragFloat("##Y", &vector.y, 0.5f)) {
					setVector(vector);
				}
				ImGui::SameLine();
				ImGui::PopItemWidth();
			}
			theme.PopColor(2); // Button, ButtonHovered
			theme.PushColor(ImGuiCol_Button, EditorCol_Z);
			theme.PushColor(ImGuiCol_ButtonHovered, EditorCol_Z);
			{
				ImGui::SetNextItemWidth(lineheight);
				theme.PushColor(ImGuiCol_Text, EditorCol_Secondary2);
				ImGui::PushFont(Fonts()->notoSansBold);
				if (ImGui::Button("Z", btnSize)) {
					setVector(glm::vec3(vector.x, vector.y, resetVal));
				}
				ImGui::PopFont();
				theme.PopColor();
				ImGui::SameLine();
				if (ImGui::DragFloat("##Z", &vector.z, 0.5f)) {
					setVector(vector);
				}
				ImGui::PopItemWidth();
			}
			theme.PopColor(2); // Button, ButtonHovered
		}
		theme.PopColor(); // ButtonActive

		ImGui::PopStyleVar();
		ImGui::PopID();
		ImGui::Columns(1);
	}

	inline void DrawTransformSliders(std::shared_ptr<EditorState> editorState, EntityHandle entity) {
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{ 5, 2 });
		auto& transform = entity.GetComponent<TransformComponent>();
		TransformVec3Slider(editorState, "Position", transform.GetTranslation(), [&transform](glm::vec3 vector) {
				transform.SetTranslation(vector); 
			}
		);

		TransformVec3Slider(editorState, "Rotation", transform.GetRotation(), [&transform](glm::vec3 vector) {
				transform.SetRotation(vector); 
			}
		);

		TransformVec3Slider(editorState, "Scale", transform.GetScale(), [&transform](glm::vec3 vector) {
				transform.SetScale(vector);
			}, 
			1.0f
		);
		ImGui::PopStyleVar();
	}
}