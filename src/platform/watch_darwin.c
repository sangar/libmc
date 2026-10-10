#ifdef __APPLE__

#include <dispatch/dispatch.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "mc/concurrency/queue.h"
#include "mc/core/arena.h"
#include "mc/text/path.h"
#include "watch_backend.h"

// FSEvents watches whole trees and calls back on a dispatch queue with
// resolved paths. The callback hands each call's events over in a Batch;
// watch_backend_wait maps them back to the roots as given and applies skip
// on the reading thread.

// Cross builds have no macOS SDK, so the FSEvents and CoreFoundation API this
// file uses is declared here. Where the SDK is present its header comes too,
// and the compiler checks the declarations and constants below against it.
// It cannot come with mc/platform/platform.h, as both define a FileInfo.
#if __has_include(<CoreServices/CoreServices.h>)
#include <CoreServices/CoreServices.h>
#else
typedef struct CFArrayCallBacks CFArrayCallBacks;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
typedef struct FSEventStreamContext {
    long version;
    void *info;
    const void *(*retain)(const void *info);
    void (*release)(const void *info);
    const struct __CFString *(*copyDescription)(const void *info);
} FSEventStreamContext;
#endif

typedef unsigned char Boolean;
typedef unsigned char UInt8;
typedef unsigned int UInt32;
typedef unsigned long long UInt64;
typedef signed long CFIndex;
typedef double CFTimeInterval;
typedef UInt32 CFStringEncoding;
typedef const void *CFTypeRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFArray *CFMutableArrayRef;
typedef UInt32 FSEventStreamCreateFlags;
typedef UInt32 FSEventStreamEventFlags;
typedef UInt64 FSEventStreamEventId;
typedef struct __FSEventStream *FSEventStreamRef;
typedef const struct __FSEventStream *ConstFSEventStreamRef;
typedef void (*FSEventStreamCallback)(ConstFSEventStreamRef stream, void *info, size_t count, void *paths,
                                      const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]);

void CFRelease(CFTypeRef value);
CFStringRef CFStringCreateWithBytes(CFAllocatorRef allocator, const UInt8 *bytes, CFIndex count,
                                    CFStringEncoding encoding, Boolean external);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef allocator, CFIndex capacity, const CFArrayCallBacks *callbacks);
void CFArrayAppendValue(CFMutableArrayRef array, const void *value);
FSEventStreamRef FSEventStreamCreate(CFAllocatorRef allocator, FSEventStreamCallback callback,
                                     FSEventStreamContext *context, CFArrayRef paths, FSEventStreamEventId since,
                                     CFTimeInterval latency, FSEventStreamCreateFlags flags);
void FSEventStreamSetDispatchQueue(FSEventStreamRef stream, dispatch_queue_t queue);
Boolean FSEventStreamStart(FSEventStreamRef stream);
void FSEventStreamStop(FSEventStreamRef stream);
void FSEventStreamInvalidate(FSEventStreamRef stream);
void FSEventStreamRelease(FSEventStreamRef stream);

enum {
    CREATE_NO_DEFER = 0x00000002,
    CREATE_WATCH_ROOT = 0x00000004,
    CREATE_FILE_EVENTS = 0x00000010,
    EVENT_MUST_SCAN_SUBDIRS = 0x00000001,
    EVENT_USER_DROPPED = 0x00000002,
    EVENT_KERNEL_DROPPED = 0x00000004,
    EVENT_HISTORY_DONE = 0x00000010,
    EVENT_ROOT_CHANGED = 0x00000020,
    EVENT_ITEM_RENAMED = 0x00000800,
    EVENT_ITEM_IS_DIR = 0x00020000,
};
static const FSEventStreamEventId SINCE_NOW = 0xFFFFFFFFFFFFFFFFULL;
static const CFStringEncoding ENCODING_UTF8 = 0x08000100;

