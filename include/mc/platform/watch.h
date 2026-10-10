#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// Watch reports changes below directory trees, from FSEvents on macOS and
// inotify on Linux. Their events are unreliable in order and detail, so an
// event names only a path to look at again: stat it to learn whether it was
// created, changed or removed. Programs that link libmc on macOS add
// -framework CoreServices.
typedef struct Watch Watch;

typedef struct WatchEvent {
    // path is a root or below one, spelled from the root as given, cleaned.
    String path;
    // rescan says that changes below path may have gone unreported, so its
    // whole tree needs a fresh look. It is set for a directory moved in or
    // out, whose contents move without events of their own, for a root that
    // moved, and when the system dropped events. inotify also sets it for a
    // new directory, since files can land in it before it is watched.
    bool rescan;
} WatchEvent;

typedef struct WatchEventList {
    WatchEvent *items;
    size_t count;
    size_t capacity;
} WatchEventList;

// WatchSkip is asked about each directory below the roots, on the thread
// that calls watch_create or watch_read. True leaves what is below it
// unwatched and unreported; the directory itself, an entry of its parent,
// is still reported.
typedef bool (*WatchSkip)(void *context, String directory);

// watch_create watches each root, a directory, and everything below it; skip
// may be nullptr. On Linux running out of inotify watches is ERR_PLATFORM.
[[nodiscard]] Error watch_create(StringList roots, WatchSkip skip, void *context, Watch **watch, Err *err);
void watch_destroy(Watch *watch);

// watch_read waits up to timeout_ms for a change. After the first it keeps
// collecting until settle_ms pass without another, but for no longer than
// eight times settle_ms, so a path changed many times in a row is appended
// to events once. No change within timeout_ms is ERR_OK with nothing
// appended. On an error, events keeps what was collected before it.
[[nodiscard]] Error watch_read(Watch *watch, Arena *arena, int timeout_ms, int settle_ms, WatchEventList *events,
                               Err *err);
