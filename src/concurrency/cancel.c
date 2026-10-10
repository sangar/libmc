#include "mc/concurrency/cancel.h"

void cancel_init(Cancel *cancel)
{
    atomic_init(&cancel->requested, false);
    mutex_init(&cancel->mutex);
    cond_init(&cancel->cond);
}

void cancel_destroy(Cancel *cancel)
{
    cond_destroy(&cancel->cond);
    mutex_destroy(&cancel->mutex);
}

void cancel_request(Cancel *cancel)
{
    mutex_lock(&cancel->mutex);
    atomic_store(&cancel->requested, true);
    cond_broadcast(&cancel->cond);
    mutex_unlock(&cancel->mutex);
}

bool cancel_requested(Cancel *cancel)
{
    return atomic_load(&cancel->requested);
}

void cancel_lock(Cancel *cancel)
{
    mutex_lock(&cancel->mutex);
}

void cancel_unlock(Cancel *cancel)
{
    mutex_unlock(&cancel->mutex);
}

bool cancel_wait(Cancel *cancel, int64_t deadline_ns)
{
    if (cancel_requested(cancel)) {
        return false;
    }
    if (deadline_ns == 0) {
        cond_wait(&cancel->cond, &cancel->mutex);
    } else {
        cond_wait_until(&cancel->cond, &cancel->mutex, deadline_ns);
    }
    return !cancel_requested(cancel);
}

void cancel_notify(Cancel *cancel)
{
    mutex_lock(&cancel->mutex);
    cond_broadcast(&cancel->cond);
    mutex_unlock(&cancel->mutex);
}

void cancel_notify_locked(Cancel *cancel)
{
    cond_broadcast(&cancel->cond);
}

bool cancel_sleep(Cancel *cancel, int64_t milliseconds)
{
    int64_t deadline = clock_monotonic_ns() + milliseconds * NS_PER_MILLISECOND;
    cancel_lock(cancel);
    while (!cancel_requested(cancel) && clock_monotonic_ns() < deadline) {
        cancel_wait(cancel, deadline);
    }
    bool live = !cancel_requested(cancel);
    cancel_unlock(cancel);
    return live;
}
