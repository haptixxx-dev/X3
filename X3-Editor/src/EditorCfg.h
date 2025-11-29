#pragma once
#include <filesystem>

namespace Laura
{
	namespace EditorCfg
	{
		// Defined in the .cpp
		extern std::filesystem::path EXECUTABLE_DIR;
		extern std::filesystem::path RESOURCES_PATH;

		inline void Init(const std::filesystem::path& exeDir)
		{
			EXECUTABLE_DIR = exeDir;

			#ifdef BUILD_INSTALL
			RESOURCES_PATH = exeDir / "editor_res";
			#else
			RESOURCES_PATH = CMAKE_EDITOR_RESOURCES_PATH;
			#endif
		}
	}
}