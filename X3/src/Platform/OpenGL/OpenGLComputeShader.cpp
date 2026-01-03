#include <GL/glew.h>
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/OpenGL/OpenGLdebugFuncs.h"

namespace X3 
{

	OpenGLComputeShader::OpenGLComputeShader(const std::string& filepath, const glm::uvec3& workGroupSizes)
		: m_Filepath(filepath), m_WorkGroupSizes(workGroupSizes), m_ID(0) {
		CreateShader();
	}

	OpenGLComputeShader::~OpenGLComputeShader() {
		GLCall(glDeleteProgram(m_ID));
	}

	void OpenGLComputeShader::Bind() {
		if (m_ID == 0) {
			LOG_ENGINE_ERROR("[ERROR] Attempting to bind invalid compute shader (ID=0)");
			return;
		}
		GLCall(glUseProgram(m_ID));
	}

	void OpenGLComputeShader::Unbind() {
		GLCall(glUseProgram(0));
	}

	void OpenGLComputeShader::Dispatch() {
		#ifdef MEASURE_GPU_RENDER_TIME
		GLuint query;
		glGenQueries(1, &query);
		glBeginQuery(GL_TIME_ELAPSED, query);
		#endif

		glDispatchCompute(m_WorkGroupSizes.x, m_WorkGroupSizes.y, m_WorkGroupSizes.z);
		GLCall(glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT));

		#ifdef MEASURE_GPU_RENDER_TIME
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
		#endif
	}

	std::string OpenGLComputeShader::ProcessIncludes(const std::string& source, const std::string& baseDir, std::set<std::string>& includedFiles) {
		std::stringstream result;
		std::istringstream stream(source);
		std::string line;

		while (std::getline(stream, line)) {
			// Check for #include directive
			size_t includePos = line.find("#include");
			if (includePos != std::string::npos) {
				// Find the quoted path
				size_t firstQuote = line.find('"', includePos);
				size_t lastQuote = line.rfind('"');

				if (firstQuote != std::string::npos && lastQuote != std::string::npos && firstQuote < lastQuote) {
					std::string includePath = line.substr(firstQuote + 1, lastQuote - firstQuote - 1);

					// Build full path relative to base directory
					std::filesystem::path fullPath = std::filesystem::path(baseDir) / includePath;
					std::string fullPathStr = fullPath.string();

					// Guard against circular includes
					if (includedFiles.find(fullPathStr) != includedFiles.end()) {
						result << "// Skipped already included: " << includePath << '\n';
						continue;
					}
					includedFiles.insert(fullPathStr);

					// Read the included file
					std::ifstream includeStream(fullPathStr);
					if (!includeStream.is_open()) {
						LOG_ENGINE_ERROR("[ERROR] Failed to open shader include file: {}", fullPathStr);
						result << "// ERROR: Failed to include: " << includePath << '\n';
						continue;
					}

					std::stringstream includeContent;
					includeContent << includeStream.rdbuf();
					includeStream.close();

					// Get directory of included file for nested includes
					std::string includeDir = fullPath.parent_path().string();

					// Recursively process includes in the included file
					result << "// BEGIN include: " << includePath << '\n';
					result << ProcessIncludes(includeContent.str(), includeDir, includedFiles);
					result << "// END include: " << includePath << '\n';
				} else {
					// Malformed include directive
					LOG_ENGINE_WARN("[WARN] Malformed #include directive: {}", line);
					result << line << '\n';
				}
			} else {
				result << line << '\n';
			}
		}

		return result.str();
	}

	std::string OpenGLComputeShader::ParseShaderFile() {
		std::ifstream stream(m_Filepath);
		if (!stream.is_open()) {
			LOG_ENGINE_CRITICAL("[ERROR] Failed to open compute shader file: {}", m_Filepath);
			return "";
		}
		std::stringstream ss;
		ss << stream.rdbuf();
		stream.close();

		std::string source = ss.str();

		// Get directory of the shader file for relative includes
		std::filesystem::path shaderPath(m_Filepath);
		std::string baseDir = shaderPath.parent_path().string();

		LOG_ENGINE_INFO("[Shader] Loading: {} (baseDir: {})", m_Filepath, baseDir);

		// Process #include directives
		std::set<std::string> includedFiles;
		includedFiles.insert(m_Filepath); // Mark main file as included to prevent self-include
		source = ProcessIncludes(source, baseDir, includedFiles);

		LOG_ENGINE_INFO("[Shader] Processed {} includes, total length: {} chars", includedFiles.size() - 1, source.length());

		return source;
	}

	void OpenGLComputeShader::CreateShader() {
		std::string computeShaderSource = ParseShaderFile();
		if (computeShaderSource.empty()) {
			LOG_ENGINE_CRITICAL("[ERROR] Compute shader source is empty, cannot create shader");
			m_ID = 0;
			return;
		}
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
			LOG_ENGINE_CRITICAL("[ERROR] Compute Shader compilation error in {}:", m_Filepath);
			LOG_ENGINE_CRITICAL(message);
			// Also log first 500 chars of processed source to help debug
			LOG_ENGINE_ERROR("[DEBUG] First 500 chars of processed source:\n{}", computeShaderSource.substr(0, 500));
			GLCall(glDeleteShader(computeShaderID));
			m_ID = 0;
			return;
		}

		m_ID = glCreateProgram(); // is m_RendererID
		GLCall(glAttachShader(m_ID, computeShaderID));
		GLCall(glLinkProgram(m_ID));

		// Check link status
		GLCall(glGetProgramiv(m_ID, GL_LINK_STATUS, &result));
		if (result == GL_FALSE) {
			int length;
			GLCall(glGetProgramiv(m_ID, GL_INFO_LOG_LENGTH, &length));
			char* message = (char*)alloca(length * sizeof(char));
			GLCall(glGetProgramInfoLog(m_ID, length, &length, message));
			LOG_ENGINE_CRITICAL("[ERROR] Compute Shader program linking error:");
			LOG_ENGINE_CRITICAL(message);
			GLCall(glDeleteProgram(m_ID));
			GLCall(glDeleteShader(computeShaderID));
			m_ID = 0;
			return;
		}

		GLCall(glValidateProgram(m_ID));
		GLCall(glDeleteShader(computeShaderID));
	}

}