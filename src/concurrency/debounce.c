#include "mc/concurrency/debounce.h"

#include "mc/container/strmap.h"
#include "mc/core/arena.h"
#include "mc/platform/platform.h"

typedef struct Pending {
    String key;
    int64_t deadline_ns;
} Pending;

struct Debouncer {
    Arena *arena;   // the debouncer itself
    Arena *waiting; // the pending keys and their index, reset whenever none is left
    Arena *firing;  // copies of the keys being fired
    int64_t delay_ns;
    DebounceFunction fire;
    void *context;
    Mutex mutex;
    Cond changed;
    Thread thread;
    bool stopped;
    Pending *pending;
    size_t count;
    size_t capacity;
    StrMap *positions; // key -> its position in pending, stored as position + 1
};

static void forget_all(Debouncer *debouncer)
{
    arena_reset(debouncer->waiting);
    debouncer->pending = nullptr;
    debouncer->count = 0;
    debouncer->capacity = 0;
    debouncer->positions = strmap_create(debouncer->waiting, 0);
}

static void remove_at(Debouncer *debouncer, size_t i)
{
    strmap_remove(debouncer->positions, debouncer->pending[i].key, nullptr);
    debouncer->count--;
    if (i != debouncer->count) {
        debouncer->pending[i] = debouncer->pending[debouncer->count];
        strmap_put(debouncer->positions, debouncer->pending[i].key, (void *)(i + 1));
    }
}

static int64_t earliest_deadline(const Debouncer *debouncer)
{
    int64_t earliest = INT64_MAX;
    for (size_t i = 0; i < debouncer->count; i++) {
        earliest = debouncer->pending[i].deadline_ns < earliest ? debouncer->pending[i].deadline_ns : earliest;
    }
    return earliest;
}

// take_due moves the keys whose deadline has passed into the firing arena; called with the lock held.
static StringList take_due(Debouncer *debouncer, int64_t now)
{
    StringList due = { 0 };
    for (size_t i = 0; i < debouncer->count;) {
        if (debouncer->pending[i].deadline_ns <= now) {
            strlist_push(debouncer->firing, &due, str_copy(debouncer->firing, debouncer->pending[i].key));
            remove_at(debouncer, i);
        } else {
            i++;
        }
    }
    if (debouncer->count == 0) {
        forget_all(debouncer);
    }
    return due;
}

static void *run_timer(void *argument)
{
    Debouncer *debouncer = argument;
    mutex_lock(&debouncer->mutex);
    while (!debouncer->stopped) {
        if (debouncer->count == 0) {
            cond_wait(&debouncer->changed, &debouncer->mutex);
            continue;
        }
        int64_t earliest = earliest_deadline(debouncer);
        int64_t now = clock_monotonic_ns();
        if (earliest > now) {
            cond_wait_until(&debouncer->changed, &debouncer->mutex, earliest);
            continue;
        }
        StringList due = take_due(debouncer, now);
        mutex_unlock(&debouncer->mutex);
        for (size_t i = 0; i < due.count; i++) {
            debouncer->fire(debouncer->context, due.items[i]);
        }
        arena_reset(debouncer->firing);
        mutex_lock(&debouncer->mutex);
    }
    mutex_unlock(&debouncer->mutex);
    return nullptr;
}

Error debounce_create(int64_t delay_ns, DebounceFunction fire, void *context, Debouncer **debouncer, Err *err)
{
    Arena *arena = arena_create(0);
    Debouncer *created = arena_push(arena, sizeof *created);
    created->arena = arena;
    created->waiting = arena_create(0);
    created->firing = arena_create(0);
    created->delay_ns = delay_ns;
    created->fire = fire;
    created->context = context;
    mutex_init(&created->mutex);
    cond_init(&created->changed);
    forget_all(created);
    Error e = thread_start(&created->thread, run_timer, created, err);
    if (e != ERR_OK) {
        cond_destroy(&created->changed);
        mutex_destroy(&created->mutex);
        arena_destroy(created->firing);
        arena_destroy(created->waiting);
        arena_destroy(arena);
        return e;
    }
    *debouncer = created;
    return ERR_OK;
}

void debounce_destroy(Debouncer *debouncer)
{
    if (debouncer == nullptr) {
        return;
    }
    mutex_lock(&debouncer->mutex);
    debouncer->stopped = true;
    cond_broadcast(&debouncer->changed);
    mutex_unlock(&debouncer->mutex);
    thread_join(&debouncer->thread);
    cond_destroy(&debouncer->changed);
    mutex_destroy(&debouncer->mutex);
    arena_destroy(debouncer->firing);
    arena_destroy(debouncer->waiting);
    arena_destroy(debouncer->arena);
}

void debounce_trigger(Debouncer *debouncer, String key)
{
    mutex_lock(&debouncer->mutex);
    int64_t deadline_ns = clock_monotonic_ns() + debouncer->delay_ns;
    size_t position = (size_t)strmap_get(debouncer->positions, key);
    if (position > 0) {
        debouncer->pending[position - 1].deadline_ns = deadline_ns;
    } else {
        debouncer->pending = arena_grow(debouncer->waiting, debouncer->pending, &debouncer->capacity, debouncer->count,
                                        sizeof *debouncer->pending);
        debouncer->pending[debouncer->count] = (Pending){ str_copy(debouncer->waiting, key), deadline_ns };
        strmap_put(debouncer->positions, key, (void *)(debouncer->count + 1));
        debouncer->count++;
    }
    cond_signal(&debouncer->changed);
    mutex_unlock(&debouncer->mutex);
}

size_t debounce_pending(Debouncer *debouncer)
{
    mutex_lock(&debouncer->mutex);
    size_t count = debouncer->count;
    mutex_unlock(&debouncer->mutex);
    return count;
}
