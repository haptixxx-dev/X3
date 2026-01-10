#pragma once

#include <chrono>

namespace X3
{
    class Time {
    public:
        // Call at the start of each frame to update timing
        static void Update() {
            auto now = std::chrono::high_resolution_clock::now();
            s_DeltaTime = std::chrono::duration<float>(now - s_LastFrameTime).count();
            s_LastFrameTime = now;
            s_TotalTime += s_DeltaTime;
            s_FrameCount++;
        }

        // Time since last frame in seconds
        static float GetDeltaTime() { return s_DeltaTime; }

        // Total time since application start in seconds
        static float GetTotalTime() { return s_TotalTime; }

        // Fixed timestep for physics (default 1/60s = ~16.67ms)
        static float GetFixedDeltaTime() { return s_FixedDeltaTime; }
        static void SetFixedDeltaTime(float dt) { s_FixedDeltaTime = dt; }

        // Frame count since application start
        static uint64_t GetFrameCount() { return s_FrameCount; }

    private:
        inline static std::chrono::high_resolution_clock::time_point s_LastFrameTime =
            std::chrono::high_resolution_clock::now();
        inline static float s_DeltaTime = 0.0f;
        inline static float s_TotalTime = 0.0f;
        inline static float s_FixedDeltaTime = 1.0f / 60.0f; // 60 Hz physics default
        inline static uint64_t s_FrameCount = 0;
    };
}
