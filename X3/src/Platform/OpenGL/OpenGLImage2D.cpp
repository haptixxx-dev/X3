#include "Platform/OpenGL/OpenGLImage2D.h"
#include "Platform/OpenGL/OpenGLdebugFuncs.h"

namespace Laura 
{

	OpenGLImage2D::OpenGLImage2D(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType)
		: m_ImageUnit(imageUnit), m_ID(0), m_Dimensions(width, height) {

		if (width <= 0 || height <= 0) {
			LOG_ENGINE_CRITICAL("Error: Invalid image dimensions {0}x{1}", width, height);
			return;
		}

		if (imageUnit < 0 || imageUnit >= GL_MAX_IMAGE_UNITS) {
			LOG_ENGINE_CRITICAL("Error: Invalid image unit slot {0}", imageUnit);
			return;
		}

		// DSA direct state access texture creation (Create a texture without having to bind it)
		GLCall(glCreateTextures(GL_TEXTURE_2D, 1, &m_ID));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
		GLCall(glTextureParameteri(m_ID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
		GLCall(glTextureStorage2D(m_ID, 1, GL_RGBA32F, width, height));
		const float zero[4] = {0.f, 0.f, 0.f, 0.f};
		GLCall(glClearTexImage(m_ID, 0, GL_RGBA, GL_FLOAT, zero));

		if (data) {	// Passing data to the texture if not nullptr
			GLCall(glTextureSubImage2D(m_ID, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data));
		}
		else {
			LOG_ENGINE_WARN("Generating empty image");
		}

		// Converting the Image2DType to OpenGL's equivalent
		GLenum m_Image2DType;
		switch (imageType) {
			case Image2DType::LR_READ:
				m_Image2DType = GL_READ_ONLY; 
				break;
			case Image2DType::LR_WRITE:
				m_Image2DType = GL_WRITE_ONLY; 
				break;
			case Image2DType::LR_READ_WRITE:
				m_Image2DType = GL_READ_WRITE; 
				break;
			default:
				LOG_ENGINE_CRITICAL("Invalid Image2DType"); 
				break;
		}

		// Binding the texture to the image unit
		GLCall(glBindImageTexture(m_ImageUnit, m_ID, 0, GL_FALSE, 0, m_Image2DType, GL_RGBA32F));
	}

	OpenGLImage2D::~OpenGLImage2D() {
		if (m_ID != 0) {
			GLCall(glDeleteTextures(1, &m_ID));
			m_ID = 0;
		}
	}

	void OpenGLImage2D::ChangeImageUnit(int imageUnit) {
		if (imageUnit < 0 || imageUnit >= GL_MAX_IMAGE_UNITS) {
			LOG_ENGINE_CRITICAL("Error: Invalid image unit slot {0}", imageUnit);
			return;
		}

		if (imageUnit != m_ImageUnit) {
			m_ImageUnit = imageUnit;
			GLCall(glBindImageTexture(m_ImageUnit, m_ID, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F));
		}
	}
}