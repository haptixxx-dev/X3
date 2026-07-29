#pragma once

// =============================================================================
// JobSystem.h -- the engine's task scheduler, a thin facade over enkiTS.
//
// WHY enkiTS AND NOT JOLT'S THREAD POOL. Jolt already ships a
// JobSystemThreadPool, and reusing it would have avoided a dependency. It is
// created and destroyed with the PHYSICS SIMULATION LIFECYCLE
// (PhysicsLayer deliberately avoids spinning it up when not simulating), and
// its barrier model is shaped around a physics step. Decoupling it from that
// lifecycle would have been more work than adopting a library, and the API
// inherited would have been an awkward fit for asset loading and, later,
// lightmap baking.
//
// WHY A FACADE AND NOT enkiTS DIRECTLY. Two reasons, both practical. Callers
// get `parallelFor` and `run` rather than TaskSet subclasses and manual
// lifetime management -- enkiTS requires a task object to outlive its
// completion, which is the single easiest way to corrupt memory with it.
// And the engine keeps ONE scheduler: a second one would oversubscribe the
// machine, and Phase 10's lightmap bake wants the whole machine.
//
// LIFETIME. Init() is called once from Application startup and Shutdown() once
// at teardown, both on the main thread. Every wait is a BLOCKING wait that also
// executes pending work on the calling thread, so a caller that waits is not
// idle -- that is enkiTS's design and it is why nesting parallelFor is safe
// rather than deadlocking.
// =============================================================================

#include "lrpch.h"

#include <cstdint>
#include <functional>

namespace enki { class TaskScheduler; }

namespace X3
{

	class JobSystem
	{
	public:
		/// Starts the scheduler. threadCount == 0 means "one per hardware thread",
		/// which is what enkiTS defaults to. Calling this twice is a no-op.
		static void Init(uint32_t threadCount = 0);

		/// Stops the scheduler and joins its threads. Safe to call without Init.
		static void Shutdown();

		/// True between Init and Shutdown. Every entry point below falls back to
		/// running INLINE, on the calling thread, when this is false -- so a tool
		/// or test that never called Init still produces correct results, just
		/// serially. That fallback is why callers do not need to guard.
		static bool Available();

		/// Number of threads that can execute work, including the calling one.
		/// 1 when the scheduler is not running.
		static uint32_t ThreadCount();

		/// Splits [0, count) across the scheduler and BLOCKS until every index has
		/// been visited. `body` is called with a half-open sub-range and must be
		/// safe to call from several threads at once.
		///
		/// Ranges, not individual indices: a per-index callback would pay an
		/// indirect call per element, which swamps the work for anything
		/// fine-grained. `minRange` is the smallest chunk worth handing to another
		/// thread; below it the whole thing runs on one.
		static void ParallelFor(uint32_t count, uint32_t minRange,
		                        const std::function<void(uint32_t begin, uint32_t end)>& body);

		/// ParallelFor with a per-index body, for coarse work where the indirect
		/// call does not matter (one asset import per index, say).
		static void ParallelForEach(uint32_t count,
		                            const std::function<void(uint32_t index)>& body);
	};

}
