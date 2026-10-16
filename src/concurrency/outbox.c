#include "mc/concurrency/outbox.h"

#include <string.h>

#include "mc/core/arena.h"
#include "mc/platform/platform.h"

enum { INITIAL_CAPACITY = 4096 };

struct Outbox {
    Arena *arena;
    Mutex mutex;
    Cond ready;
    // Bytes are put into arenas[active]; take hands them over and switches
    // to the other arena, which the bytes taken before were in.
    Arena *arenas[2];
    size_t active;
    char *data;
    size_t len;
    size_t capacity;
    size_t limit;
    bool closed;
};

Outbox *outbox_create(size_t limit)
{
    Arena *arena = arena_create(0);
    Outbox *outbox = arena_push(arena, sizeof *outbox);
    outbox->arena = arena;
    mutex_init(&outbox->mutex);
    cond_init(&outbox->ready);
    outbox->arenas[0] = arena_create(0);
    outbox->arenas[1] = arena_create(0);
    outbox->limit = limit;
    return outbox;
}

void outbox_destroy(Outbox *outbox)
{
    if (outbox == nullptr) {
        return;
    }
    cond_destroy(&outbox->ready);
    mutex_destroy(&outbox->mutex);
    arena_destroy(outbox->arenas[0]);
    arena_destroy(outbox->arenas[1]);
    arena_destroy(outbox->arena);
}

// reserve makes room for more bytes after len; the mutex must be held.
static void reserve(Outbox *outbox, size_t more)
{
    size_t needed = outbox->len + more;
    if (needed <= outbox->capacity) {
        return;
    }
    size_t capacity = outbox->capacity < INITIAL_CAPACITY ? INITIAL_CAPACITY : arena_size_mul(outbox->capacity, 2);
    capacity = capacity < needed ? needed : capacity;
    char *grown = arena_push_aligned(outbox->arenas[outbox->active], capacity, 1);
    if (outbox->len > 0) {
        memcpy(grown, outbox->data, outbox->len);
    }
    outbox->data = grown;
    outbox->capacity = capacity;
}

bool outbox_put_parts(Outbox *outbox, const String *parts, size_t count)
{
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total = arena_size_add(total, parts[i].len);
    }
    mutex_lock(&outbox->mutex);
    bool taken = !outbox->closed && total <= outbox->limit - min_size(outbox->len, outbox->limit);
    if (taken) {
        reserve(outbox, total);
        for (size_t i = 0; i < count; i++) {
            if (parts[i].len > 0) {
                memcpy(outbox->data + outbox->len, parts[i].data, parts[i].len);
                outbox->len += parts[i].len;
            }
        }
    } else {
        outbox->closed = true;
    }
    cond_signal(&outbox->ready);
    mutex_unlock(&outbox->mutex);
    return taken;
}

bool outbox_put(Outbox *outbox, String bytes)
{
    return outbox_put_parts(outbox, &bytes, 1);
}

String outbox_take(Outbox *outbox)
{
    mutex_lock(&outbox->mutex);
    while (outbox->len == 0 && !outbox->closed) {
        cond_wait(&outbox->ready, &outbox->mutex);
    }
    String taken = outbox->len > 0 ? (String){ outbox->data, outbox->len } : S("");
    outbox->active ^= 1;
    arena_reset(outbox->arenas[outbox->active]);
    outbox->data = nullptr;
    outbox->len = 0;
    outbox->capacity = 0;
    mutex_unlock(&outbox->mutex);
    return taken;
}

void outbox_close(Outbox *outbox)
{
    mutex_lock(&outbox->mutex);
    outbox->closed = true;
    cond_signal(&outbox->ready);
    mutex_unlock(&outbox->mutex);
}
