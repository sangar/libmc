#pragma once

#include <stdatomic.h>

#include "mc/platform/platform.h"

// Cancel is a cancellation token shared by the threads of one run.
// Cancelling it wakes everything waiting on it. Work that waits for either
// its own condition or cancellation takes the lock, checks its condition and
// calls cancel_wait, so one condition variable serves both.
typedef struct Cancel {
    atomic_bool requested;
    Mutex mutex;
    Cond cond;
} Cancel;

void cancel_init(Cancel *cancel);
void cancel_destroy(Cancel *cancel);
void cancel_request(Cancel *cancel);
bool cancel_requested(Cancel *cancel);

void cancel_lock(Cancel *cancel);
void cancel_unlock(Cancel *cancel);
// cancel_wait, called with the lock held, releases it until cancel_notify,
// cancellation or the clock_monotonic_ns deadline (0 waits without one),
// then holds it again. It returns false once cancellation was requested.
bool cancel_wait(Cancel *cancel, int64_t deadline_ns);
void cancel_notify(Cancel *cancel);
// cancel_notify_locked is cancel_notify for a caller that holds the lock.
void cancel_notify_locked(Cancel *cancel);

// cancel_sleep waits for milliseconds, or less when cancelled, and reports whether the run is still live.
bool cancel_sleep(Cancel *cancel, int64_t milliseconds);
