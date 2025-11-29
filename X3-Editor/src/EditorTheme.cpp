#include "EditorTheme.h"
#include <fstream>

namespace Laura
{

    std::pair<bool, std::string> EditorTheme::SerializeToYAML(const std::filesystem::path& filepath) {
        if (filepath.extension() != EDITOR_THEME_FILE_EXTENSION) {
            return { false, "Invalid file extension for theme file (or none selected): " + filepath.string() };
        }

        std::ofstream fout(filepath);
        if (!fout.is_open()) {
            return { false, "Could not open file for writing: " + filepath.string() };
        }

        YAML::Node node;
        try {
            node = YAML::convert<EditorTheme>::encode(*this);
        }
        catch (const YAML::RepresentationException& e) {
            return { false, "YAML representation error (make sure the file is valid): " + filepath.string() + ", error: " + e.what() };
        }
        catch (const std::exception& e) {
            return { false, "Unknown error occurred while saving file: " + filepath.string() + ", error: " + e.what() };
        }

        fout << node;
        fout.close();
        return { true, "" };
    }

    std::pair<bool, std::string> EditorTheme::DeserializeFromYAML(const std::filesystem::path& filepath) {
        if (filepath.extension() != EDITOR_THEME_FILE_EXTENSION) {
            return { false, "Invalid file extension for theme file (or none selected): " + filepath.string() };
        }

        YAML::Node node;
        try {
            node = YAML::LoadFile(filepath.string());
        }
        catch (const YAML::BadFile& e) {
            return { false, "Failed to load file: " + filepath.string() };
        }
        catch (const YAML::ParserException& e) {
            return { false, "Failed to parse file: " + filepath.string() };
        }
        catch (const std::exception& e) {
            return { false, "Unknown error occurred while loading file: " + filepath.string() + ", error: " + e.what() };
        }

        EditorTheme newEditorTheme;
        try {
            YAML::convert<EditorTheme>::decode(node, newEditorTheme);
        }
        catch (const YAML::RepresentationException& e) {
            return { false, "YAML representation error (make sure the file is valid): " + filepath.string() + ", error: " + e.what() };
        }
        catch (const std::exception& e) {
            return { false, "Unknown error occurred while loading file: " + filepath.string() + ", error: " + e.what() };
        }
        
        *this = std::move(newEditorTheme);

        return { true, "" };
    }

	void EditorTheme::LoadDefaultDark() {
		m_ColorPallete[EditorCol_Primary1]    = RGBA(77, 77, 79);
		m_ColorPallete[EditorCol_Primary2]    = RGBA(70, 70, 77);
		m_ColorPallete[EditorCol_Primary3]    = RGBA(30, 30, 30);
		m_ColorPallete[EditorCol_Secondary1]  = RGBA(20, 20, 20);
		m_ColorPallete[EditorCol_Secondary2]  = RGBA(55, 55, 61);
		m_ColorPallete[EditorCol_Accent1]     = RGBA(66, 150, 250);
		m_ColorPallete[EditorCol_Accent2]     = RGBA(96, 115, 181);
		m_ColorPallete[EditorCol_Text1]       = RGBA(255, 255, 255);
		m_ColorPallete[EditorCol_Text2]       = RGBA(128, 128, 128);
		m_ColorPallete[EditorCol_Background1] = RGBA(37, 37, 38);
		m_ColorPallete[EditorCol_Background2] = RGBA(30, 30, 30);
		m_ColorPallete[EditorCol_Background3] = RGBA(51, 51, 51);
		m_ColorPallete[EditorCol_Background4] = RGBA(0, 0, 0);
		m_ColorPallete[EditorCol_Error]       = RGBA(219, 72, 115);     // Errors
		m_ColorPallete[EditorCol_Warning]     = RGBA(213, 152, 87);     // Warnings
		m_ColorPallete[EditorCol_Success]     = RGBA(174, 243, 87);     // Success
		m_ColorPallete[EditorCol_X]           = RGBA(219, 72, 115);     // Transform axis X
		m_ColorPallete[EditorCol_Y]           = RGBA(174, 243, 87);     // Transform axis Y
		m_ColorPallete[EditorCol_Z]           = RGBA(118, 162, 250);     // Transform axis Z
		ApplyAllToImgui();
	}

