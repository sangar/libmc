#ifdef __linux__
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mc/container/strmap.h"
#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "watch_backend.h"

// inotify watches single directories, so every directory below the roots
// gets a watch, and directories that appear later get theirs as they are
// reported. Each watch descriptor maps back to its directory's path.

enum { COMPACT_AFTER = 1024 };

typedef struct Directory {
    int descriptor;
    String path;
    bool root;
} Directory;

struct WatchBackend {
    int fd;
    StringList roots;
    WatchSkip skip;
    void *context;
    Arena *directory_arena; // the directories and their index, rebuilt once mostly stale
    StrMap *directories;    // watch descriptor -> Directory
    size_t forgotten;       // directories removed since the last rebuild
    // A directory moved away is held here until the event that follows shows
    // whether it moved within the roots, as IN_MOVED_TO with the same cookie.
    bool moving;
    uint32_t moved_cookie;
    String moved_from;
};

static String descriptor_key(const int *descriptor)
{
    return (String){ (const char *)descriptor, sizeof *descriptor };
}

[[nodiscard]] static Error fail_watch(Err *err, int code, String path)
{
    if (code == ENOSPC) {
        return err_set(err, ERR_PLATFORM,
                       "watch %.*s: out of inotify watches; raise fs.inotify.max_user_watches with sysctl",
                       (int)path.len, path.data);
    }
    return err_set(err, code == EMFILE ? ERR_PLATFORM : ERR_IO, "watch %.*s: %s", (int)path.len, path.data,
                   strerror(code));
}

static void remember(WatchBackend *backend, int descriptor, String path, bool root)
{
    Directory *directory = arena_push(backend->directory_arena, sizeof *directory);
    *directory = (Directory){ descriptor, str_copy(backend->directory_arena, path), root };
    strmap_put(backend->directories, descriptor_key(&directory->descriptor), directory);
}

// rebuild copies the live directories into a fresh arena, dropping what removed ones left behind.
static void rebuild(WatchBackend *backend)
{
    Arena *old_arena = backend->directory_arena;
    StrMap *old = backend->directories;
    backend->directory_arena = arena_create(0);
    backend->directories = strmap_create(backend->directory_arena, strmap_count(old));
    backend->forgotten = 0;
    StrMapIterator iterator = strmap_iterate(old);
    String key;
    void *value;
    while (strmap_next(&iterator, &key, &value)) {
        Directory *directory = value;
        remember(backend, directory->descriptor, directory->path, directory->root);
    }
    arena_destroy(old_arena);
}

static void forget(WatchBackend *backend, int descriptor)
{
    if (strmap_remove(backend->directories, descriptor_key(&descriptor), nullptr) &&
        ++backend->forgotten > COMPACT_AFTER && backend->forgotten > strmap_count(backend->directories)) {
        rebuild(backend);
    }
}

static bool is_dot_or_dot_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

// watch_mask follows a root given as a symlink, but no symlink below it.
static uint32_t watch_mask(bool root)
{
    uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO |
                    IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
    return root ? mask : mask | IN_DONT_FOLLOW;
}

static bool skipped(const WatchBackend *backend, String directory)
{
    return backend->skip != nullptr && backend->skip(backend->context, directory);
}

// watch_tree watches the directory at path and every directory below it
// that skip lets through. A directory that vanishes on the way is not an error.
[[nodiscard]] static Error watch_tree(WatchBackend *backend, Arena *scratch, String path, bool root, Err *err)
{
    const char *cpath = str_cstr(scratch, path);
    int descriptor = inotify_add_watch(backend->fd, cpath, watch_mask(root));
    if (descriptor < 0) {
        bool gone = errno == ENOENT || errno == ENOTDIR || errno == EACCES;
        return gone && !root ? ERR_OK : fail_watch(err, errno, path);
    }
    remember(backend, descriptor, path, root);
    DIR *dir = opendir(cpath);
    if (dir == nullptr) {
        return ERR_OK;
    }
    Error e = ERR_OK;
    struct dirent *entry;
    while (e == ERR_OK && (entry = readdir(dir)) != nullptr) {
        if (is_dot_or_dot_dot(entry->d_name)) {
            continue;
        }
        bool is_dir = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
        }
        ArenaMark mark = arena_mark(scratch);
        String child = path_join(scratch, path, S(entry->d_name));
        if (is_dir && !skipped(backend, child)) {
            e = watch_tree(backend, scratch, child, false, err);
        }
        arena_release(mark);
    }
    closedir(dir);
    return e;
}

// unwatch_tree removes the watches of path and the directories below it,
// which moved out of the roots. inotify then reports IN_IGNORED for each.
static void unwatch_tree(WatchBackend *backend, String path)
{
    StrMapIterator iterator = strmap_iterate(backend->directories);
    String key;
    void *value;
    while (strmap_next(&iterator, &key, &value)) {
        Directory *directory = value;
        String below;
        if (str_equal(directory->path, path) || path_relative(path, directory->path, &below)) {
            inotify_rm_watch(backend->fd, directory->descriptor);
        }
    }
}

