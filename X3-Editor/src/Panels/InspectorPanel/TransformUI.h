#pragma once

#include "Laura.h"
#include "EditorState.h"
#include <imgui_internal.h>
#include "ImGuiContextFontRegistry.h"

namespace Laura
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