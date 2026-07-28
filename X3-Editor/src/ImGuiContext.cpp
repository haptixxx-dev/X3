#include "lrpch.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <vulkan/vulkan.h>
#include <imgui_impl_vulkan.h>
#include "Platform/Vulkan/VulkanContext.h"
#include <ImGuizmo.h>
#include <IconsFontAwesome6.h>
#include <IconsFontAwesome6Brands.h>
#include <GLFW/glfw3.h>
#include <implot.h>
#include "ImGuiContext.h"
#include "EditorCfg.h"
#include "Core/IWindow.h"

namespace X3
{
namespace {

    // ImGui copies ImGui_ImplVulkan_InitInfo by value, but
    // PipelineRenderingCreateInfo keeps a POINTER to the colour-format array and
    // dereferences it at every pipeline creation, not only at init
    // (imgui_impl_vulkan.cpp:975). A local would dangle. There is exactly one
    // ImGui Vulkan backend per process, so file scope is the correct lifetime.
    VkFormat g_ImGuiColorFormat = VK_FORMAT_UNDEFINED;

    // One builder for both call sites: Init() and the full re-init that a
    // swapchain recreation forces. They drifted apart before -- the re-init path
    // silently omitted MinImageCount handling -- and a second copy of a struct
    // this fiddly is a bug waiting to happen.
    ImGui_ImplVulkan_InitInfo MakeImGuiVulkanInitInfo(VulkanContext* vkContext) {
        g_ImGuiColorFormat = vkContext->getSwapchainImageFormat();

        ImGui_ImplVulkan_InitInfo info{};
        info.Instance       = vkContext->getInstance();
        info.PhysicalDevice = vkContext->getPhysicalDevice();
        info.Device         = vkContext->getDevice();
        info.QueueFamily    = vkContext->getGraphicsQueueFamily();
        info.Queue          = vkContext->getGraphicsQueue();
        info.PipelineCache  = VK_NULL_HANDLE;
        info.DescriptorPool = vkContext->getDescriptorPool();

        // Dynamic rendering: there is no VkRenderPass in the engine any more, so
        // there is none to hand ImGui. RenderPass is ignored when
        // UseDynamicRendering is set (imgui_impl_vulkan.h:79).
        info.RenderPass          = VK_NULL_HANDLE;
        info.Subpass             = 0;
        info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo       = VkPipelineRenderingCreateInfoKHR{};
        info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        // pNext MUST stay null -- ImGui asserts on it (imgui_impl_vulkan.cpp:974)
        // because it chains the struct into its own pipeline create info.
        info.PipelineRenderingCreateInfo.pNext                   = nullptr;
        info.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
        info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_ImGuiColorFormat;

        // Both counts come from the swapchain so ImGui's per-frame buffers align
        // with the images it will actually render into.
        const uint32_t swapchainImageCount = vkContext->getSwapchainImageCount();
        info.ImageCount    = swapchainImageCount;
        info.MinImageCount = swapchainImageCount;
        info.MSAASamples   = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = [](VkResult err) {
            if (err != VK_SUCCESS) {
                LOG_ENGINE_ERROR("ImGui Vulkan error: {}", static_cast<int>(err));
            }
        };
        return info;
    }

}

    ImGuiContext::ImGuiContext(std::shared_ptr<IWindow> window)
        : m_Window(window)
        , m_FontRegistry(std::make_shared<ImGuiContextFontRegistry>())
        , m_ImGuiIniPath(EditorCfg::RESOURCES_PATH / "imgui.ini")
        , m_DefaultImGuiIniPath(EditorCfg::RESOURCES_PATH / "default_imgui.ini")
        , m_LoadDefaultLayoutBeforeNewFrame(false)
    {}

    ImGuiContext::~ImGuiContext() {
        ImPlot::DestroyContext();

        // Shutdown the Vulkan backend
        ImGui_ImplVulkan_Shutdown();

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
        // Vulkan multi-viewport needs per-viewport swapchains, render passes and
        // framebuffers - Phase 13. ImGuiConfigFlags_ViewportsEnable stays unset.
        //io.ConfigViewportsNoAutoMerge = true;
        //io.ConfigViewportsNoTaskBarIcon = true;


        // Setup Dear ImGui style
        ImGui::StyleColorsDark();

        // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
        ImGuiStyle& style = ImGui::GetStyle();
        // Dead while multi-viewport is disabled (Phase 13 re-enable point).
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

        ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(m_Window->getNativeWindow()), true);

        // Setup Vulkan init info
        VulkanContext* vkContext = VulkanContext::Get();
        ImGui_ImplVulkan_InitInfo init_info = MakeImGuiVulkanInitInfo(vkContext);

        if (!ImGui_ImplVulkan_Init(&init_info)) {
            LOG_ENGINE_CRITICAL("Failed to initialize ImGui Vulkan backend!");
        }

        // Upload fonts - this creates the font texture and descriptor set
        if (!ImGui_ImplVulkan_CreateFontsTexture()) {
            LOG_ENGINE_CRITICAL("Failed to create ImGui font texture!");
        }

        // Wait for font upload to complete
        vkDeviceWaitIdle(vkContext->getDevice());
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

        // Check if swapchain was recreated - requires full ImGui Vulkan re-init
        // (SetMinImageCount alone doesn't recreate pipeline layout which becomes stale)
        VulkanContext* vkContext = VulkanContext::Get();
        if (vkContext && vkContext->wasSwapchainRecreated()) {
            // Wait for GPU to finish using old resources
            vkDeviceWaitIdle(vkContext->getDevice());

            // Full shutdown and re-init of Vulkan backend
            ImGui_ImplVulkan_Shutdown();

            // Re-create init info with current state. The swapchain FORMAT can
            // change across a recreation (a monitor change, an HDR toggle), and
            // ImGui's pipeline bakes it in, which is the whole reason this path
            // does a full re-init rather than SetMinImageCount.
            ImGui_ImplVulkan_InitInfo init_info = MakeImGuiVulkanInitInfo(vkContext);

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
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
    }

    void ImGuiContext::EndFrame() {
        ImGui::Render();

        VulkanContext* vkContext = VulkanContext::Get();

        // Application::run owns the frame. EndFrame no longer starts one; if there
        // is no frame the acquire failed this iteration and there is nothing to
        // record into.
        if (!vkContext->frameActive()) {
            return;
        }

        // ImGui is the last recorder in the editor frame and owns THE one
        // rendering block. Opening it here rather than in beginFrame() is what
        // keeps the compute dispatch above at top level, where vkCmdDispatch is
        // legal (VUID-vkCmdDispatch-renderpass).
        //
        // The block clears: the editor draws the path-traced image as an ImGui
        // texture inside the viewport panel, so nothing of the previous frame's
        // swapchain contents is wanted, and LOAD_OP_LOAD would introduce the exact
        // read-after-write hazard the old overlay pass had.
        vkContext->beginSwapchainRendering();

        VkCommandBuffer cmd = vkContext->getCurrentCommandBuffer();
        assert(cmd != VK_NULL_HANDLE && "Command buffer is null");

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        vkContext->endSwapchainRendering();

        ImGuiIO& io = ImGui::GetIO();
        // Dead while multi-viewport is disabled (Phase 13 re-enable point).
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