  void EditorTheme::LoadDefaultLight() {
	m_ColorPallete[EditorCol_Primary1]    = RGBA(180, 180, 185);   // UI highlights
	m_ColorPallete[EditorCol_Primary2]    = RGBA(160, 160, 170);   // Hover backgrounds
	m_ColorPallete[EditorCol_Primary3]    = RGBA(210, 210, 210);   // Panel backgrounds
	m_ColorPallete[EditorCol_Secondary1]  = RGBA(225, 225, 225);   // Window background
	m_ColorPallete[EditorCol_Secondary2]  = RGBA(190, 190, 200);   // Inactive UI areas
	m_ColorPallete[EditorCol_Accent1]     = RGBA(90, 140, 200);    // Main accent blue (more subtle)
	m_ColorPallete[EditorCol_Accent2]     = RGBA(110, 110, 120);   // Minor accents
	m_ColorPallete[EditorCol_Text1]       = RGBA(30, 30, 30);      // Main text
	m_ColorPallete[EditorCol_Text2]       = RGBA(90, 90, 90);      // Disabled/secondary text
	m_ColorPallete[EditorCol_Background1] = RGBA(240, 240, 240);   // Window background
	m_ColorPallete[EditorCol_Background2] = RGBA(225, 225, 225);   // Group panels
	m_ColorPallete[EditorCol_Background3] = RGBA(200, 200, 200);   // Inner panels
	m_ColorPallete[EditorCol_Background4] = RGBA(255, 255, 255);      // Transparent
	m_ColorPallete[EditorCol_Error]       = RGBA(219, 72, 115);     // Errors
	m_ColorPallete[EditorCol_Warning]     = RGBA(213, 152, 87);     // Warnings
	m_ColorPallete[EditorCol_Success]     = RGBA(174, 243, 87);     // Success
	m_ColorPallete[EditorCol_X]           = RGBA(219, 72, 115);     // Transform axis X
	m_ColorPallete[EditorCol_Y]           = RGBA(174, 243, 87);     // Transform axis Y
	m_ColorPallete[EditorCol_Z]           = RGBA(118, 162, 250);     // Transform axis Z
	ApplyAllToImgui();
} 
    void EditorTheme::ApplyAllToImgui() {
		ImGuiStyle& style = ImGui::GetStyle();
		style.Colors[ImGuiCol_WindowBg]                  = m_ColorPallete[EditorCol_Background1];
		style.Colors[ImGuiCol_PopupBg]                   = m_ColorPallete[EditorCol_Background2];
		style.Colors[ImGuiCol_Border]                    = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_Header]                    = m_ColorPallete[EditorCol_Primary3];
		style.Colors[ImGuiCol_HeaderHovered]             = m_ColorPallete[EditorCol_Primary2];
		style.Colors[ImGuiCol_HeaderActive]              = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_Button]                    = m_ColorPallete[EditorCol_Primary3];
		style.Colors[ImGuiCol_ButtonHovered]             = m_ColorPallete[EditorCol_Primary1];
		style.Colors[ImGuiCol_ButtonActive]              = m_ColorPallete[EditorCol_Primary2];
		style.Colors[ImGuiCol_CheckMark]                 = m_ColorPallete[EditorCol_Text1];
		style.Colors[ImGuiCol_SliderGrab]                = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_SliderGrabActive]          = m_ColorPallete[EditorCol_Accent1];
		style.Colors[ImGuiCol_FrameBg]                   = m_ColorPallete[EditorCol_Primary3];
		style.Colors[ImGuiCol_FrameBgHovered]            = m_ColorPallete[EditorCol_Primary1];
		style.Colors[ImGuiCol_FrameBgActive]             = m_ColorPallete[EditorCol_Primary2];
		style.Colors[ImGuiCol_Tab]                       = m_ColorPallete[EditorCol_Background2];
		style.Colors[ImGuiCol_TabHovered]                = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_TabActive]                 = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_TabSelectedOverline]       = m_ColorPallete[EditorCol_Accent1];
		style.Colors[ImGuiCol_TabDimmedSelectedOverline] = m_ColorPallete[EditorCol_Primary1];
		style.Colors[ImGuiCol_TabUnfocused]              = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_TabUnfocusedActive]        = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_TableRowBg]				 = m_ColorPallete[EditorCol_Background2];
		style.Colors[ImGuiCol_TableRowBgAlt]			 = m_ColorPallete[EditorCol_Background1];
		style.Colors[ImGuiCol_TitleBg]                   = m_ColorPallete[EditorCol_Background2];
		style.Colors[ImGuiCol_TitleBgActive]             = m_ColorPallete[EditorCol_Background2];
		style.Colors[ImGuiCol_TitleBgCollapsed]          = m_ColorPallete[EditorCol_Background2];
		style.Colors[ImGuiCol_ScrollbarGrab]             = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_ResizeGrip]                = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_ResizeGripHovered]         = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_ResizeGripActive]          = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_Separator]                 = m_ColorPallete[EditorCol_Primary2];
		style.Colors[ImGuiCol_SeparatorHovered]          = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_SeparatorActive]           = m_ColorPallete[EditorCol_Secondary2];
		style.Colors[ImGuiCol_Text]                      = m_ColorPallete[EditorCol_Text1];
		style.Colors[ImGuiCol_TextDisabled]              = m_ColorPallete[EditorCol_Text2];
		style.Colors[ImGuiCol_MenuBarBg]                 = m_ColorPallete[EditorCol_Secondary1];
	}
}