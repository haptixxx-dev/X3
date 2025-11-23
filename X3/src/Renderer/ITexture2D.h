#pragma once

#include "lrpch.h"

namespace Laura 
{

	class ITexture2D {
	public:
		static std::shared_ptr<ITexture2D> Create(const unsigned char* data, const int width, const int height, int textureUnit);
		virtual ~ITexture2D() {}
		virtual void ChangeTextureUnit(int textureUnit) = 0;
		virtual int GetID() const = 0;
	};

}