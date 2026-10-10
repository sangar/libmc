#pragma once

#include "mc/platform/watch.h"

// The operating system's half of Watch: watch_darwin.c on macOS,
// watch_linux.c on Linux. watch.c batches what it delivers.
typedef struct WatchBackend WatchBackend;

// watch_backend_open allocates what lives as long as the watch in arena.
[[nodiscard]] Error watch_backend_open(Arena *arena, StringList roots, WatchSkip skip, void *context,
                                       WatchBackend **backend, Err *err);
void watch_backend_close(WatchBackend *backend);
// watch_backend_wait waits up to timeout_ms for events and appends them to
// events in arena, a path possibly more than once.
[[nodiscard]] Error watch_backend_wait(WatchBackend *backend, Arena *arena, int timeout_ms, WatchEventList *events,
                                       Err *err);
