#pragma once

#include "lrpch.h"

namespace Laura 
{

	enum struct Image2DType {
		LR_READ,
		LR_WRITE,
		LR_READ_WRITE
	};

	class IImage2D {
	public:
		static std::shared_ptr<IImage2D> Create(unsigned char* data, int width, int height, int imageUnit, Image2DType imageType);
		virtual ~IImage2D() {}
		virtual void ChangeImageUnit(int imageUnit) = 0;
		virtual int GetID() const = 0;
		virtual glm::ivec2 GetDimensions() const = 0;
	};
}