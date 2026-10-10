#pragma once

#include "mc/core/base.h"

// Queue is an unbounded, blocking FIFO of pointers shared between threads.
// Its ring buffer lives in an arena of its own and doubles when full, so it
// holds at most twice the most items it ever held at once.
typedef struct Queue Queue;

Queue *queue_create(void);
// queue_destroy needs every thread to be done with the queue.
void queue_destroy(Queue *queue);

// queue_push takes any pointer but nullptr, which queue_pop uses to say the
// queue is closed. It returns false, without queueing the item, once the
// queue is closed.
[[nodiscard]] bool queue_push(Queue *queue, void *item);
// queue_pop blocks until an item arrives; it returns nullptr once the queue is closed and drained.
void *queue_pop(Queue *queue);
// queue_pop_timeout waits at most timeout_ms, 0 not at all; it returns false on timeout or once closed and drained.
bool queue_pop_timeout(Queue *queue, int64_t timeout_ms, void **item);
size_t queue_count(Queue *queue);
// queue_close wakes every waiter and turns away later pushes; items already
// queued can still be popped.
void queue_close(Queue *queue);
