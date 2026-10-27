#ifdef __linux__

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "mc/core/arena.h"
#include "mc/core/base.h"
#include "mc/platform/poll.h"

enum { MAX_READY = 256 };

struct Poller {
    Arena *arena;
    int queue;
    int wake;
};

Error poller_create(Poller **poller, Err *err)
{
    int queue = epoll_create1(EPOLL_CLOEXEC);
    if (queue < 0) {
        return err_set(err, ERR_PLATFORM, "epoll_create1: %s", strerror(errno));
    }
    int wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct epoll_event event = { .events = EPOLLIN, .data.ptr = nullptr };
    if (wake < 0 || epoll_ctl(queue, EPOLL_CTL_ADD, wake, &event) < 0) {
        Error e = err_set(err, ERR_PLATFORM, "eventfd: %s", strerror(errno));
        if (wake >= 0) {
            close(wake);
        }
        close(queue);
        return e;
    }
    Arena *arena = arena_create(0);
    Poller *created = arena_push(arena, sizeof *created);
    *created = (Poller){ .arena = arena, .queue = queue, .wake = wake };
    *poller = created;
    return ERR_OK;
}

void poller_destroy(Poller *poller)
{
    close(poller->wake);
    close(poller->queue);
    arena_destroy(poller->arena);
}

// poller_watch removes a socket watched for nothing, since epoll reports a
// hangup even then, which would wake the loop again and again.
Error poller_watch(Poller *poller, int fd, PollInterest interest, void *data, Err *err)
{
    assert(data != nullptr);
    if (interest == POLL_NONE) {
        if (epoll_ctl(poller->queue, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT) {
            return err_set(err, ERR_PLATFORM, "epoll_ctl: %s", strerror(errno));
        }
        return ERR_OK;
    }
    uint32_t events = ((interest & POLL_READ) != 0 ? EPOLLIN : 0) | ((interest & POLL_WRITE) != 0 ? EPOLLOUT : 0);
    struct epoll_event event = { .events = events, .data.ptr = data };
    int result = epoll_ctl(poller->queue, EPOLL_CTL_MOD, fd, &event);
    if (result < 0 && errno == ENOENT) {
        result = epoll_ctl(poller->queue, EPOLL_CTL_ADD, fd, &event);
    }
    if (result < 0) {
        return err_set(err, ERR_PLATFORM, "epoll_ctl: %s", strerror(errno));
    }
    return ERR_OK;
}

size_t poller_wait(Poller *poller, void **ready, size_t capacity, int timeout_ms)
{
    struct epoll_event events[MAX_READY];
    int count = epoll_wait(poller->queue, events, (int)(capacity < MAX_READY ? capacity : MAX_READY), timeout_ms);
    for (int i = 0; i < count; i++) {
        ready[i] = events[i].data.ptr;
        if (ready[i] == nullptr) {
            // Reading resets the count; a read that finds it already reset is as good.
            eventfd_t wakes = 0;
            unused(eventfd_read(poller->wake, &wakes));
        }
    }
    return count > 0 ? (size_t)count : 0;
}

void poller_wake(Poller *poller)
{
    // Adding fails only when the count would overflow, and then a wake is already pending.
    unused(eventfd_write(poller->wake, 1));
}

#endif
