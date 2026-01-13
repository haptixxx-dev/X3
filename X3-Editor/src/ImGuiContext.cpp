#include "lrpch.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#ifdef X3_USE_OPENGL
#include <imgui_impl_opengl3.h>
#endif
#ifdef X3_USE_VULKAN
#include <vulkan/vulkan.h>
#include <imgui_impl_vulkan.h>
#include "Platform/Vulkan/VulkanContext.h"
#endif
#include <ImGuizmo.h>
#include <IconsFontAwesome6.h>
#include <IconsFontAwesome6Brands.h>
#include <GLFW/glfw3.h>
#include <implot.h>
#include "ImGuiContext.h"
#include "EditorCfg.h"
#include "Core/IWindow.h"
#include "Renderer/IRendererAPI.h"

namespace X3 
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

        // Shutdown the correct backend
    #ifdef X3_USE_VULKAN
        ImGui_ImplVulkan_Shutdown();
    #else
        ImGui_ImplOpenGL3_Shutdown();
    #endif

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

		// X3 BRAND ICONS 
        cfg.MergeMode = false;
        cfg.GlyphOffset = ImVec2(0.0f, -2.0f);
        static const ImWchar X3BrandIcons_ranges[] = { 0xF101, 0xF103, 0 };
		m_FontRegistry->X3BrandIcons = io.Fonts->AddFontFromFileTTF(
			(EditorCfg::RESOURCES_PATH / "Fonts/X3-brand-icons/X3-brand-icons.ttf").string().c_str(),
			22.0f, &cfg, X3BrandIcons_ranges 
		);

        ImGui::GetIO().UserData = m_FontRegistry.get();

        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
    #ifndef X3_USE_VULKAN
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows (OpenGL only)
    #endif
        // Note: Vulkan multi-viewport requires per-viewport swapchains, render passes, and framebuffers
        // which are not implemented. Viewports are disabled for Vulkan to prevent crashes.
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

        // Initialize the correct backend based on renderer API
    #ifdef X3_USE_VULKAN
        ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(m_Window->getNativeWindow()), true);

        // Setup Vulkan init info
        VulkanContext* vkContext = VulkanContext::Get();
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = vkContext->getInstance();
        init_info.PhysicalDevice = vkContext->getPhysicalDevice();
        init_info.Device = vkContext->getDevice();
        init_info.QueueFamily = vkContext->getGraphicsQueueFamily();
        init_info.Queue = vkContext->getGraphicsQueue();
        init_info.PipelineCache = VK_NULL_HANDLE;
        init_info.DescriptorPool = vkContext->getDescriptorPool();
        // Use overlay render pass - ImGui renders after blitImageToSwapchain which ends the main render pass
        init_info.RenderPass = vkContext->getOverlayRenderPass();
        init_info.Subpass = 0;
        init_info.MinImageCount = vkContext->getMinImageCount();
        // Use MAX_FRAMES_IN_FLIGHT for ImageCount to match fence synchronization
        // This ensures ImGui's per-frame buffers align with our frame pacing
        uint32_t swapchainImageCount = vkContext->getSwapchainImageCount();
        init_info.ImageCount    = swapchainImageCount;
        init_info.MinImageCount = swapchainImageCount;
        // init_info.ImageCount = vkContext->getMaxFramesInFlight();
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = [](VkResult err) {
            if (err != VK_SUCCESS) {
                LOG_ENGINE_ERROR("ImGui Vulkan error: {}", static_cast<int>(err));
            }
        };

        if (!ImGui_ImplVulkan_Init(&init_info)) {
            LOG_ENGINE_CRITICAL("Failed to initialize ImGui Vulkan backend!");
        }

        // Upload fonts - this creates the font texture and descriptor set
        if (!ImGui_ImplVulkan_CreateFontsTexture()) {
            LOG_ENGINE_CRITICAL("Failed to create ImGui font texture!");
        }

        // Wait for font upload to complete
        vkDeviceWaitIdle(vkContext->getDevice());
    #else
        ImGui_ImplGlfw_InitForOpenGL(static_cast<GLFWwindow*>(m_Window->getNativeWindow()), true);
        ImGui_ImplOpenGL3_Init("#version 460");
    #endif
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

    #ifdef X3_USE_VULKAN
        // Check if swapchain was recreated - requires full ImGui Vulkan re-init
        // (SetMinImageCount alone doesn't recreate pipeline layout which becomes stale)
        VulkanContext* vkContext = VulkanContext::Get();
        if (vkContext && vkContext->wasSwapchainRecreated()) {
            // Wait for GPU to finish using old resources
            vkDeviceWaitIdle(vkContext->getDevice());

            // Full shutdown and re-init of Vulkan backend
            ImGui_ImplVulkan_Shutdown();

            // Re-create init info with current state
            ImGui_ImplVulkan_InitInfo init_info = {};
            init_info.Instance = vkContext->getInstance();
            init_info.PhysicalDevice = vkContext->getPhysicalDevice();
            init_info.Device = vkContext->getDevice();
            init_info.QueueFamily = vkContext->getGraphicsQueueFamily();
            init_info.Queue = vkContext->getGraphicsQueue();
            init_info.PipelineCache = VK_NULL_HANDLE;
            init_info.DescriptorPool = vkContext->getDescriptorPool();
            init_info.RenderPass = vkContext->getOverlayRenderPass();
            init_info.Subpass = 0;
            uint32_t swapchainImageCount = vkContext->getSwapchainImageCount();
            init_info.ImageCount = swapchainImageCount;
            init_info.MinImageCount = swapchainImageCount;
            init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            init_info.CheckVkResultFn = [](VkResult err) {
                if (err != VK_SUCCESS) {
                    LOG_ENGINE_ERROR("ImGui Vulkan error: {}", static_cast<int>(err));
                }
            };

            if (!ImGui_ImplVulkan_Init(&init_info)) {
                LOG_ENGINE_CRITICAL("Failed to re-initialize ImGui Vulkan backend after swapchain recreation!");
            }

            // Recreate font texture
            if (!ImGui_ImplVulkan_CreateFontsTexture()) {
                LOG_ENGINE_CRITICAL("Failed to recreate ImGui font texture!");
            }

            vkDeviceWaitIdle(vkContext->getDevice());
            vkContext->clearSwapchainRecreatedFlag();
            LOG_ENGINE_INFO("ImGui Vulkan backend re-initialized after swapchain recreation");
        }
        ImGui_ImplVulkan_NewFrame();
    #else
        ImGui_ImplOpenGL3_NewFrame();
    #endif
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    void ImGuiContext::EndFrame() {
        ImGui::Render();

    #ifdef X3_USE_VULKAN
        VulkanContext* vkContext = VulkanContext::Get();
        LOG_ENGINE_INFO("ImGui EndFrame: ensureFrameStarted");
        // Ensure a frame is started (handles first frame after ImGui init)
        vkContext->ensureFrameStarted();

        LOG_ENGINE_INFO("ImGui EndFrame: beginOverlayRenderPass");
        // ImGui MUST render in the overlay render pass (its pipeline was created for it)
        // End any active render pass and start the overlay render pass
        vkContext->beginOverlayRenderPass();

        VkCommandBuffer cmd = vkContext->getCurrentCommandBuffer();
        LOG_ENGINE_INFO("ImGui EndFrame: got command buffer {:p}", (void*)cmd);

        // Validate state before ImGui render (these should never fail if setup is correct)
        assert(cmd != VK_NULL_HANDLE && "Command buffer is null");
        assert(vkContext->isRenderPassActive() && "Render pass must be active for ImGui");

        LOG_ENGINE_INFO("ImGui EndFrame: calling RenderDrawData");
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        LOG_ENGINE_INFO("ImGui EndFrame: RenderDrawData complete");
    #else
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    #endif

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
