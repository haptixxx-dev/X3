#pragma once
#include <filesystem>

namespace X3
{
	namespace RuntimeCfg
	{
		// Defined in the .cpp
		extern std::filesystem::path EXECUTABLE_DIR;

		inline void Init(const std::filesystem::path& exeDir) {
			EXECUTABLE_DIR = exeDir;
		}
	}
}