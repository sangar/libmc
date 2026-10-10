#include "test.h"

#include <stdatomic.h>

#include "mc/concurrency/cancel.h"
#include "mc/concurrency/debounce.h"
#include "mc/concurrency/queue.h"
#include "mc/concurrency/threadpool.h"

typedef struct {
    Cancel *cancel;
    bool live;
} Sleeper;

static void *sleep_long(void *argument)
{
    Sleeper *sleeper = argument;
    sleeper->live = cancel_sleep(sleeper->cancel, 10000);
    return nullptr;
}

static void cancel_wakes_sleepers(Test *test)
{
    Cancel cancel;
    cancel_init(&cancel);
    test_check(test, cancel_sleep(&cancel, 1), "a short sleep ends live");
    cancel_lock(&cancel);
    test_check(test, cancel_wait(&cancel, clock_monotonic_ns() + NS_PER_MILLISECOND), "a deadline ends a wait live");
    cancel_unlock(&cancel);

    Sleeper sleeper = { .cancel = &cancel, .live = true };
    Thread thread;
    test_check(test, thread_start(&thread, sleep_long, &sleeper, nullptr) == ERR_OK, "start a sleeper");
    int64_t started = clock_monotonic_ns();
    clock_sleep_ms(5);
    cancel_request(&cancel);
    thread_join(&thread);
    test_check(test, !sleeper.live, "the sleeper sees the cancellation");
    test_check(test, clock_monotonic_ns() - started < 5LL * NS_PER_SECOND, "cancellation cuts the sleep short");
    test_check(test, cancel_requested(&cancel) && !cancel_sleep(&cancel, 1000), "later sleeps return at once");
    cancel_destroy(&cancel);
}

static void queue_keeps_order_across_growth(Test *test)
{
    Queue *queue = queue_create();
    int *values = arena_push(test->arena, 200 * sizeof *values);
    bool pushed = true;
    for (int i = 0; i < 200; i++) {
        values[i] = i;
        pushed = pushed && queue_push(queue, &values[i]);
    }
    bool ordered = pushed && queue_count(queue) == 200;
    for (int i = 0; i < 200; i++) {
        int *value = queue_pop(queue);
        ordered = ordered && value == &values[i];
    }
    test_check(test, ordered, "items come out in the order they went in");

    void *item;
    int64_t before = clock_monotonic_ns();
    test_check(test, !queue_pop_timeout(queue, 5, &item), "an empty queue times out");
    test_check(test, clock_monotonic_ns() - before >= 5LL * NS_PER_MILLISECOND, "the timeout is waited out");
    test_check(test, !queue_pop_timeout(queue, 0, &item), "a zero timeout does not wait");

    test_check(test, queue_push(queue, &values[7]), "an open queue takes a push");
    queue_close(queue);
    test_check(test, !queue_push(queue, &values[8]), "a closed queue turns pushes away");
    test_check(test, queue_pop(queue) == &values[7], "a closed queue still drains");
    test_check(test, queue_pop(queue) == nullptr, "then reports closed");
    queue_destroy(queue);
}

typedef struct {
    Queue *queue;
    int *values;
    atomic_int accepted;
} Producer;

static void *produce(void *argument)
{
    Producer *producer = argument;
    for (int i = 0; i < 1000; i++) {
        if (!queue_push(producer->queue, &producer->values[i])) {
            break;
        }
        atomic_fetch_add(&producer->accepted, 1);
    }
    return nullptr;
}

static void queue_serves_many_producers(Test *test)
{
    Queue *queue = queue_create();
    int *values = arena_push(test->arena, 1000 * sizeof *values);
    for (int i = 0; i < 1000; i++) {
        values[i] = i;
    }
    Producer producer = { .queue = queue, .values = values };
    Thread threads[4];
    for (size_t i = 0; i < countof(threads); i++) {
        test_check(test, thread_start(&threads[i], produce, &producer, nullptr) == ERR_OK, "start a producer");
    }
    long long sum = 0;
    for (int i = 0; i < 4000; i++) {
        sum += *(int *)queue_pop(queue);
    }
    for (size_t i = 0; i < countof(threads); i++) {
        thread_join(&threads[i]);
    }
    test_check(test, sum == 4LL * 999 * 1000 / 2, "every item arrives exactly once");
    queue_destroy(queue);
}

static void queue_close_races_producers(Test *test)
{
    Queue *queue = queue_create();
    int *values = arena_push(test->arena, 1000 * sizeof *values);
    Producer producer = { .queue = queue, .values = values };
    Thread threads[4];
    for (size_t i = 0; i < countof(threads); i++) {
        test_check(test, thread_start(&threads[i], produce, &producer, nullptr) == ERR_OK, "start a producer");
    }
    int popped = 0;
    while (popped < 100 && queue_pop(queue) != nullptr) {
        popped++;
    }
    queue_close(queue);
    while (queue_pop(queue) != nullptr) {
        popped++;
    }
    for (size_t i = 0; i < countof(threads); i++) {
        thread_join(&threads[i]);
    }
    test_check(test, popped == atomic_load(&producer.accepted), "every accepted item is popped");
    queue_destroy(queue);
}

typedef struct {
    Task task;
    atomic_int *done;
} Counted;

static void count_one(void *argument)
{
    Counted *counted = argument;
    atomic_fetch_add(counted->done, 1);
}

typedef struct {
    const int *values;
    long long *sums;
} Summing;

static void sum_chunk(void *context, size_t low, size_t high, size_t chunk)
{
    Summing *summing = context;
    for (size_t i = low; i < high; i++) {
        summing->sums[chunk] += summing->values[i];
    }
}