#if __has_include(<CoreServices/CoreServices.h>)
static_assert(CREATE_NO_DEFER == kFSEventStreamCreateFlagNoDefer);
static_assert(CREATE_WATCH_ROOT == kFSEventStreamCreateFlagWatchRoot);
static_assert(CREATE_FILE_EVENTS == kFSEventStreamCreateFlagFileEvents);
static_assert(EVENT_MUST_SCAN_SUBDIRS == kFSEventStreamEventFlagMustScanSubDirs);
static_assert(EVENT_USER_DROPPED == kFSEventStreamEventFlagUserDropped);
static_assert(EVENT_KERNEL_DROPPED == kFSEventStreamEventFlagKernelDropped);
static_assert(EVENT_HISTORY_DONE == kFSEventStreamEventFlagHistoryDone);
static_assert(EVENT_ROOT_CHANGED == kFSEventStreamEventFlagRootChanged);
static_assert(EVENT_ITEM_RENAMED == kFSEventStreamEventFlagItemRenamed);
static_assert(EVENT_ITEM_IS_DIR == kFSEventStreamEventFlagItemIsDir);
static_assert(0xFFFFFFFFFFFFFFFFULL == kFSEventStreamEventIdSinceNow);
static_assert(0x08000100 == kCFStringEncodingUTF8);
#endif

static const double LATENCY_SECONDS = 0.05;

typedef struct Root {
    String given;
    String resolved;
} Root;

typedef struct Batch {
    Arena *arena; // the batch itself and its events
    WatchEventList events;
} Batch;

struct WatchBackend {
    Root *roots;
    size_t root_count;
    WatchSkip skip;
    void *context;
    FSEventStreamRef stream;
    dispatch_queue_t queue;
    Queue *batches;
};

// needs_rescan says what is below the path may have changed without events of its own.
static bool needs_rescan(FSEventStreamEventFlags flags)
{
    FSEventStreamEventFlags dropped = EVENT_MUST_SCAN_SUBDIRS | EVENT_USER_DROPPED | EVENT_KERNEL_DROPPED;
    bool moved_directory = (flags & EVENT_ITEM_IS_DIR) && (flags & EVENT_ITEM_RENAMED);
    return (flags & (dropped | EVENT_ROOT_CHANGED)) != 0 || moved_directory;
}

static void on_events(ConstFSEventStreamRef stream, void *info, size_t count, void *paths,
                      const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[])
{
    WatchBackend *backend = info;
    char **list = paths;
    Arena *arena = arena_create(0);
    Batch *batch = arena_push(arena, sizeof *batch);
    batch->arena = arena;
    for (size_t i = 0; i < count; i++) {
        if (flags[i] & EVENT_HISTORY_DONE) {
            continue;
        }
        WatchEventList *events = &batch->events;
        events->items = arena_grow(arena, events->items, &events->capacity, events->count, sizeof *events->items);
        events->items[events->count++] = (WatchEvent){ str_copy(arena, S(list[i])), needs_rescan(flags[i]) };
    }
    if (!queue_push(backend->batches, batch)) {
        arena_destroy(arena);
    }
}

[[nodiscard]] static Error add_root(Arena *arena, String path, Root *root, Err *err)
{
    root->given = path_clean(arena, path);
    char resolved[PATH_MAX];
    if (realpath(str_cstr(arena, root->given), resolved) == nullptr) {
        return err_set(err, ERR_IO, "watch %.*s: %s", (int)path.len, path.data, strerror(errno));
    }
    root->resolved = str_copy(arena, S(resolved));
    return ERR_OK;
}

static void release_batches(WatchBackend *backend)
{
    queue_close(backend->batches);
    void *batch;
    while ((batch = queue_pop(backend->batches)) != nullptr) {
        arena_destroy(((Batch *)batch)->arena);
    }
    queue_destroy(backend->batches);
}

