#pragma once

#include "lrpch.h"

namespace Laura 
{

	class IComputeShader {
	public:
		static std::shared_ptr<IComputeShader> Create(const std::string& filepath, const glm::uvec3& workGroupSizes);

		virtual ~IComputeShader() {}
		virtual void Bind() = 0;
		virtual void Unbind() = 0;
		virtual void Dispatch() = 0;

		/// SETTERS ///
		virtual void setWorkGroupSizes(const glm::uvec3 workGroupSizes) = 0;

		/// GETTERS ///
		virtual uint32_t GetID() = 0;
		virtual glm::uvec3 getWorkGroupSizes() = 0;
		virtual std::string getFilePath() = 0;
	};
}