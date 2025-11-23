#include <GL/glew.h>
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/OpenGL/OpenGLdebugFuncs.h"

namespace Laura 
{

	OpenGLComputeShader::OpenGLComputeShader(const std::string& filepath, const glm::uvec3& workGroupSizes)
		: m_Filepath(filepath), m_WorkGroupSizes(workGroupSizes), m_ID(0) {
		CreateShader();
	}

	OpenGLComputeShader::~OpenGLComputeShader() {
		GLCall(glDeleteProgram(m_ID));
	}

	void OpenGLComputeShader::Bind() {
		GLCall(glUseProgram(m_ID));
	}

	void OpenGLComputeShader::Unbind() {
		GLCall(glUseProgram(0));
	}

	void OpenGLComputeShader::Dispatch() {
		GLuint query;
		glGenQueries(1, &query);
		glBeginQuery(GL_TIME_ELAPSED, query);
		
		glDispatchCompute(m_WorkGroupSizes.x, m_WorkGroupSizes.y, m_WorkGroupSizes.z);
		GLCall(glMemoryBarrier(GL_ALL_BARRIER_BITS));

		#ifdef MEASURE_GPU_RENDER_TIME
		{
			//LOG THE EXACT SHADER EXECUTION TIME
			glEndQuery(GL_TIME_ELAPSED);

			GLint available = 0;
			while (!available) {
				glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
			}

			GLuint64 elapsedTime;
			glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsedTime);

			double elapsedTimeMs = elapsedTime / 1.0e6;
			printf("Compute Shader Execution Time: %.3f ms\n", elapsedTimeMs);

			glDeleteQueries(1, &query);
		}
		#endif
	}

	std::string OpenGLComputeShader::ParseShaderFile() {
		std::ifstream stream(m_Filepath);
		std::stringstream ss;
		std::string line;
		while (getline(stream, line)) {
			ss << line << '\n';
		}
		std::string source = ss.str();
		return source;
	}

	void OpenGLComputeShader::CreateShader() {
		std::string computeShaderSource = ParseShaderFile();
		const char* src = &computeShaderSource[0];
		uint32_t computeShaderID = glCreateShader(GL_COMPUTE_SHADER);

		GLCall(glShaderSource(computeShaderID, 1, &src, NULL));
		GLCall(glCompileShader(computeShaderID));

		int result;
		GLCall(glGetShaderiv(computeShaderID, GL_COMPILE_STATUS, &result));
		if (result == GL_FALSE) {
			int length;
			GLCall(glGetShaderiv(computeShaderID, GL_INFO_LOG_LENGTH, &length));
			char* message = (char*)alloca(length * sizeof(char));
			GLCall(glGetShaderInfoLog(computeShaderID, length, &length, message));
			LOG_ENGINE_CRITICAL("[ERROR] Compute Shader compilaion error:");
			LOG_ENGINE_CRITICAL(message);
			GLCall(glDeleteShader(computeShaderID));
			m_ID = 0;
			return;
		}

		m_ID = glCreateProgram(); // is m_RendererID
		GLCall(glAttachShader(m_ID, computeShaderID));
		GLCall(glLinkProgram(m_ID));
		GLCall(glValidateProgram(m_ID));
		GLCall(glDeleteShader(computeShaderID));
	}

}