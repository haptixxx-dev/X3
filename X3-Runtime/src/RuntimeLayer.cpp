#include "RuntimeLayer.h"
#include "RuntimeCfg.h"
#include <filesystem>
#include <algorithm>
#include "Core/Events/WindowEvents.h"
#include <stb_image/stb_image.h>


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
		, m_Framebuffer(0)
		, m_ViewportCoords(0, 0, 0, 0)
		, m_WindowSize(0, 0)

		, m_ShowLogoScreen(true)
		, m_LogoWidth(0)
		, m_LogoHeight(0)
		, m_LogoTexHandle(0)
		, m_UpdateViewportCoordinates(false)
	{}

	bool RuntimeLayer::LoadLogoFromDisk(GLuint* out_texture, int* out_width, int* out_height) {
		int width = 0, height = 0, channelsInFile = 0;
		stbi_set_flip_vertically_on_load(1);

		std::filesystem::path path = EngineCfg::RESOURCES_PATH / "made_with_X3.png";

		unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channelsInFile, STBI_rgb_alpha);
		if (!data) {
			LOG_ENGINE_CRITICAL("LoadLogoFromDisk: failed to load texture {0}", path.string());
			return false;
		}

		GLuint tex = 0;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);

		// Optional but robust
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// Prefer sized internal format
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

		stbi_image_free(data);

		*out_texture = tex;
		*out_width = width;
		*out_height = height;
		return true;
	}

	void RuntimeLayer::onAttach() { m_ExportSettings = DeserializeExportSettingsYaml(RuntimeCfg::EXECUTABLE_DIR).value_or(ExportSettings{});
		m_Window->setVSync(m_ExportSettings.vSync);
		m_Window->setFullscreen(m_ExportSettings.fullscreen);

		m_WindowSize = m_Window->getFrameBufferSize();
		m_UpdateViewportCoordinates = true;

		if (m_ShowLogoScreen) {
			if (m_LogoTexHandle == 0) {
				if (!LoadLogoFromDisk(&m_LogoTexHandle, &m_LogoWidth, &m_LogoHeight)) {
					m_ShowLogoScreen = false;
				}
			}
			if (m_ShowLogoScreen) {
				InitLogoResources();
			}
		}

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

		glGenFramebuffers(1, &m_Framebuffer);
	}

	void RuntimeLayer::onDetach() {
		if (m_Framebuffer) {
			glDeleteFramebuffers(1, &m_Framebuffer);
		}
		DestroyLogoResources();
	}

	void RuntimeLayer::onUpdate() {
		if (m_ShowLogoScreen) {
			static bool firstCall = true;
			if (firstCall) {
				m_SplashStartTime = std::chrono::steady_clock::now();
				firstCall = false;
			}

			float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_SplashStartTime).count();
			float fadeInTime   = 1.5f;  // seconds
			float holdTime     = 0.5f;  // seconds
			float fadeOutTime  = 0.5f;  // seconds
			float totalTime    = fadeInTime + holdTime + fadeOutTime;

			float alpha = 1.0f;
			if (elapsed < fadeInTime) {
				alpha = elapsed / fadeInTime;
			} 
			else if (elapsed < fadeInTime + holdTime) {
				alpha = 1.0f;
			}
			else if (elapsed < totalTime) {
				float t = (elapsed - (fadeInTime + holdTime)) / fadeOutTime;
				alpha = 1.0f - t;
			}
			else {
				m_ShowLogoScreen = false;
				DestroyLogoResources();
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, m_WindowSize.x, m_WindowSize.y);
			glDisable(GL_DEPTH_TEST);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glClearColor(0.12549019607843137f, 0.12156862745098039f, 0.13333333333333333f, 1.0f); // dark gray
			glClear(GL_COLOR_BUFFER_BIT);
			RenderLogo(alpha);
			return;
		}
		 
		if (m_CurrentFrame) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, m_Framebuffer);
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_CurrentFrame->GetID(), 0);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // Bind the default framebuffer (screen)
			CalculateViewportCoordinates();
			// Display the Texture
			glBlitFramebuffer(0, 0, m_CurrentFrame->GetDimensions().x, m_CurrentFrame->GetDimensions().y,
							 m_ViewportCoords.x, m_ViewportCoords.y, m_ViewportCoords.z, m_ViewportCoords.w,
							 GL_COLOR_BUFFER_BIT, GL_NEAREST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); // Unbind
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

			if (m_ShowLogoScreen) {
			}
		}
	}

	bool RuntimeLayer::InitLogoResources() {
		if (m_LogoProgram != 0) {
			return true;
		}
		const char* vsSrc = "#version 330 core\n"
			"layout(location=0) in vec2 aPos;\n"
			"layout(location=1) in vec2 aUV;\n"
			"out vec2 vUV;\n"
			"void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }";
		const char* fsSrc = "#version 330 core\n"
			"in vec2 vUV;\n"
			"out vec4 FragColor;\n"
			"uniform sampler2D uTex;\n"
			"uniform float uAlpha;\n"
			"void main(){ vec4 c = texture(uTex, vUV); FragColor = vec4(c.rgb, c.a * uAlpha); }";

		auto compileShader = [](GLenum type, const char* src) -> GLuint {
			GLuint s = glCreateShader(type);
			glShaderSource(s, 1, &src, nullptr);
			glCompileShader(s);
			GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
			if (!ok) { GLint len=0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len); std::string log(len, '\0'); glGetShaderInfoLog(s, len, nullptr, log.data()); LOG_ENGINE_CRITICAL("Logo shader compile error: {0}", log); glDeleteShader(s); return 0; }
			return s;
		};
		GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
		GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
		if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return false; }
		m_LogoProgram = glCreateProgram();
		glAttachShader(m_LogoProgram, vs);
		glAttachShader(m_LogoProgram, fs);
		glLinkProgram(m_LogoProgram);
		glDeleteShader(vs);
		glDeleteShader(fs);
		GLint linked=0; glGetProgramiv(m_LogoProgram, GL_LINK_STATUS, &linked);
		if (!linked) { GLint len=0; glGetProgramiv(m_LogoProgram, GL_INFO_LOG_LENGTH, &len); std::string log(len, '\0'); glGetProgramInfoLog(m_LogoProgram, len, nullptr, log.data()); LOG_ENGINE_CRITICAL("Logo program link error: {0}", log); glDeleteProgram(m_LogoProgram); m_LogoProgram=0; return false; }

		glGenVertexArrays(1, &m_LogoVAO);
		glGenBuffers(1, &m_LogoVBO);
		glBindVertexArray(m_LogoVAO);
		glBindBuffer(GL_ARRAY_BUFFER, m_LogoVBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)(sizeof(float) * 2));
		glBindVertexArray(0);

		m_LogoUniformLocationAlpha = glGetUniformLocation(m_LogoProgram, "uAlpha");
		m_LogoUniformLocationSampler = glGetUniformLocation(m_LogoProgram, "uTex");
		return true;
	}

	void RuntimeLayer::DestroyLogoResources() {
		if (m_LogoVBO) { glDeleteBuffers(1, &m_LogoVBO); m_LogoVBO = 0; }
		if (m_LogoVAO) { glDeleteVertexArrays(1, &m_LogoVAO); m_LogoVAO = 0; }
		if (m_LogoProgram) { glDeleteProgram(m_LogoProgram); m_LogoProgram = 0; }
		if (m_LogoTexHandle) { glDeleteTextures(1, &m_LogoTexHandle); m_LogoTexHandle = 0; }
	}

	void RuntimeLayer::RenderLogo(float alpha) {
		if (!m_LogoProgram || !m_LogoVAO || m_LogoTexHandle == 0 || m_WindowSize.x <= 0 || m_WindowSize.y <= 0) {
			return;
		}
		int targetW = m_WindowSize.x;
		int targetH = static_cast<int>(std::ceil((float)m_WindowSize.x * ((float)m_LogoHeight / (float)m_LogoWidth)));
		if (targetH > m_WindowSize.y) {
			targetH = m_WindowSize.y;
			targetW = static_cast<int>(std::ceil((float)m_WindowSize.y * ((float)m_LogoWidth / (float)m_LogoHeight)));
		}
		float sx = (float)targetW / (float)m_WindowSize.x;
		float sy = (float)targetH / (float)m_WindowSize.y;
		float verts[] = {
			-sx, -sy, 0.0f, 0.0f,
			 sx, -sy, 1.0f, 0.0f,
			 sx,  sy, 1.0f, 1.0f,
			-sx, -sy, 0.0f, 0.0f,
			 sx,  sy, 1.0f, 1.0f,
			-sx,  sy, 0.0f, 1.0f,
		};
		glBindBuffer(GL_ARRAY_BUFFER, m_LogoVBO);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
		glUseProgram(m_LogoProgram);
		glUniform1f(m_LogoUniformLocationAlpha, alpha);
		glUniform1i(m_LogoUniformLocationSampler, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_LogoTexHandle);
		glBindVertexArray(m_LogoVAO);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glBindVertexArray(0);
	}

	void RuntimeLayer::CalculateViewportCoordinates() {
		if (!m_CurrentFrame || !m_UpdateViewportCoordinates) { 
			return; 
		}
		m_UpdateViewportCoordinates = false;

		glm::ivec2 imageSize = m_CurrentFrame->GetDimensions();
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