#pragma once

#include "lrpch.h"
#include "Renderer/IShaderStorageBuffer.h"

namespace Laura
{

	class OpenGLShaderStorageBuffer : public IShaderStorageBuffer {
	public:
		OpenGLShaderStorageBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type);

		virtual void Bind() override;
		virtual void Unbind() override;

		virtual void AddData(uint32_t offset, uint32_t dataSize, const void* data) override;

		virtual void SetBindingPoint(uint32_t bindingPoint) override;

		virtual void* ReadData(uint32_t offset, uint32_t dataSize) override;

	private:
		uint32_t m_ID, m_Size, m_BindingPoint;
		BufferUsageType m_UsageType;
	};
}