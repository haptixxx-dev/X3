#pragma once

#include "lrpch.h"
#include "Renderer/IComputeShader.h"

namespace Laura 
{

	class OpenGLComputeShader : public IComputeShader {
	public:
		OpenGLComputeShader(const std::string& filepath, const glm::uvec3& workGroupSizes);
		~OpenGLComputeShader();

		virtual void Bind() override;
		virtual void Unbind() override;
		virtual void Dispatch() override;

		/// SETTERS ///
		inline virtual void setWorkGroupSizes(const glm::uvec3 workGroupSizes) override { m_WorkGroupSizes = workGroupSizes; };

		/// GETTERS ///
		inline virtual uint32_t GetID() override { return m_ID; };
		inline virtual glm::uvec3 getWorkGroupSizes() override { return m_WorkGroupSizes; };
		inline virtual std::string getFilePath() override { return m_Filepath; };

	private:
		uint32_t m_ID;
		glm::uvec3 m_WorkGroupSizes;
		const std::string m_Filepath;

	private:
		void CreateShader();
		std::string ParseShaderFile();
	};
}