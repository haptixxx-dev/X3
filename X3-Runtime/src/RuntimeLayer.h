#pragma once

#include <X3.h>
#include <Export/ExportSettings.h>
#ifdef X3_USE_OPENGL
#include <GL/glew.h>
#endif
#ifdef X3_USE_VULKAN
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanImage2D.h"
#endif
#include <chrono>

namespace X3
{

	class RuntimeLayer : public ILayer {
	public:
		RuntimeLayer(std::shared_ptr<IWindow> window,
					std::shared_ptr<Profiler> profiler,
					std::shared_ptr<IEventDispatcher> eventDispatcher,
					std::shared_ptr<ProjectManager> projectManager
		);

		virtual void onAttach() override;
		virtual void onDetach() override;
		virtual void onUpdate() override;
		virtual void onEvent(std::shared_ptr<IEvent> event) override;

	#ifdef X3_USE_OPENGL
		bool LoadLogoFromDisk(unsigned int* out_texture, int* out_width, int* out_height);
	#endif

	private:
		void CalculateViewportCoordinates();
	#ifdef X3_USE_OPENGL
		bool InitLogoResources();
		void DestroyLogoResources();
		void RenderLogo(float alpha);
	#endif
		// Engine Systems
		std::shared_ptr<IWindow> m_Window;
		std::shared_ptr<Profiler> m_Profiler;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher; // layerstack
		std::shared_ptr<ProjectManager> m_ProjectManager;

		std::shared_ptr<IImage2D> m_CurrentFrame;
		unsigned int m_Framebuffer = 0;

		ExportSettings m_ExportSettings;

		// Viewport scaling variables
		glm::ivec4 m_ViewportCoords; // x, y, width, height for glBlitFramebuffer
		glm::ivec2 m_WindowSize;
		bool m_UpdateViewportCoordinates;

	#ifdef X3_USE_OPENGL
		// splash screen (OpenGL only for now)
		bool m_ShowLogoScreen;
		int m_LogoWidth, m_LogoHeight;
		unsigned int m_LogoTexHandle = 0;
		unsigned int m_LogoVAO = 0, m_LogoVBO = 0, m_LogoProgram = 0;
		int m_LogoUniformLocationAlpha = -1, m_LogoUniformLocationSampler = -1;
		std::chrono::steady_clock::time_point m_SplashStartTime;
	#endif
	};
}