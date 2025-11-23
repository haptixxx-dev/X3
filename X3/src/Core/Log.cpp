#include "Core/Log.h"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace Laura 
{

	std::shared_ptr<spdlog::logger> Log::s_EngineLogger;
	std::shared_ptr<spdlog::logger> Log::s_EditorLogger;

	void Log::Init() {
		spdlog::set_pattern("%^[%T] %n: %v%$");
		Log::s_EngineLogger = spdlog::stdout_color_mt("Core");
		Log::s_EngineLogger->set_level(spdlog::level::trace);

		Log::s_EditorLogger = spdlog::stdout_color_mt("App");
		Log::s_EditorLogger->set_level(spdlog::level::trace);
	}

}