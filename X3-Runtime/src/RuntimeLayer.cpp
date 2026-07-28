#include "RuntimeLayer.h"
#include "RuntimeCfg.h"
#include <filesystem>
#include <algorithm>
#include "Core/Events/WindowEvents.h"

namespace X3
{
	RuntimeLayer::RuntimeLayer(std::shared_ptr<IWindow> window,
							   std::shared_ptr<Profiler> profiler,
							   std::shared_ptr<IEventDispatcher> eventDispatcher,
							   std::shared_ptr<ProjectManager> projectManager
	)
		: m_Window(window)
		, m_Profiler(profiler)
		, m_EventDispatcher(eventDispatcher)
		, m_ProjectManager(projectManager)
		, m_ViewportCoords(0, 0, 0, 0)
		, m_WindowSize(0, 0)
		, m_UpdateViewportCoordinates(false)
	{}

	void RuntimeLayer::onAttach() {
		m_ExportSettings = DeserializeExportSettingsYaml(RuntimeCfg::EXECUTABLE_DIR).value_or(ExportSettings{});
		m_Window->setVSync(m_ExportSettings.vSync);
		m_Window->setFullscreen(m_ExportSettings.fullscreen);

		m_WindowSize = m_Window->getFrameBufferSize();
		m_UpdateViewportCoordinates = true;

		std::filesystem::path projectFilePath = "";
		for (const auto& entry : std::filesystem::directory_iterator(RuntimeCfg::EXECUTABLE_DIR)) {
			if (entry.is_regular_file()) {
				auto path = entry.path();
				if (path.extension() == PROJECT_FILE_EXTENSION) {
					projectFilePath = path;
				}
			}
		}
		m_ProjectManager->OpenProject(projectFilePath);
		m_Window->setTitle(m_ProjectManager->GetProjectName());

		m_EventDispatcher->dispatchEvent(std::make_shared<UpdateRenderSettingsEvent>(m_ProjectManager->GetMutableRuntimeRenderSettings()));
	}

	void RuntimeLayer::onDetach() {}

	void RuntimeLayer::onUpdate() {
		if (m_CurrentFrame) {
			CalculateViewportCoordinates();

			auto context = VulkanContext::Get();
			// The image tracks its own layout now, so the blit takes it through
			// GENERAL -> TRANSFER_SRC_OPTIMAL -> GENERAL itself; nothing here has
			// to remember which layout it is in.
			if (const FrameContext* frame = context ? context->currentFrame() : nullptr) {
				context->blitImageToSwapchain(*frame, *m_CurrentFrame, m_ViewportCoords, m_WindowSize);
			}
		}
	}

	void RuntimeLayer::onEvent(std::shared_ptr<IEvent> event) {
		if (event->GetType() == EventType::NEW_FRAME_RENDERED_EVENT) {
			m_CurrentFrame = std::dynamic_pointer_cast<NewFrameRenderedEvent>(event)->frame;
		}
		if (event->GetType() == EventType::KEY_PRESS_EVENT) {
			if (std::dynamic_pointer_cast<KeyPressEvent>(event)->key == Key::F11) {
				m_Window->setFullscreen(!m_Window->isFullscreen());
			}
		}
		if (event->GetType() == EventType::WINDOW_RESIZE_EVENT) {
			std::cout << "resize event" << std::endl;
			m_WindowSize = std::dynamic_pointer_cast<WindowResizeEvent>(event)->windowSize;
			m_UpdateViewportCoordinates = true;
		}
	}

	void RuntimeLayer::CalculateViewportCoordinates() {
		if (!m_CurrentFrame || !m_UpdateViewportCoordinates) {
			return;
		}
		m_UpdateViewportCoordinates = false;

		glm::ivec2 imageSize = m_CurrentFrame->dimensions();
		// m_WindowSize set on WINDOW_RESIZE_EVENT

		switch (m_ExportSettings.screenFitMode) {
			case ScreenFitMode::OriginalCentered: {
				int offsetX = (m_WindowSize.x - imageSize.x) / 2;
				int offsetY = (m_WindowSize.y - imageSize.y) / 2;

				// Ensure positive (in case image is larger than window)
				offsetX = std::max(0, offsetX);
				offsetY = std::max(0, offsetY);

				m_ViewportCoords = glm::ivec4(
					offsetX,                    // x
					offsetY,                    // y
					offsetX + imageSize.x,      // width
					offsetY + imageSize.y       // height
				);
				break;
			}

			case ScreenFitMode::StretchFill: {
				m_ViewportCoords = glm::ivec4(
					0,              // x
					0,              // y
					m_WindowSize.x,   // width
					m_WindowSize.y    // height
				);
				break;
			}

			case ScreenFitMode::MaxAspectFit: {
				float windowAspectRatio = static_cast<float>(m_WindowSize.x) / static_cast<float>(m_WindowSize.y);
				float imageAspectRatio = static_cast<float>(imageSize.x) / static_cast<float>(imageSize.y);

				int targetWidth, targetHeight;
				if (windowAspectRatio <= imageAspectRatio) {
					// Width is the limiting factor
					targetWidth = m_WindowSize.x;
					targetHeight = static_cast<int>(std::ceil(m_WindowSize.x / imageAspectRatio));
				} else {
					// Height is the limiting factor
					targetWidth = static_cast<int>(std::ceil(m_WindowSize.y * imageAspectRatio));
					targetHeight = m_WindowSize.y;
				}

				// Center the scaled image
				int offsetX = (m_WindowSize.x - targetWidth) / 2;
				int offsetY = (m_WindowSize.y - targetHeight) / 2;

				m_ViewportCoords = glm::ivec4(
					offsetX,                // x
					offsetY,                // y
					offsetX + targetWidth,  // width
					offsetY + targetHeight  // height
				);
				break;
			}
			default:
				// Fallback to MaxAspectFit
				m_ExportSettings.screenFitMode = ScreenFitMode::MaxAspectFit;
				CalculateViewportCoordinates();
				break;
		}
	}
}