static void threadpool_runs_tasks_and_chunks(Test *test)
{
    ThreadPool *pool;
    Err err = { 0 };
    test_check(test, threadpool_create(4, &pool, &err) == ERR_OK && threadpool_size(pool) == 4, "start a pool");

    atomic_int done = 0;
    Counted *tasks = arena_push(test->arena, 100 * sizeof *tasks);
    for (size_t i = 0; i < 100; i++) {
        tasks[i] = (Counted){ .task = { .run = count_one, .argument = &tasks[i] }, .done = &done };
        threadpool_submit(pool, &tasks[i].task);
    }

    int *values = arena_push(test->arena, 10000 * sizeof *values);
    for (int i = 0; i < 10000; i++) {
        values[i] = i;
    }
    ParallelPlan plan = parallel_plan(pool, 10000, 100);
    test_check(test, plan.chunks == 4 && plan.chunk_size == 2500, "the plan gives each worker a chunk");
    test_check(test, parallel_plan(pool, 10, 100).chunks == 1, "small inputs stay in one chunk");
    Summing summing = { .values = values, .sums = arena_push(test->arena, plan.chunks * sizeof *summing.sums) };
    threadpool_run_chunks(pool, test->arena, plan, sum_chunk, &summing);
    long long total = 0;
    for (size_t c = 0; c < plan.chunks; c++) {
        total += summing.sums[c];
    }
    test_check(test, total == 9999LL * 10000 / 2, "every chunk ran once");

    threadpool_destroy(pool);
    test_check(test, atomic_load(&done) == 100, "destroy runs the queued tasks first");
}

static void square_index(void *context, size_t index)
{
    size_t *squares = context;
    squares[index] = index * index;
}

static void threads_run_each_runs_every_index(Test *test)
{
    size_t *squares = arena_push(test->arena, 8 * sizeof *squares);
    Err err = { 0 };
    test_check(test, threads_run_each(test->arena, 8, square_index, squares, &err) == ERR_OK, "run eight jobs");
    test_check(test, squares[0] == 0 && squares[7] == 49, "each job got its index");
}

typedef struct {
    Mutex mutex;
    Cond fired;
    Arena *arena;
    StringList keys;
    int64_t fired_ns;
} Firings;

static void record_firing(void *context, String key)
{
    Firings *firings = context;
    mutex_lock(&firings->mutex);
    strlist_push(firings->arena, &firings->keys, str_copy(firings->arena, key));
    firings->fired_ns = clock_monotonic_ns();
    cond_broadcast(&firings->fired);
    mutex_unlock(&firings->mutex);
}

static bool await_firings(Firings *firings, size_t count)
{
    int64_t deadline = clock_monotonic_ns() + 5LL * NS_PER_SECOND;
    mutex_lock(&firings->mutex);
    while (firings->keys.count < count && clock_monotonic_ns() < deadline) {
        cond_wait_until(&firings->fired, &firings->mutex, deadline);
    }
    bool reached = firings->keys.count >= count;
    mutex_unlock(&firings->mutex);
    return reached;
}

static void debounce_fires_each_key_once_after_quiet(Test *test)
{
    Firings firings = { .arena = test->arena };
    mutex_init(&firings.mutex);
    cond_init(&firings.fired);
    Debouncer *debouncer;
    const int64_t delay = 40 * NS_PER_MILLISECOND;
    if (debounce_create(delay, record_firing, &firings, &debouncer, nullptr) != ERR_OK) {
        test_check(test, false, "create a debouncer");
        return;
    }
    int64_t last_trigger = 0;
    for (int i = 0; i < 4; i++) {
        debounce_trigger(debouncer, S("a"));
        last_trigger = clock_monotonic_ns();
        clock_sleep_ms(10);
    }
    debounce_trigger(debouncer, S("b"));
    test_check(test, debounce_pending(debouncer) == 2, "two keys wait");
    test_check(test, await_firings(&firings, 2), "both keys fire");
    clock_sleep_ms(80);
    mutex_lock(&firings.mutex);
    test_check_str(test, str_join(test->arena, firings.keys, S(",")), S("a,b"), "each key fires once");
    mutex_unlock(&firings.mutex);
    test_check(test, firings.fired_ns - last_trigger >= delay, "a retrigger pushes the deadline back");
    test_check(test, debounce_pending(debouncer) == 0, "nothing waits after firing");

    debounce_trigger(debouncer, S("again"));
    test_check(test, await_firings(&firings, 3), "a key can fire again after the waiting keys were reset");
    debounce_destroy(debouncer);

    Debouncer *dropping;
    test_check(test, debounce_create(10LL * NS_PER_SECOND, record_firing, &firings, &dropping, nullptr) == ERR_OK,
               "create another");
    debounce_trigger(dropping, S("never"));
    debounce_destroy(dropping);
    test_check(test, firings.keys.count == 3, "destroying drops waiting keys");
    cond_destroy(&firings.fired);
    mutex_destroy(&firings.mutex);
}

const TestCase CONCURRENCY_TESTS[] = {
    { "cancel_wakes_sleepers", cancel_wakes_sleepers },
    { "queue_keeps_order_across_growth", queue_keeps_order_across_growth },
    { "queue_serves_many_producers", queue_serves_many_producers },
    { "queue_close_races_producers", queue_close_races_producers },
    { "threadpool_runs_tasks_and_chunks", threadpool_runs_tasks_and_chunks },
    { "threads_run_each_runs_every_index", threads_run_each_runs_every_index },
    { "debounce_fires_each_key_once_after_quiet", debounce_fires_each_key_once_after_quiet },
    { nullptr, nullptr },
};
