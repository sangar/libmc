#include "mc/concurrency/queue.h"

#include <assert.h>

#include "mc/core/arena.h"
#include "mc/platform/platform.h"

enum { INITIAL_CAPACITY = 64 };

struct Queue {
    Arena *arena; // holds the queue itself and every ring buffer it grew through
    Mutex mutex;
    Cond nonempty;
    void **items;
    size_t head;
    size_t count;
    size_t capacity;
    bool closed;
};

Queue *queue_create(void)
{
    Arena *arena = arena_create(0);
    Queue *queue = arena_push(arena, sizeof(Queue));
    queue->arena = arena;
    queue->capacity = INITIAL_CAPACITY;
    queue->items = arena_push(arena, arena_size_mul(queue->capacity, sizeof *queue->items));
    mutex_init(&queue->mutex);
    cond_init(&queue->nonempty);
    return queue;
}

void queue_destroy(Queue *queue)
{
    if (queue == nullptr) {
        return;
    }
    cond_destroy(&queue->nonempty);
    mutex_destroy(&queue->mutex);
    arena_destroy(queue->arena);
}

static void grow(Queue *queue)
{
    size_t capacity = arena_size_mul(queue->capacity, 2);
    void **items = arena_push(queue->arena, arena_size_mul(capacity, sizeof *items));
    for (size_t i = 0; i < queue->count; i++) {
        items[i] = queue->items[(queue->head + i) % queue->capacity];
    }
    queue->items = items;
    queue->capacity = capacity;
    queue->head = 0;
}

bool queue_push(Queue *queue, void *item)
{
    assert(item != nullptr);
    mutex_lock(&queue->mutex);
    if (queue->closed) {
        mutex_unlock(&queue->mutex);
        return false;
    }
    if (queue->count == queue->capacity) {
        grow(queue);
    }
    queue->items[(queue->head + queue->count) % queue->capacity] = item;
    queue->count++;
    cond_signal(&queue->nonempty);
    mutex_unlock(&queue->mutex);
    return true;
}

static void *take(Queue *queue)
{
    void *item = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    return item;
}

void *queue_pop(Queue *queue)
{
    mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->closed) {
        cond_wait(&queue->nonempty, &queue->mutex);
    }
    void *item = queue->count > 0 ? take(queue) : nullptr;
    mutex_unlock(&queue->mutex);
    return item;
}

bool queue_pop_timeout(Queue *queue, int64_t timeout_ms, void **item)
{
    int64_t deadline = clock_monotonic_ns() + timeout_ms * NS_PER_MILLISECOND;
    mutex_lock(&queue->mutex);
    while (queue->count == 0 && !queue->closed && clock_monotonic_ns() < deadline) {
        cond_wait_until(&queue->nonempty, &queue->mutex, deadline);
    }
    bool got = queue->count > 0;
    if (got) {
        *item = take(queue);
    }
    mutex_unlock(&queue->mutex);
    return got;
}

size_t queue_count(Queue *queue)
{
    mutex_lock(&queue->mutex);
    size_t count = queue->count;
    mutex_unlock(&queue->mutex);
    return count;
}

void queue_close(Queue *queue)
{
    mutex_lock(&queue->mutex);
    queue->closed = true;
    cond_broadcast(&queue->nonempty);
    mutex_unlock(&queue->mutex);
}
