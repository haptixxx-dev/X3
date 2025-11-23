#include "OpenGLUniformBuffer.h"
#include <GL/glew.h>
#include "Platform/OpenGL/OpenGLdebugFuncs.h"

namespace Laura {

	OpenGLUniformBuffer::OpenGLUniformBuffer(uint32_t size, uint32_t bindingPoint, BufferUsageType type)
		: m_ID(0), m_Size(size), m_BindingPoint(bindingPoint), m_UsageType(type) {
		GLCall(glGenBuffers(1, &m_ID));
		GLCall(glBindBuffer(GL_UNIFORM_BUFFER, m_ID));
		GLCall(glBufferData(GL_UNIFORM_BUFFER, m_Size, nullptr, (m_UsageType == BufferUsageType::STATIC_DRAW) ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW));
		GLCall(glBindBufferBase(GL_UNIFORM_BUFFER, m_BindingPoint, m_ID)); // binding the UBO to the binding point
	}

	void OpenGLUniformBuffer::Bind() {
		glBindBuffer(GL_UNIFORM_BUFFER, m_ID);
	}

	void OpenGLUniformBuffer::Unbind() {
		GLCall(glBindBuffer(GL_UNIFORM_BUFFER, 0));
		GLCall(glMemoryBarrier(GL_UNIFORM_BARRIER_BIT));
	}

	void OpenGLUniformBuffer::AddData(uint32_t offset, uint32_t dataSize, const void* data) {
		GLCall(glBufferSubData(GL_UNIFORM_BUFFER, offset, dataSize, data));
	}

	void OpenGLUniformBuffer::SetBindingPoint(uint32_t bindingPoint) {
		m_BindingPoint = bindingPoint;
		GLCall(glBindBufferBase(GL_UNIFORM_BUFFER, m_BindingPoint, m_ID));
	}
}