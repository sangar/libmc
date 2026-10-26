#ifdef __APPLE__

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>

#include "mc/core/arena.h"
#include "mc/platform/poll.h"

enum { WAKE_IDENT = 1, MAX_READY = 256 };

struct Poller {
    Arena *arena;
    int queue;
};

Error poller_create(Poller **poller, Err *err)
{
    int queue = kqueue();
    if (queue < 0) {
        return err_set(err, ERR_PLATFORM, "kqueue: %s", strerror(errno));
    }
    struct kevent wake;
    EV_SET(&wake, WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(queue, &wake, 1, nullptr, 0, nullptr) < 0) {
        Error e = err_set(err, ERR_PLATFORM, "kevent: %s", strerror(errno));
        close(queue);
        return e;
    }
    Arena *arena = arena_create(0);
    Poller *created = arena_push(arena, sizeof *created);
    *created = (Poller){ .arena = arena, .queue = queue };
    *poller = created;
    return ERR_OK;
}

void poller_destroy(Poller *poller)
{
    close(poller->queue);
    arena_destroy(poller->arena);
}

Error poller_watch(Poller *poller, int fd, PollInterest interest, void *data, Err *err)
{
    assert(data != nullptr);
    struct kevent changes[2];
    unsigned short read_flags = (interest & POLL_READ) != 0 ? EV_ENABLE : EV_DISABLE;
    unsigned short write_flags = (interest & POLL_WRITE) != 0 ? EV_ENABLE : EV_DISABLE;
    EV_SET(&changes[0], (uintptr_t)fd, EVFILT_READ, EV_ADD | read_flags, 0, 0, data);
    EV_SET(&changes[1], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | write_flags, 0, 0, data);
    if (kevent(poller->queue, changes, 2, nullptr, 0, nullptr) < 0) {
        return err_set(err, ERR_PLATFORM, "kevent: %s", strerror(errno));
    }
    return ERR_OK;
}

size_t poller_wait(Poller *poller, void **ready, size_t capacity, int timeout_ms)
{
    struct kevent events[MAX_READY];
    struct timespec timeout = { .tv_sec = timeout_ms / 1000, .tv_nsec = (long)(timeout_ms % 1000) * 1000000 };
    int count = kevent(poller->queue, nullptr, 0, events, (int)(capacity < MAX_READY ? capacity : MAX_READY),
                       timeout_ms < 0 ? nullptr : &timeout);
    for (int i = 0; i < count; i++) {
        ready[i] = events[i].filter == EVFILT_USER ? nullptr : events[i].udata;
    }
    return count > 0 ? (size_t)count : 0;
}

void poller_wake(Poller *poller)
{
    struct kevent wake;
    EV_SET(&wake, WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    int result = kevent(poller->queue, &wake, 1, nullptr, 0, nullptr);
    assert(result == 0);
    unused(result);
}

#endif