// repath gives path and the directories below it, which moved within the roots, their new paths.
static void repath(WatchBackend *backend, String from, String to)
{
    StrMapIterator iterator = strmap_iterate(backend->directories);
    String key;
    void *value;
    while (strmap_next(&iterator, &key, &value)) {
        Directory *directory = value;
        String below;
        if (str_equal(directory->path, from)) {
            directory->path = str_copy(backend->directory_arena, to);
        } else if (path_relative(from, directory->path, &below)) {
            directory->path = path_join(backend->directory_arena, to, below);
        }
    }
}

static void finish_move_away(WatchBackend *backend)
{
    if (backend->moving) {
        unwatch_tree(backend, backend->moved_from);
        backend->moving = false;
    }
}

Error watch_backend_open(Arena *arena, StringList roots, WatchSkip skip, void *context, WatchBackend **backend,
                         Err *err)
{
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        return err_set(err, ERR_PLATFORM, "inotify: %s", strerror(errno));
    }
    WatchBackend *created = arena_push(arena, sizeof *created);
    created->fd = fd;
    created->skip = skip;
    created->context = context;
    created->directory_arena = arena_create(0);
    created->directories = strmap_create(created->directory_arena, 0);
    Arena *scratch = arena_create(0);
    Error e = ERR_OK;
    for (size_t i = 0; e == ERR_OK && i < roots.count; i++) {
        String root = path_clean(arena, roots.items[i]);
        strlist_push(arena, &created->roots, root);
        e = watch_tree(created, scratch, root, true, err);
    }
    arena_destroy(scratch);
    if (e != ERR_OK) {
        watch_backend_close(created);
        return e;
    }
    *backend = created;
    return ERR_OK;
}

void watch_backend_close(WatchBackend *backend)
{
    close(backend->fd);
    arena_destroy(backend->directory_arena);
}

static void append(Arena *arena, WatchEventList *events, String path, bool rescan)
{
    events->items = arena_grow(arena, events->items, &events->capacity, events->count, sizeof *events->items);
    events->items[events->count++] = (WatchEvent){ path, rescan };
}

[[nodiscard]] static Error handle(WatchBackend *backend, Arena *arena, const struct inotify_event *event,
                                  WatchEventList *events, Err *err)
{
    if (event->mask & IN_Q_OVERFLOW) {
        for (size_t i = 0; i < backend->roots.count; i++) {
            append(arena, events, backend->roots.items[i], true);
        }
        return ERR_OK;
    }
    bool moved_within = (event->mask & IN_MOVED_TO) && event->cookie == backend->moved_cookie;
    if (!moved_within) {
        finish_move_away(backend);
    }
    Directory *directory = strmap_get(backend->directories, descriptor_key(&event->wd));
    if (directory == nullptr) {
        return ERR_OK;
    }
    if (event->mask & IN_IGNORED) {
        forget(backend, event->wd);
        return ERR_OK;
    }
    if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        if (directory->root) {
            append(arena, events, directory->path, true);
        }
        return ERR_OK;
    }
    if (event->len == 0 || event->name[0] == '\0') {
        append(arena, events, directory->path, false);
        return ERR_OK;
    }
    String path = path_join(arena, directory->path, S(event->name));
    bool is_dir = (event->mask & IN_ISDIR) != 0;
    if (is_dir && (event->mask & IN_MOVED_FROM)) {
        backend->moving = true;
        backend->moved_cookie = event->cookie;
        backend->moved_from = path;
        append(arena, events, path, true);
        return ERR_OK;
    }
    bool appeared = is_dir && (event->mask & (IN_CREATE | IN_MOVED_TO)) && !skipped(backend, path);
    append(arena, events, path, appeared);
    if (!appeared) {
        return ERR_OK;
    }
    if ((event->mask & IN_MOVED_TO) && backend->moving && event->cookie == backend->moved_cookie) {
        repath(backend, backend->moved_from, path);
        backend->moving = false;
        return ERR_OK;
    }
    // Files can land in a new directory before its watch is added, which is why the event asks for a rescan.
    return watch_tree(backend, arena, path, false, err);
}

Error watch_backend_wait(WatchBackend *backend, Arena *arena, int timeout_ms, WatchEventList *events, Err *err)
{
    struct pollfd ready = { .fd = backend->fd, .events = POLLIN };
    int polled = poll(&ready, 1, timeout_ms);
    if (polled < 0) {
        return errno == EINTR ? ERR_OK : err_set(err, ERR_PLATFORM, "inotify: %s", strerror(errno));
    }
    alignas(struct inotify_event) char buffer[16 * 1024];
    Error e = ERR_OK;
    for (;;) {
        ssize_t got = read(backend->fd, buffer, sizeof buffer);
        if (got <= 0) {
            bool drained = got == 0 || errno == EAGAIN || errno == EINTR;
            finish_move_away(backend);
            return drained ? e : err_set(err, ERR_PLATFORM, "inotify: %s", strerror(errno));
        }
        for (char *at = buffer; at < buffer + got;) {
            const struct inotify_event *event = (const struct inotify_event *)at;
            at += sizeof *event + event->len;
            Error handled = handle(backend, arena, event, events, err);
            e = e == ERR_OK ? handled : e;
        }
    }
}

#endif
