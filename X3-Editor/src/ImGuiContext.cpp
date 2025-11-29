#include "lrpch.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <IconsFontAwesome6.h>
#include <IconsFontAwesome6Brands.h>
#include <GLFW/glfw3.h>
#include <implot.h>
#include "ImGuiContext.h"
#include "EditorCfg.h"
#include "Core/IWindow.h"

namespace Laura 
{

    ImGuiContext::ImGuiContext(std::shared_ptr<IWindow> window)
        : m_Window(window)
        , m_FontRegistry(std::make_shared<ImGuiContextFontRegistry>())
        , m_ImGuiIniPath(EditorCfg::RESOURCES_PATH / "imgui.ini")
        , m_DefaultImGuiIniPath(EditorCfg::RESOURCES_PATH / "default_imgui.ini")
        , m_LoadDefaultLayoutBeforeNewFrame(false)
    {}

    ImGuiContext::~ImGuiContext() {
        ImPlot::DestroyContext();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    void ImGuiContext::Init() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();


        // .INI FILE
        io.IniFilename = NULL; // ensure custom management for .ini files
		if (!std::filesystem::exists(m_ImGuiIniPath)) {
			if (std::filesystem::exists(m_DefaultImGuiIniPath)) {
				std::error_code ec;
				std::filesystem::copy_file(m_DefaultImGuiIniPath, m_ImGuiIniPath, std::filesystem::copy_options::overwrite_existing, ec);
                if (!ec)    { LOG_EDITOR_TRACE("ImGuiContext::Init(): Copied {0}", m_DefaultImGuiIniPath.string()); }
                else        { LOG_EDITOR_CRITICAL("ImGuiContext::Init(): Failed to copy default_imgui.ini: {0}", ec.message()); }
			} 
            else { LOG_EDITOR_CRITICAL("ImGuiContext::Init(): default_imgui.ini missing {0}", m_DefaultImGuiIniPath.string()); }
		}
        ImGui::LoadIniSettingsFromDisk(m_ImGuiIniPath.string().c_str());
        LOG_EDITOR_TRACE("ImGuiContext::Init(): Loaded .ini file {0}", m_ImGuiIniPath.string());


        // FONTS
		static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
		// DEFAULT FONT
		ImFontConfig cfg;
		cfg.PixelSnapH = true;
		cfg.MergeMode = false;
		m_FontRegistry->notoSansRegular = io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/Noto_Sans/NotoSans-Regular.ttf").string().c_str(),
			16.0f, &cfg
		);
		cfg.MergeMode = true; // merge icons
		io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/fontawesome-free-6.6.0-desktop/Font Awesome 6 Free-Solid-900.otf").string().c_str(),
			13.0f, &cfg, iconRanges
		);
        io.FontDefault = m_FontRegistry->notoSansRegular;

		// HIGH RES ICONS (standalone, not merged)
		cfg.MergeMode = false;
		m_FontRegistry->highResIcons = io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/fontawesome-free-6.6.0-desktop/Font Awesome 6 Free-Solid-900.otf").string().c_str(),
			40.0f, &cfg, iconRanges
		);

		// NOTOSANS BOLD
		cfg.MergeMode = false;
		m_FontRegistry->notoSansBold = io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/Noto_Sans/NotoSans-SemiBold.ttf").string().c_str(),
			16.0f, &cfg
		);
		cfg.MergeMode = true; // merge icons into bold
		io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/fontawesome-free-6.6.0-desktop/Font Awesome 6 Free-Solid-900.otf").string().c_str(),
			13.0f, &cfg, iconRanges
		);

		// CODICON 
        cfg.MergeMode = false;
        cfg.GlyphOffset = ImVec2(0.0f, -25.0f);
        static const ImWchar codicon_ranges[] = { 0xF101, 0xF2FF, 0 };
		m_FontRegistry->codicon = io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/vscode-codicon/vscode-codicon.ttf").string().c_str(),
			40.0f, &cfg, codicon_ranges
		);

		// LAURA BRAND ICONS 
        cfg.MergeMode = false;
        cfg.GlyphOffset = ImVec2(0.0f, -2.0f);
        static const ImWchar lauraBrandIcons_ranges[] = { 0xF101, 0xF103, 0 };
		m_FontRegistry->lauraBrandIcons = io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/laura-brand-icons/laura-brand-icons.ttf").string().c_str(),
			22.0f, &cfg, lauraBrandIcons_ranges 
		);

        ImGui::GetIO().UserData = m_FontRegistry.get();

        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
        //io.ConfigViewportsNoAutoMerge = true;
        //io.ConfigViewportsNoTaskBarIcon = true;


        // Setup Dear ImGui style
        ImGui::StyleColorsDark();

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
        ImGuiStyle& style = ImGui::GetStyle();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            style.WindowRounding = 0.0f;
            style.TabRounding = 0.0f;
            style.TabBarBorderSize = 0.0f;
            style.GrabRounding = 2.0f;
            style.ScrollbarRounding = 2.0f;
            style.DockingSeparatorSize = 0.0f;
            style.WindowBorderSize = 0.0f;
        }

        style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_None; // remove the menu button from the titlebar

        ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(m_Window->getNativeWindow()), true); // true: install callbacks in the GLFW window
        ImGui_ImplOpenGL3_Init("#version 460");
    }

    void ImGuiContext::LoadDefaultLayout() {
        m_LoadDefaultLayoutBeforeNewFrame = true;
    }

    void ImGuiContext::BeginFrame() {
        if (m_LoadDefaultLayoutBeforeNewFrame) {
            m_LoadDefaultLayoutBeforeNewFrame = false;
			if (std::filesystem::exists(m_DefaultImGuiIniPath)) {
				ImGui::LoadIniSettingsFromDisk(m_DefaultImGuiIniPath.string().c_str());
			}
			else { LOG_EDITOR_CRITICAL("ImGuiContext::Init(): default_imgui.ini missing {0}", m_DefaultImGuiIniPath.string()); }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiContext::EndFrame() {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup_current_context = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup_current_context);
        }
    }

    void ImGuiContext::Shutdown() {
        if (std::filesystem::exists(m_ImGuiIniPath)) {
            ImGui::SaveIniSettingsToDisk(m_ImGuiIniPath.string().c_str());
        }
    }

}
