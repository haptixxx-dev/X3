#pragma once

#include "Laura.h"
#include "EditorState.h"

namespace Laura
{

	/*
		Executes the OnConfirm function. If "doubleConfirmEnabled" is enabled in the editor state, 
		a popup will prompt the user for confirmation first. To use this with a button click, 
		set a bool to true and pass it as the "execute" parameter. This bool ensures the popup 
		shows, as calling this function directly in the button's if clause won't trigger it.
	*/
	template <typename OnConfirm>
	void ConfirmAndExecute(	bool& shouldExecute, 
							const char* popupTitle,		
							const char* popupMessage, 
							OnConfirm onConfirm, 
							std::shared_ptr<EditorState> editorState) {
		if (!shouldExecute) {
			return;
		}
		
		EditorTheme& theme = editorState->temp.editorTheme;
		ImGui::OpenPopup(popupTitle);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
		
		ImGui::SetNextWindowSizeConstraints({ 300.0f, 0.0f }, { 400.0f, FLT_MAX });
		theme.PushColor(ImGuiCol_PopupBg, EditorCol_Background1);
		if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
			ImGui::TextWrapped(popupMessage);
			theme.PopColor();

			float panelWidth = ImGui::GetContentRegionAvail().x;
			float buttonWidth = panelWidth * 0.5f - 5.0f;
			ImGui::Dummy({ 5.0f, 0.0f });
			
			theme.PushColor(ImGuiCol_Button, EditorCol_Primary3);
			if (ImGui::Button("Yes", ImVec2(buttonWidth, 0))) {
				onConfirm();
				shouldExecute = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::SameLine();
			if (ImGui::Button("No", ImVec2(buttonWidth, 0))) {
				shouldExecute = false;
				ImGui::CloseCurrentPopup();
			}
			theme.PopColor();
			ImGui::EndPopup();
		}
		theme.PopColor();
		ImGui::PopStyleVar();
	}

	template <typename OnAccept, typename OnReject>
	void ConfirmWithCancel(
		bool& shouldExecute,
		const char* popupTitle,
		const char* popupMessage,
		const char* acceptText,
		const char* rejectText,
		const char* cancelText,
		OnAccept onAccept,
		OnReject onReject,
		std::shared_ptr<EditorState> editorState,
		std::function<void()> afterPopup = [](){})
	{
		if (!shouldExecute) {
			return;
		}

		auto theme = editorState->temp.editorTheme;
		bool cancelWasCalled = false;

		ImGui::OpenPopup(popupTitle);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
		float spacing = 5.0f;
		float widestText = std::max({
			ImGui::CalcTextSize(acceptText).x,
			ImGui::CalcTextSize(rejectText).x,
			ImGui::CalcTextSize(cancelText).x
		});
		float buttonWidth = widestText + ImGui::GetStyle().FramePadding.x * 2.0f;
		
		ImGui::SetNextWindowSize({ buttonWidth * 3.0f + spacing * 4.0f, 0.0f });
		theme.PushColor(ImGuiCol_PopupBg, EditorCol_Background1);
		if (ImGui::BeginPopupModal(popupTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			theme.PushColor(ImGuiCol_Text, EditorCol_Warning);
			ImGui::TextWrapped(popupMessage);
			theme.PopColor();

			theme.PushColor(ImGuiCol_Button, EditorCol_Primary3);
			ImGui::Dummy(ImVec2(0.0f, 5.0f));
			float cursorX = spacing;
			ImGui::SetCursorPosX(cursorX);
			if (ImGui::Button(acceptText, ImVec2(buttonWidth, 0))) {
				onAccept();
				shouldExecute = false;
				ImGui::CloseCurrentPopup();
			}

			cursorX += spacing + buttonWidth;
			ImGui::SameLine();
			ImGui::SetCursorPosX(cursorX);
			if (ImGui::Button(rejectText, ImVec2(buttonWidth, 0))) {
				onReject();
				shouldExecute = false;
				ImGui::CloseCurrentPopup();
			}

			cursorX += spacing + buttonWidth;
			ImGui::SameLine();
			ImGui::SetCursorPosX(cursorX);
			if (ImGui::Button(cancelText, ImVec2(buttonWidth, 0))) {
				shouldExecute = false;
				cancelWasCalled = true;
				ImGui::CloseCurrentPopup();
			}
			theme.PopColor();
			ImGui::EndPopup();
		}
		theme.PopColor();
		ImGui::PopStyleVar();

		if (!shouldExecute && !cancelWasCalled) { // after the flag has been reset by the popup && cancel was called
			afterPopup();
		}
	}
}
