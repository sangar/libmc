#pragma once

#include "mc/core/base.h"
#include "mc/platform/platform.h"

// Waker lets other threads wake one that waits for something to do. Wakes
// that come while nobody waits are kept, and several of them wake it once.
// Waker waker; waker_init(&waker); before use.
typedef struct Waker {
    Mutex mutex;
    Cond cond;
    bool woken;
} Waker;

void waker_init(Waker *waker);
void waker_destroy(Waker *waker);
// waker_wake may be called from any thread.
void waker_wake(Waker *waker);
// waker_wait returns once woken, or after timeout_ms, and reports whether it was woken.
bool waker_wait(Waker *waker, int64_t timeout_ms);
