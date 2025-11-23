#include "OpenGLShaderStorageBuffer.h"
#include <GL/glew.h>
#include "Platform/OpenGL/OpenGLdebugFuncs.h"

namespace Laura
{

	OpenGLShaderStorageBuffer::OpenGLShaderStorageBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type)
		: m_ID(0), m_Size(size), m_BindingPoint(bindingPoint), m_UsageType(type) {
		GLCall(glGenBuffers(1, &m_ID));
		GLCall(glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ID));
		GLCall(glBufferData(GL_SHADER_STORAGE_BUFFER, m_Size, nullptr, (m_UsageType == BufferUsageType::STATIC_DRAW) ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW));
		GLCall(glBindBufferBase(GL_SHADER_STORAGE_BUFFER, m_BindingPoint, m_ID)); // binding the UBO to the binding point
	}

	void OpenGLShaderStorageBuffer::Bind() {
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ID);
	}

	void OpenGLShaderStorageBuffer::Unbind() {
		GLCall(glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0));
		GLCall(glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT));
	}

	void OpenGLShaderStorageBuffer::AddData(uint32_t offset, uint32_t dataSize, const void* data) {
		GLCall(glBufferSubData(GL_SHADER_STORAGE_BUFFER, offset, dataSize, data));
	}

	void OpenGLShaderStorageBuffer::SetBindingPoint(uint32_t bindingPoint) {
		m_BindingPoint = bindingPoint;
		GLCall(glBindBufferBase(GL_SHADER_STORAGE_BUFFER, m_BindingPoint, m_ID));
	}

	void* OpenGLShaderStorageBuffer::ReadData(uint32_t offset, uint32_t dataSize) {
		Bind();
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); // make sure all data has been written before proceeding

		void* dataPtr = (void*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, offset, dataSize, GL_MAP_READ_BIT);

		if (dataPtr == nullptr) {
			LOG_ENGINE_CRITICAL("[ERROR] reading SSBO Buffer");
			ASSERT(false);
			return nullptr;
		}
		else {
			GLCall(glUnmapBuffer(GL_SHADER_STORAGE_BUFFER));
		}

		GLCall(glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0));
		return dataPtr;		
	}
}