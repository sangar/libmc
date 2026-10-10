#pragma once

#include "mc/core/arena.h"
#include "mc/core/error.h"

// ThreadPool runs tasks on a fixed set of worker threads. Whoever owns the
// work creates the pool and passes it down.
typedef struct ThreadPool ThreadPool;

// A Task lives in the caller's memory, usually inside the work item it runs on.
typedef struct Task {
    void (*run)(void *argument);
    void *argument;
} Task;

// threadpool_create starts thread_count workers, or one per CPU when 0.
[[nodiscard]] Error threadpool_create(size_t thread_count, ThreadPool **pool, Err *err);
// threadpool_destroy runs the tasks still queued, then stops the workers.
void threadpool_destroy(ThreadPool *pool);
size_t threadpool_size(const ThreadPool *pool);
// threadpool_submit queues a task; the caller keeps it alive until it has run.
void threadpool_submit(ThreadPool *pool, Task *task);

// A ParallelPlan splits count items into chunks of at least min_chunk, so
// callers can size per-chunk outputs before threadpool_run_chunks runs them.
typedef struct ParallelPlan {
    size_t count;
    size_t chunk_size;
    size_t chunks;
} ParallelPlan;

typedef void (*ChunkFn)(void *context, size_t low, size_t high, size_t chunk);

ParallelPlan parallel_plan(const ThreadPool *pool, size_t count, size_t min_chunk);
// threadpool_run_chunks calls run(context, low, high, chunk) for every chunk
// of the plan on the pool and returns when all of them have finished. Its
// bookkeeping lives in scratch until then. A task running on the same pool
// must not call it: the waiting task holds a worker the chunks may need, so
// the pool can deadlock.
void threadpool_run_chunks(ThreadPool *pool, Arena *scratch, ParallelPlan plan, ChunkFn run, void *context);

typedef void (*IndexedFn)(void *context, size_t index);

// threads_run_each runs run(context, i) for i below count, each on a thread
// of its own, and waits for all of them. It suits a few blocking jobs, such
// as remote commands, where a pool's fixed size would serialise them. When a
// thread cannot be started, the jobs already running finish and the error is
// returned.
[[nodiscard]] Error threads_run_each(Arena *scratch, size_t count, IndexedFn run, void *context, Err *err);
