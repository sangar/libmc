#pragma once

#include <stddef.h>

#include "mc/core/error.h"

// Poller waits on many sockets at once, for an event loop: kqueue on macOS,
// epoll on Linux. One thread uses a poller, except poller_wake, which any
// thread may call. Watches are level-triggered: a socket stays ready until it
// is read or written. The net_try_* calls in mc/platform/platform.h move the
// bytes without waiting.
typedef struct Poller Poller;

typedef enum PollInterest {
    POLL_NONE = 0,
    POLL_READ = 1,
    POLL_WRITE = 2,
} PollInterest;

// poller_create makes a poller; the caller owns it.
[[nodiscard]] Error poller_create(Poller **poller, Err *err);
void poller_destroy(Poller *poller);

// poller_watch sets what fd is watched for, replacing what it was watched for
// before; POLL_NONE stops watching it. data, which must not be nullptr, comes
// back from poller_wait when fd is ready.
[[nodiscard]] Error poller_watch(Poller *poller, int fd, PollInterest interest, void *data, Err *err);

// poller_wait waits up to timeout_ms, -1 without limit, for a watched socket
// to be ready or a wake, and stores the data of at most capacity ready
// sockets in ready. A wake comes back as nullptr. A socket ready to be read
// and written can come back twice.
size_t poller_wait(Poller *poller, void **ready, size_t capacity, int timeout_ms);

// poller_wake makes the poller's poller_wait return, now or when it next waits.
void poller_wake(Poller *poller);
