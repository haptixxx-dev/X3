#pragma once

#include <memory>
#include "spdlog/spdlog.h"

namespace X3 
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

#define LOG_ENGINE_TRACE(...)    ::X3::Log::GetEngineLogger()->trace(__VA_ARGS__)
#define LOG_ENGINE_INFO(...)     ::X3::Log::GetEngineLogger()->info(__VA_ARGS__)
#define LOG_ENGINE_WARN(...)     ::X3::Log::GetEngineLogger()->warn(__VA_ARGS__)
#define LOG_ENGINE_ERROR(...)    ::X3::Log::GetEngineLogger()->error(__VA_ARGS__)
#define LOG_ENGINE_CRITICAL(...) ::X3::Log::GetEngineLogger()->critical(__VA_ARGS__)

#define LOG_EDITOR_TRACE(...)    ::X3::Log::GetEditorLogger()->trace(__VA_ARGS__)
#define LOG_EDITOR_INFO(...)     ::X3::Log::GetEditorLogger()->info(__VA_ARGS__)
#define LOG_EDITOR_WARN(...)     ::X3::Log::GetEditorLogger()->warn(__VA_ARGS__)
#define LOG_EDITOR_ERROR(...)    ::X3::Log::GetEditorLogger()->error(__VA_ARGS__)
#define LOG_EDITOR_CRITICAL(...) ::X3::Log::GetEditorLogger()->critical(__VA_ARGS__)
