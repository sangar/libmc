#include "mc/concurrency/waker.h"

void waker_init(Waker *waker)
{
    mutex_init(&waker->mutex);
    cond_init(&waker->cond);
    waker->woken = false;
}

void waker_destroy(Waker *waker)
{
    cond_destroy(&waker->cond);
    mutex_destroy(&waker->mutex);
}

void waker_wake(Waker *waker)
{
    mutex_lock(&waker->mutex);
    waker->woken = true;
    cond_signal(&waker->cond);
    mutex_unlock(&waker->mutex);
}

bool waker_wait(Waker *waker, int64_t timeout_ms)
{
    int64_t deadline = clock_monotonic_ns() + timeout_ms * NS_PER_MILLISECOND;
    mutex_lock(&waker->mutex);
    while (!waker->woken && clock_monotonic_ns() < deadline) {
        cond_wait_until(&waker->cond, &waker->mutex, deadline);
    }
    bool woken = waker->woken;
    waker->woken = false;
    mutex_unlock(&waker->mutex);
    return woken;
}
