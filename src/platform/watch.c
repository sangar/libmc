#include "mc/platform/watch.h"

#include "mc/container/strmap.h"
#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "watch_backend.h"

enum { SETTLE_LIMIT = 8 };

struct Watch {
    Arena *arena;   // the watch and what its backend keeps for its lifetime
    Arena *scratch; // one read's raw events
    WatchBackend *backend;
};

[[nodiscard]] static Error check_roots(StringList roots, Err *err)
{
    for (size_t i = 0; i < roots.count; i++) {
        FileInfo info;
        Error e = file_info_follow(roots.items[i], &info, err);
        if (e == ERR_OK && !info.is_dir) {
            e = err_set(err, info.exists ? ERR_INVALID_ARGUMENT : ERR_NOT_FOUND, "watch %.*s: not a directory",
                        (int)roots.items[i].len, roots.items[i].data);
        }
        if (e != ERR_OK) {
            return e;
        }
    }
    return ERR_OK;
}

Error watch_create(StringList roots, WatchSkip skip, void *context, Watch **watch, Err *err)
{
    Error e = check_roots(roots, err);
    if (e != ERR_OK) {
        return e;
    }
    Arena *arena = arena_create(0);
    Watch *created = arena_push(arena, sizeof *created);
    created->arena = arena;
    created->scratch = arena_create(0);
    e = watch_backend_open(arena, roots, skip, context, &created->backend, err);
    if (e != ERR_OK) {
        arena_destroy(created->scratch);
        arena_destroy(arena);
        return e;
    }
    *watch = created;
    return ERR_OK;
}

void watch_destroy(Watch *watch)
{
    if (watch == nullptr) {
        return;
    }
    watch_backend_close(watch->backend);
    arena_destroy(watch->scratch);
    arena_destroy(watch->arena);
}

static int milliseconds_until(int64_t deadline_ns)
{
    int64_t left = deadline_ns - clock_monotonic_ns();
    return left <= 0 ? 0 : (int)((left + NS_PER_MILLISECOND - 1) / NS_PER_MILLISECOND);
}

// append_distinct appends each path in raw to events once, with rescan if any of its events asked for it.
static void append_distinct(Arena *arena, Arena *scratch, WatchEventList raw, WatchEventList *events)
{
    StrMap *positions = strmap_create(scratch, raw.count);
    for (size_t i = 0; i < raw.count; i++) {
        size_t position = (size_t)strmap_get(positions, raw.items[i].path);
        if (position > 0) {
            events->items[position - 1].rescan |= raw.items[i].rescan;
            continue;
        }
        events->items = arena_grow(arena, events->items, &events->capacity, events->count, sizeof *events->items);
        events->items[events->count++] = (WatchEvent){ str_copy(arena, raw.items[i].path), raw.items[i].rescan };
        strmap_put(positions, raw.items[i].path, (void *)events->count);
    }
}

Error watch_read(Watch *watch, Arena *arena, int timeout_ms, int settle_ms, WatchEventList *events, Err *err)
{
    arena_reset(watch->scratch);
    WatchEventList raw = { 0 };
    Error e = watch_backend_wait(watch->backend, watch->scratch, timeout_ms, &raw, err);
    int64_t give_up = clock_monotonic_ns() + (int64_t)settle_ms * SETTLE_LIMIT * NS_PER_MILLISECOND;
    size_t before = 0;
    while (e == ERR_OK && raw.count > before && settle_ms > 0 && clock_monotonic_ns() < give_up) {
        before = raw.count;
        int wait_ms = milliseconds_until(give_up);
        e = watch_backend_wait(watch->backend, watch->scratch, wait_ms < settle_ms ? wait_ms : settle_ms, &raw, err);
    }
    append_distinct(arena, watch->scratch, raw, events);
    return e;
}
