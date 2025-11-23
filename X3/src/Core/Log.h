#pragma once

#include <memory>
#include "spdlog/spdlog.h"

namespace Laura 
{

	class Log {
	public:
		static void Init();

		inline static std::shared_ptr<spdlog::logger>& GetEngineLogger() { return s_EngineLogger; }
		inline static std::shared_ptr<spdlog::logger>& GetEditorLogger() { return s_EditorLogger; }

	private:
		static std::shared_ptr<spdlog::logger> s_EngineLogger;
		static std::shared_ptr<spdlog::logger> s_EditorLogger;
	};
}

#define LOG_ENGINE_TRACE(...)    ::Laura::Log::GetEngineLogger()->trace(__VA_ARGS__)
#define LOG_ENGINE_INFO(...)     ::Laura::Log::GetEngineLogger()->info(__VA_ARGS__)
#define LOG_ENGINE_WARN(...)     ::Laura::Log::GetEngineLogger()->warn(__VA_ARGS__)
#define LOG_ENGINE_ERROR(...)    ::Laura::Log::GetEngineLogger()->error(__VA_ARGS__)
#define LOG_ENGINE_CRITICAL(...) ::Laura::Log::GetEngineLogger()->critical(__VA_ARGS__)

#define LOG_EDITOR_TRACE(...)    ::Laura::Log::GetEditorLogger()->trace(__VA_ARGS__)
#define LOG_EDITOR_INFO(...)     ::Laura::Log::GetEditorLogger()->info(__VA_ARGS__)
#define LOG_EDITOR_WARN(...)     ::Laura::Log::GetEditorLogger()->warn(__VA_ARGS__)
#define LOG_EDITOR_ERROR(...)    ::Laura::Log::GetEditorLogger()->error(__VA_ARGS__)
#define LOG_EDITOR_CRITICAL(...) ::Laura::Log::GetEditorLogger()->critical(__VA_ARGS__)
