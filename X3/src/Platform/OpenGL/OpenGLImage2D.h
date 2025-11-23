#pragma once

#include "lrpch.h"
#include <GL/glew.h>
#include "Renderer/IImage2D.h"
#include "glm/glm.hpp"

namespace Laura 
{

	class OpenGLImage2D : public IImage2D {
	public:
		OpenGLImage2D(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType);
		virtual ~OpenGLImage2D() override;
		virtual void ChangeImageUnit(int imageUnit) override;
		inline virtual int GetID() const override { return m_ID; }
		inline virtual glm::ivec2 GetDimensions() const override { return m_Dimensions; }

	private:
		unsigned int m_ID;
		int m_ImageUnit;
		GLenum m_Image2DType;
		glm::ivec2 m_Dimensions;
	};
}