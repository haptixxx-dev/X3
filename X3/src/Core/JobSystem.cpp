#include "Core/JobSystem.h"

#include <TaskScheduler.h>

#include <memory>

namespace X3
{

	namespace {
		std::unique_ptr<enki::TaskScheduler> g_Scheduler;

		// enkiTS TaskSet whose body is a std::function. Kept private: exposing it
		// would let a caller allocate one and let it die before it completed,
		// which is the classic way to corrupt memory with this library. Every
		// instance below is a stack object that its own blocking wait outlives.
		struct RangeTask : enki::ITaskSet
		{
			const std::function<void(uint32_t, uint32_t)>* body = nullptr;

			RangeTask(uint32_t setSize, uint32_t minRange,
			          const std::function<void(uint32_t, uint32_t)>& fn)
				: enki::ITaskSet(setSize, minRange), body(&fn) {}

			void ExecuteRange(enki::TaskSetPartition range, uint32_t /*threadnum*/) override
			{
				(*body)(range.start, range.end);
			}
		};
	}

	void JobSystem::Init(uint32_t threadCount) {
		if (g_Scheduler) {
			LOG_ENGINE_WARN("JobSystem::Init called twice; ignoring");
			return;
		}

		g_Scheduler = std::make_unique<enki::TaskScheduler>();

		enki::TaskSchedulerConfig config;
		if (threadCount > 0)
			config.numTaskThreadsToCreate = threadCount - 1;   // the caller is one of them
		g_Scheduler->Initialize(config);

		LOG_ENGINE_INFO("JobSystem: started with {} threads", g_Scheduler->GetNumTaskThreads());
	}

	void JobSystem::Shutdown() {
		if (!g_Scheduler) return;
		// WaitforAllAndShutdown blocks until in-flight tasks finish. Anything that
		// outlives this is a task holding a dangling reference, so the block is the
		// point rather than a courtesy.
		g_Scheduler->WaitforAllAndShutdown();
		g_Scheduler.reset();
		LOG_ENGINE_INFO("JobSystem: shut down");
	}

	bool JobSystem::Available() {
		return g_Scheduler != nullptr;
	}

	uint32_t JobSystem::ThreadCount() {
		return g_Scheduler ? g_Scheduler->GetNumTaskThreads() : 1u;
	}

	void JobSystem::ParallelFor(uint32_t count, uint32_t minRange,
	                            const std::function<void(uint32_t, uint32_t)>& body) {
		if (count == 0) return;

		// INLINE FALLBACK. A tool, a test, or any code path that runs before
		// Application starts the scheduler still gets correct results, serially.
		// Without this every caller would need its own guard, and one of them
		// would eventually forget.
		if (!g_Scheduler) {
			body(0, count);
			return;
		}

		RangeTask task(count, minRange == 0 ? 1u : minRange, body);
		g_Scheduler->AddTaskSetToPipe(&task);
		// This does NOT idle: enkiTS executes pending work on the waiting thread,
		// which is what makes nesting ParallelFor safe rather than deadlock-prone.
		g_Scheduler->WaitforTask(&task);
	}

	void JobSystem::ParallelForEach(uint32_t count, const std::function<void(uint32_t)>& body) {
		ParallelFor(count, 1, [&body](uint32_t begin, uint32_t end) {
			for (uint32_t i = begin; i < end; ++i)
				body(i);
		});
	}

}
