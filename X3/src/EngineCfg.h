#pragma once

namespace Laura
{
	namespace EngineCfg
	{
		// Defined in the .cpp 
		// path to the exe currently running the engine
		extern std::filesystem::path EXECUTABLE_DIR;
		extern std::filesystem::path RESOURCES_PATH;

		inline void Init(const std::filesystem::path& exeDir)
		{
			EXECUTABLE_DIR = exeDir;

			#ifdef BUILD_INSTALL
			RESOURCES_PATH = exeDir / "engine_res";
			#else
			RESOURCES_PATH = CMAKE_ENGINE_RESOURCES_PATH;
			#endif
		}
	}
}