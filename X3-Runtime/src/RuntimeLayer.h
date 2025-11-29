#pragma once

#include <X3.h>
#include <Export/ExportSettings.h>
#include <GL/glew.h>
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

		bool LoadLogoFromDisk(GLuint* out_texture, int* out_width, int* out_height);

	private:
		void CalculateViewportCoordinates();
		bool InitLogoResources();
		void DestroyLogoResources();
		void RenderLogo(float alpha);
		// Engine Systems
		std::shared_ptr<IWindow> m_Window;
		std::shared_ptr<Profiler> m_Profiler;
		std::shared_ptr<IEventDispatcher> m_EventDispatcher; // layerstack  
		std::shared_ptr<ProjectManager> m_ProjectManager;
		
		std::shared_ptr<IImage2D> m_CurrentFrame;
		unsigned int m_Framebuffer;

		ExportSettings m_ExportSettings;

		// Viewport scaling variables
		glm::ivec4 m_ViewportCoords; // x, y, width, height for glBlitFramebuffer
		glm::ivec2 m_WindowSize;
		bool m_UpdateViewportCoordinates;

		// splash screen
		bool m_ShowLogoScreen;
		int m_LogoWidth, m_LogoHeight;
		GLuint m_LogoTexHandle;
		GLuint m_LogoVAO = 0, m_LogoVBO = 0, m_LogoProgram = 0;
		int m_LogoUniformLocationAlpha = -1, m_LogoUniformLocationSampler = -1;
		std::chrono::steady_clock::time_point m_SplashStartTime;
	};
}