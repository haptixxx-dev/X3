#pragma once

#include "lrpch.h"

namespace Laura 
{
	// Enum class to give the user the option to choose between STATIC_DRAW or DYNAMIC_DRAW
	// STATIC_DRAW: The data store contents will be modified once and used many times.
	// DYNAMIC_DRAW: The data store contents will be modified repeatedly and used many times.
	#ifndef BUFFER_USAGE_TYPE_STRUCT
	#define BUFFER_USAGE_TYPE_STRUCT
		enum class BufferUsageType {
			STATIC_DRAW = 0,
			DYNAMIC_DRAW = 1
		};
	#endif

	class IUniformBuffer {
	public:
		static std::shared_ptr<IUniformBuffer> Create(uint32_t size, uint32_t bindingPoint, BufferUsageType type);
		virtual ~IUniformBuffer() = default;
		virtual void Bind() = 0;
		virtual void Unbind() = 0;
		virtual void SetBindingPoint(uint32_t bindingPoint) = 0;
		virtual void AddData(uint32_t offset, uint32_t dataSize, const void* data) = 0;
	};
}