#include "mc/concurrency/threadpool.h"

#include "mc/concurrency/queue.h"
#include "mc/platform/platform.h"

#include <assert.h>

struct ThreadPool {
    Arena *arena; // holds the pool and its thread handles
    Queue *tasks;
    Thread *threads;
    size_t size;
};

static void *work(void *argument)
{
    ThreadPool *pool = argument;
    Task *task;
    while ((task = queue_pop(pool->tasks)) != nullptr) {
        task->run(task->argument);
    }
    return nullptr;
}

// stop lets the first started workers finish the queue, then frees the pool.
static void stop(ThreadPool *pool, size_t started)
{
    queue_close(pool->tasks);
    for (size_t i = 0; i < started; i++) {
        thread_join(&pool->threads[i]);
    }
    queue_destroy(pool->tasks);
    arena_destroy(pool->arena);
}

Error threadpool_create(size_t thread_count, ThreadPool **pool, Err *err)
{
    *pool = nullptr;
    Arena *arena = arena_create(0);
    ThreadPool *created = arena_push(arena, sizeof(ThreadPool));
    created->arena = arena;
    created->tasks = queue_create();
    created->size = thread_count > 0 ? thread_count : thread_cpu_count();
    created->threads = arena_push(arena, arena_size_mul(created->size, sizeof *created->threads));
    for (size_t i = 0; i < created->size; i++) {
        Error e = thread_start(&created->threads[i], work, created, err);
        if (e != ERR_OK) {
            size_t wanted = created->size;
            stop(created, i);
            return err_wrap(err, e, "thread pool of %zu", wanted);
        }
    }
    *pool = created;
    return ERR_OK;
}

void threadpool_destroy(ThreadPool *pool)
{
    if (pool != nullptr) {
        stop(pool, pool->size);
    }
}

size_t threadpool_size(const ThreadPool *pool)
{
    return pool->size;
}

void threadpool_submit(ThreadPool *pool, Task *task)
{
    [[maybe_unused]] bool queued = queue_push(pool->tasks, task);
    assert(queued && "the queue closes only when the pool is destroyed");
}

ParallelPlan parallel_plan(const ThreadPool *pool, size_t count, size_t min_chunk)
{
    size_t chunk_size = max_size((count + pool->size - 1) / pool->size, max_size(min_chunk, 1));
    return (ParallelPlan){ .count = count, .chunk_size = chunk_size, .chunks = (count + chunk_size - 1) / chunk_size };
}

typedef struct {
    Mutex mutex;
    Cond done;
    size_t remaining;
    ChunkFn run;
    void *context;
} Batch;

typedef struct {
    Task task;
    Batch *batch;
    size_t low;
    size_t high;
    size_t chunk;
} ChunkTask;

static void run_chunk(void *argument)
{
    ChunkTask *chunk = argument;
    Batch *batch = chunk->batch;
    batch->run(batch->context, chunk->low, chunk->high, chunk->chunk);
    mutex_lock(&batch->mutex);
    if (--batch->remaining == 0) {
        cond_signal(&batch->done);
    }
    mutex_unlock(&batch->mutex);
}

void threadpool_run_chunks(ThreadPool *pool, Arena *scratch, ParallelPlan plan, ChunkFn run, void *context)
{
    if (plan.chunks == 0) {
        return;
    }
    if (plan.chunks == 1) {
        run(context, 0, plan.count, 0);
        return;
    }
    Batch batch = { .remaining = plan.chunks, .run = run, .context = context };
    mutex_init(&batch.mutex);
    cond_init(&batch.done);
    ArenaMark mark = arena_mark(scratch);
    ChunkTask *chunks = arena_push(scratch, arena_size_mul(plan.chunks, sizeof *chunks));
    for (size_t c = 0; c < plan.chunks; c++) {
        size_t low = c * plan.chunk_size;
        chunks[c] = (ChunkTask){
            .task = { .run = run_chunk, .argument = &chunks[c] },
            .batch = &batch,
            .low = low,
            .high = min_size(low + plan.chunk_size, plan.count),
            .chunk = c,
        };
        threadpool_submit(pool, &chunks[c].task);
    }
    mutex_lock(&batch.mutex);
    while (batch.remaining > 0) {
        cond_wait(&batch.done, &batch.mutex);
    }
    mutex_unlock(&batch.mutex);
    arena_release(mark);
    cond_destroy(&batch.done);
    mutex_destroy(&batch.mutex);
}

typedef struct {
    IndexedFn run;
    void *context;
    size_t index;
} IndexedJob;

static void *run_indexed(void *argument)
{
    IndexedJob *job = argument;
    job->run(job->context, job->index);
    return nullptr;
}

Error threads_run_each(Arena *scratch, size_t count, IndexedFn run, void *context, Err *err)
{
    ArenaMark mark = arena_mark(scratch);
    Thread *threads = arena_push(scratch, arena_size_mul(count, sizeof *threads));
    IndexedJob *jobs = arena_push(scratch, arena_size_mul(count, sizeof *jobs));
    size_t started = 0;
    Error e = ERR_OK;
    for (; started < count; started++) {
        jobs[started] = (IndexedJob){ .run = run, .context = context, .index = started };
        e = thread_start(&threads[started], run_indexed, &jobs[started], err);
        if (e != ERR_OK) {
            break;
        }
    }
    for (size_t i = 0; i < started; i++) {
        thread_join(&threads[i]);
    }
    arena_release(mark);
    return e;
}