Error watch_backend_open(Arena *arena, StringList roots, WatchSkip skip, void *context, WatchBackend **backend,
                         Err *err)
{
    WatchBackend *created = arena_push(arena, sizeof *created);
    created->roots = arena_push(arena, arena_size_mul(roots.count, sizeof *created->roots));
    created->root_count = roots.count;
    created->skip = skip;
    created->context = context;
    for (size_t i = 0; i < roots.count; i++) {
        Error e = add_root(arena, roots.items[i], &created->roots[i], err);
        if (e != ERR_OK) {
            return e;
        }
    }
    created->batches = queue_create();

    CFMutableArrayRef paths = CFArrayCreateMutable(nullptr, (CFIndex)roots.count, &kCFTypeArrayCallBacks);
    for (size_t i = 0; i < roots.count; i++) {
        String resolved = created->roots[i].resolved;
        CFStringRef path = CFStringCreateWithBytes(nullptr, (const UInt8 *)resolved.data, (CFIndex)resolved.len,
                                                   ENCODING_UTF8, false);
        CFArrayAppendValue(paths, path);
        CFRelease(path);
    }
    FSEventStreamContext stream_context = { .info = created };
    created->stream = FSEventStreamCreate(nullptr, on_events, &stream_context, paths, SINCE_NOW,
                                          LATENCY_SECONDS,
                                          CREATE_FILE_EVENTS | CREATE_NO_DEFER | CREATE_WATCH_ROOT);
    CFRelease(paths);
    if (created->stream == nullptr) {
        release_batches(created);
        return err_set(err, ERR_PLATFORM, "FSEvents: cannot create an event stream");
    }
    created->queue = dispatch_queue_create("mc.watch", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(created->stream, created->queue);
    if (!FSEventStreamStart(created->stream)) {
        FSEventStreamInvalidate(created->stream);
        FSEventStreamRelease(created->stream);
        dispatch_release(created->queue);
        release_batches(created);
        return err_set(err, ERR_PLATFORM, "FSEvents: cannot start the event stream");
    }
    *backend = created;
    return ERR_OK;
}

// Running an empty task on the serial queue waits out a callback still in flight.
static void do_nothing(void *context)
{
}

void watch_backend_close(WatchBackend *backend)
{
    FSEventStreamStop(backend->stream);
    FSEventStreamInvalidate(backend->stream);
    FSEventStreamRelease(backend->stream);
    dispatch_sync_f(backend->queue, nullptr, do_nothing);
    dispatch_release(backend->queue);
    release_batches(backend);
}

// as_given spells a resolved path from FSEvents under the root as given; false when it is under no root.
static bool as_given(Arena *arena, const WatchBackend *backend, String path, const Root **root, String *given)
{
    for (size_t i = 0; i < backend->root_count; i++) {
        String below;
        *root = &backend->roots[i];
        if (str_equal(path, (*root)->resolved)) {
            *given = (*root)->given;
            return true;
        }
        if (path_relative((*root)->resolved, path, &below)) {
            *given = path_join(arena, (*root)->given, below);
            return true;
        }
    }
    return false;
}

// skipped asks skip about each directory between the root and path.
static bool skipped(const WatchBackend *backend, const Root *root, String path)
{
    if (backend->skip == nullptr) {
        return false;
    }
    String below;
    if (!path_relative(root->given, path, &below)) {
        return false;
    }
    size_t prefix = path.len - below.len;
    for (size_t i = 0; i < below.len; i++) {
        if (below.data[i] == '/' && backend->skip(backend->context, str_slice(path, 0, prefix + i))) {
            return true;
        }
    }
    return false;
}

// take appends the events of batch that are below a root and not skipped.
static void take(const WatchBackend *backend, Arena *arena, Batch *batch, WatchEventList *events)
{
    for (size_t i = 0; i < batch->events.count; i++) {
        const Root *root;
        String path;
        String reported = str_trim_suffix(batch->events.items[i].path, S("/"));
        if (!as_given(arena, backend, reported, &root, &path) || skipped(backend, root, path)) {
            continue;
        }
        events->items = arena_grow(arena, events->items, &events->capacity, events->count, sizeof *events->items);
        events->items[events->count++] = (WatchEvent){ str_copy(arena, path), batch->events.items[i].rescan };
    }
    arena_destroy(batch->arena);
}

Error watch_backend_wait(WatchBackend *backend, Arena *arena, int timeout_ms, WatchEventList *events, Err *err)
{
    void *batch;
    if (queue_pop_timeout(backend->batches, timeout_ms, &batch)) {
        take(backend, arena, batch, events);
        while (queue_pop_timeout(backend->batches, 0, &batch)) {
            take(backend, arena, batch, events);
        }
    }
    return ERR_OK;
}

#endif
