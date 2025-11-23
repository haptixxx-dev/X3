#include "Platform/OpenGL/OpenGLTexture2D.h"
#include <GL/glew.h>
#include "Platform/OpenGL/OpenGLdebugFuncs.h"

namespace Laura 
{

	OpenGLTexture2D::OpenGLTexture2D(const unsigned char* data, const int width, const int height, int textureUnit)
		: m_TextureUnit(textureUnit), m_ID(0) {

		if (width <= 0 || height <= 0) {
			LOG_ENGINE_CRITICAL("Error: Invalid texture dimensions {0}x{1}", width, height);
			return;
		}

		if (textureUnit < 0 || textureUnit >= GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) {
			LOG_ENGINE_CRITICAL("Error: Invalid texture unit slot {0}", textureUnit);
			return;
		}

		GLCall(glActiveTexture(GL_TEXTURE0 + m_TextureUnit));
		GLCall(glGenTextures(1, &m_ID));
		GLCall(glBindTexture(GL_TEXTURE_2D, m_ID));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

		if (!data) {
			LOG_ENGINE_WARN("Generating empty texture");
		}

		GLCall(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data));
	}

	OpenGLTexture2D::~OpenGLTexture2D() {
		if (m_ID != 0) {
			GLCall(glDeleteTextures(1, &m_ID));
			m_ID = 0;
		}
	}

	void OpenGLTexture2D::ChangeTextureUnit(int textureUnit) {
		if (textureUnit < 0 || textureUnit >= GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS) {
			LOG_ENGINE_CRITICAL("Error: Invalid texture unit slot {0}", textureUnit);
			return;
		}

		if (textureUnit != m_TextureUnit) {
			m_TextureUnit = textureUnit;
			GLCall(glActiveTexture(GL_TEXTURE0 + m_TextureUnit));
			GLCall(glBindTexture(GL_TEXTURE_2D, m_ID));
		}
	}
}