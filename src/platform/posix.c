// platform.h for the POSIX systems: macOS and Linux.
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "mc/platform/platform.h"

#include "mc/text/path.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/attr.h>
#include <xlocale.h>
// Cross builds use zig's macOS headers, which leave out sys/vnode.h; its
// object types are a fixed part of the getattrlistbulk ABI.
#if __has_include(<sys/vnode.h>)
#include <sys/vnode.h>
#else
enum { VREG = 1, VDIR = 2, VLNK = 5 };
#endif
#endif

extern char **environ;

static_assert(sizeof(pthread_mutex_t) <= sizeof(Mutex), "Mutex storage is too small");
static_assert(sizeof(pthread_cond_t) <= sizeof(Cond), "Cond storage is too small");
static_assert(sizeof(pthread_t) <= sizeof(Thread), "Thread storage is too small");

enum { PATH_LIMIT = 4096 };

// CPath is a String path as the NUL-terminated text system calls take.
typedef struct {
    char text[PATH_LIMIT];
} CPath;

[[nodiscard]] static Error to_cpath(String path, CPath *out, Err *err)
{
    if (path.len >= sizeof out->text || (path.len > 0 && memchr(path.data, '\0', path.len) != nullptr)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "unusable path: %.*s", (int)min_size(path.len, 200), path.data);
    }
    if (path.len > 0) {
        memcpy(out->text, path.data, path.len);
    }
    out->text[path.len] = '\0';
    return ERR_OK;
}

[[nodiscard]] static Error error_from_errno(int code)
{
    return code == ENOENT || code == ENOTDIR ? ERR_NOT_FOUND : ERR_IO;
}

// fail_errno records the message followed by the description of errno, and maps errno to an Error.
[[nodiscard]] [[gnu::format(printf, 2, 3)]] static Error fail_errno(Err *err, const char *format, ...)
{
    int saved = errno;
    Error code = error_from_errno(saved);
    if (err == nullptr) {
        return code;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(err->msg, sizeof err->msg, format, args);
    va_end(args);
    size_t at = written < 0 ? 0 : min_size((size_t)written, sizeof err->msg);
    if (at < sizeof err->msg) {
        snprintf(err->msg + at, sizeof err->msg - at, ": %s", strerror(saved));
    }
    return code;
}

// ---- clock ----

int64_t clock_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * NS_PER_SECOND + now.tv_nsec;
}

int64_t clock_wall_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (int64_t)now.tv_sec * NS_PER_SECOND + now.tv_nsec;
}

void clock_sleep_ms(int64_t milliseconds)
{
    if (milliseconds <= 0) {
        return;
    }
    struct timespec remaining = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

int32_t clock_local_offset(int64_t unix_seconds)
{
    time_t seconds = (time_t)unix_seconds;
    struct tm local;
    if (localtime_r(&seconds, &local) == nullptr) {
        return 0;
    }
    return (int32_t)local.tm_gmtoff;
}

// ---- numbers ----

enum { FLOAT_TEXT_LIMIT = 64, FLOAT_MAX_DIGITS = 17 };

// One C locale serves the whole process; creating it per call would allocate.
static locale_t c_locale_value; // modern-c: allow global-mutable
static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT; // modern-c: allow global-mutable

static void create_c_locale(void)
{
    c_locale_value = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    if (c_locale_value == (locale_t)0) {
        abort();
    }
}

// c_locale is used through uselocale, since glibc has no snprintf_l.
static locale_t c_locale(void)
{
    pthread_once(&c_locale_once, create_c_locale);
    return c_locale_value;
}

static bool parse_terminated(const char *text, size_t len, double *value)
{
    locale_t previous = uselocale(c_locale());
    errno = 0;
    char *end;
    double parsed = strtod(text, &end);
    bool overflow = errno == ERANGE && (parsed == HUGE_VAL || parsed == -HUGE_VAL);
    uselocale(previous);
    if (end != text + len || overflow) {
        return false;
    }
    *value = parsed;
    return true;
}

bool float_parse(String text, double *value)
{
    if (text.len == 0 || isspace((unsigned char)text.data[0])) {
        return false;
    }
    if (text.len >= FLOAT_TEXT_LIMIT) {
        Arena *scratch = arena_create(0);
        bool parsed = parse_terminated(str_cstr(scratch, text), text.len, value);
        arena_destroy(scratch);
        return parsed;
    }
    char buffer[FLOAT_TEXT_LIMIT];
    memcpy(buffer, text.data, text.len);
    buffer[text.len] = '\0';
    return parse_terminated(buffer, text.len, value);
}

String float_format(Arena *arena, double value)
{
    char buffer[FLOAT_TEXT_LIMIT];
    locale_t previous = uselocale(c_locale());
    for (int digits = 1; digits <= FLOAT_MAX_DIGITS; digits++) {
        snprintf(buffer, sizeof buffer, "%.*g", digits, value);
        if (strtod(buffer, nullptr) == value) {
            break;
        }
    }
    uselocale(previous);
    return str_copy(arena, S(buffer));
}

// ---- threads ----

static pthread_mutex_t *as_mutex(Mutex *mutex)
{
    return (pthread_mutex_t *)mutex->storage;
}

static pthread_cond_t *as_cond(Cond *cond)
{
    return (pthread_cond_t *)cond->storage;
}

static pthread_t *as_thread(Thread *thread)
{
    return (pthread_t *)thread->storage;
}

// must_succeed aborts on a failed pthread call: the primitives below cannot
// report errors, and a failure means the system ran out of resources or the
// primitive was misused.
static void must_succeed(int rc, const char *operation)
{
    if (rc != 0) {
        fprintf(stderr, "%s: %s\n", operation, strerror(rc));
        abort();
    }
}

void mutex_init(Mutex *mutex)
{
    must_succeed(pthread_mutex_init(as_mutex(mutex), nullptr), "pthread_mutex_init");
}

void mutex_destroy(Mutex *mutex)
{
    must_succeed(pthread_mutex_destroy(as_mutex(mutex)), "pthread_mutex_destroy");
}

void mutex_lock(Mutex *mutex)
{
    must_succeed(pthread_mutex_lock(as_mutex(mutex)), "pthread_mutex_lock");
}

void mutex_unlock(Mutex *mutex)
{
    must_succeed(pthread_mutex_unlock(as_mutex(mutex)), "pthread_mutex_unlock");
}

void cond_init(Cond *cond)
{
    pthread_condattr_t attributes;
    must_succeed(pthread_condattr_init(&attributes), "pthread_condattr_init");
#ifndef __APPLE__
    must_succeed(pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC), "pthread_condattr_setclock");
#endif
    must_succeed(pthread_cond_init(as_cond(cond), &attributes), "pthread_cond_init");
    must_succeed(pthread_condattr_destroy(&attributes), "pthread_condattr_destroy");
}

void cond_destroy(Cond *cond)
{
    must_succeed(pthread_cond_destroy(as_cond(cond)), "pthread_cond_destroy");
}

void cond_signal(Cond *cond)
{
    must_succeed(pthread_cond_signal(as_cond(cond)), "pthread_cond_signal");
}

void cond_broadcast(Cond *cond)
{
    must_succeed(pthread_cond_broadcast(as_cond(cond)), "pthread_cond_broadcast");
}

void cond_wait(Cond *cond, Mutex *mutex)
{
    must_succeed(pthread_cond_wait(as_cond(cond), as_mutex(mutex)), "pthread_cond_wait");
}

void cond_wait_until(Cond *cond, Mutex *mutex, int64_t deadline_ns)
{
    int64_t remaining = deadline_ns - clock_monotonic_ns();
    if (remaining <= 0) {
        return;
    }
#ifdef __APPLE__
    // macOS condition variables cannot use the monotonic clock, but take a relative timeout.
    struct timespec relative = {
        .tv_sec = (time_t)(remaining / NS_PER_SECOND),
        .tv_nsec = (long)(remaining % NS_PER_SECOND),
    };
    int rc = pthread_cond_timedwait_relative_np(as_cond(cond), as_mutex(mutex), &relative);
#else
    struct timespec absolute = {
        .tv_sec = (time_t)(deadline_ns / NS_PER_SECOND),
        .tv_nsec = (long)(deadline_ns % NS_PER_SECOND),
    };
    int rc = pthread_cond_timedwait(as_cond(cond), as_mutex(mutex), &absolute);
#endif
    must_succeed(rc == ETIMEDOUT ? 0 : rc, "pthread_cond_timedwait");
}

Error thread_start(Thread *thread, void *(*run)(void *argument), void *argument, Err *err)
{
    int rc = pthread_create(as_thread(thread), nullptr, run, argument);
    if (rc != 0) {
        return err_set(err, ERR_PLATFORM, "start thread: %s", strerror(rc));
    }
    return ERR_OK;
}

void thread_join(Thread *thread)
{
    must_succeed(pthread_join(*as_thread(thread), nullptr), "pthread_join");
}

void thread_detach(Thread *thread)
{
    must_succeed(pthread_detach(*as_thread(thread)), "pthread_detach");
}

size_t thread_cpu_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (size_t)count : 1;
}

// ---- files ----

static int64_t timespec_ns(struct timespec time)
{
    return (int64_t)time.tv_sec * NS_PER_SECOND + time.tv_nsec;
}

static int64_t mtime_ns(const struct stat *st)
{
#ifdef __APPLE__
    return timespec_ns(st->st_mtimespec);
#else
    return timespec_ns(st->st_mtim);
#endif
}

static int64_t birth_ns(const struct stat *st)
{
#ifdef __APPLE__
    return timespec_ns(st->st_birthtimespec);
#else
    unused(st);
    return 0;
#endif
}

static FileInfo describe(const struct stat *st)
{
    return (FileInfo){
        .exists = true,
        .is_dir = S_ISDIR(st->st_mode),
        .is_regular = S_ISREG(st->st_mode),
        .is_symlink = S_ISLNK(st->st_mode),
        .size = (int64_t)st->st_size,
        .mtime_ns = mtime_ns(st),
        .birth_ns = birth_ns(st),
        .mode = (unsigned)(st->st_mode & 07777),
    };
}

[[nodiscard]] static Error stat_path(String path, bool follow, FileInfo *info, Err *err)
{
    *info = (FileInfo){ 0 };
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    struct stat st;
    int rc = follow ? stat(cpath.text, &st) : lstat(cpath.text, &st);
    if (rc == 0) {
        *info = describe(&st);
        return ERR_OK;
    }
    if (errno == ENOENT || errno == ENOTDIR) {
        return ERR_OK;
    }
    return fail_errno(err, "stat %s", cpath.text);
}

Error file_info(String path, FileInfo *info, Err *err)
{
    return stat_path(path, false, info, err);
}

Error file_info_follow(String path, FileInfo *info, Err *err)
{
    return stat_path(path, true, info, err);
}

Error file_info_fd(int fd, FileInfo *info, Err *err)
{
    *info = (FileInfo){ 0 };
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return fail_errno(err, "stat descriptor %d", fd);
    }
    *info = describe(&st);
    return ERR_OK;
}

bool file_exists(String path)
{
    FileInfo info;
    return file_info_follow(path, &info, nullptr) == ERR_OK && info.exists;
}

Error file_open_read(String path, int *fd, Err *err)
{
    *fd = -1;
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    *fd = open(cpath.text, O_RDONLY | O_CLOEXEC);
    return *fd >= 0 ? ERR_OK : fail_errno(err, "open %s", cpath.text);
}

Error file_open_append(String path, int *fd, Err *err)
{
    *fd = -1;
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    *fd = open(cpath.text, O_RDWR | O_APPEND | O_CLOEXEC);
    return *fd >= 0 ? ERR_OK : fail_errno(err, "open %s", cpath.text);
}

Error file_create(String path, unsigned mode, int *fd, Err *err)
{
    *fd = -1;
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    *fd = open(cpath.text, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)mode);
    return *fd >= 0 ? ERR_OK : fail_errno(err, "create %s", cpath.text);
}

Error file_create_unique(Arena *arena, String pattern, String *path, int *fd, Err *err)
{
    *fd = -1;
    CPath cpath;
    Error e = to_cpath(pattern, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    *fd = mkostemp(cpath.text, O_CLOEXEC);
    if (*fd < 0) {
        return fail_errno(err, "create %s", cpath.text);
    }
    *path = str_copy(arena, S(cpath.text));
    return ERR_OK;
}

void file_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

Error file_read(int fd, void *buffer, size_t capacity, size_t *got, Err *err)
{
    for (;;) {
        ssize_t n = read(fd, buffer, capacity);
        if (n >= 0) {
            *got = (size_t)n;
            return ERR_OK;
        }
        if (errno != EINTR) {
            *got = 0;
            return fail_errno(err, "read");
        }
    }
}

Error file_pread(int fd, void *buffer, size_t capacity, int64_t offset, size_t *got, Err *err)
{
    for (;;) {
        ssize_t n = pread(fd, buffer, capacity, (off_t)offset);
        if (n >= 0) {
            *got = (size_t)n;
            return ERR_OK;
        }
        if (errno != EINTR) {
            *got = 0;
            return fail_errno(err, "read");
        }
    }
}

Error file_write(int fd, String data, Err *err)
{
    size_t written = 0;
    while (written < data.len) {
        ssize_t n = write(fd, data.data + written, data.len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail_errno(err, "write");
        }
        written += (size_t)n;
    }
    return ERR_OK;
}

Error file_seek(int fd, int64_t offset, Err *err)
{
    return lseek(fd, (off_t)offset, SEEK_SET) < 0 ? fail_errno(err, "seek") : ERR_OK;
}

Error file_truncate(int fd, int64_t size, Err *err)
{
    return ftruncate(fd, (off_t)size) != 0 ? fail_errno(err, "truncate") : ERR_OK;
}

Error file_rename(String from, String to, Err *err)
{
    CPath cfrom;
    CPath cto;
    Error e = to_cpath(from, &cfrom, err);
    if (e == ERR_OK) {
        e = to_cpath(to, &cto, err);
    }
    if (e != ERR_OK) {
        return e;
    }
    return rename(cfrom.text, cto.text) == 0 ? ERR_OK : fail_errno(err, "rename %s to %s", cfrom.text, cto.text);
}

Error file_remove(String path, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    return unlink(cpath.text) == 0 ? ERR_OK : fail_errno(err, "remove %s", cpath.text);
}

Error file_set_mtime(String path, int64_t mtime_ns, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    struct timespec times[2];
    times[0].tv_sec = times[1].tv_sec = (time_t)(mtime_ns / NS_PER_SECOND);
    times[0].tv_nsec = times[1].tv_nsec = (long)(mtime_ns % NS_PER_SECOND);
    return utimensat(AT_FDCWD, cpath.text, times, 0) == 0 ? ERR_OK : fail_errno(err, "set mtime %s", cpath.text);
}

Error file_read_all(Arena *arena, String path, String *contents, Err *err)
{
    *contents = S("");
    int fd;
    Error e = file_open_read(path, &fd, err);
    if (e != ERR_OK) {
        return e;
    }
    FileInfo info;
    e = file_info_fd(fd, &info, err);
    size_t capacity = e == ERR_OK && info.size > 0 ? (size_t)info.size + 1 : 4096;
    char *data = arena_push_aligned(arena, capacity, 1);
    size_t len = 0;
    while (e == ERR_OK) {
        size_t got;
        if (len + 1 == capacity) {
            // A full buffer usually means the whole file is in: probe before growing.
            char probe;
            e = file_read(fd, &probe, 1, &got, err);
            if (e != ERR_OK || got == 0) {
                break;
            }
            size_t grown = arena_size_mul(capacity, 2);
            char *fresh = arena_push_aligned(arena, grown, 1);
            memcpy(fresh, data, len);
            data = fresh;
            capacity = grown;
            data[len++] = probe;
        }
        e = file_read(fd, data + len, capacity - len - 1, &got, err);
        if (e != ERR_OK || got == 0) {
            break;
        }
        len += got;
    }
    file_close(fd);
    if (e != ERR_OK) {
        return err_wrap(err, e, "read %.*s", (int)path.len, path.data);
    }
    data[len] = '\0';
    *contents = (String){ .data = data, .len = len };
    return ERR_OK;
}

Error file_write_all(String path, String contents, unsigned mode, Err *err)
{
    int fd;
    Error e = file_create(path, mode, &fd, err);
    if (e != ERR_OK) {
        return e;
    }
    e = file_write(fd, contents, err);
    file_close(fd);
    return e == ERR_OK ? ERR_OK : err_wrap(err, e, "write %.*s", (int)path.len, path.data);
}

Error file_write_atomic(String path, String contents, unsigned mode, Err *err)
{
    Error e = dir_create_all(path_dir(path), 0755, err);
    if (e != ERR_OK) {
        return e;
    }
    CPath temp;
    e = to_cpath(path, &temp, err);
    if (e != ERR_OK) {
        return e;
    }
    static const char suffix[] = ".tmp.XXXXXX";
    if (path.len + sizeof suffix > sizeof temp.text) {
        return err_set(err, ERR_INVALID_ARGUMENT, "path too long: %s", temp.text);
    }
    memcpy(temp.text + path.len, suffix, sizeof suffix);
    int fd = mkstemp(temp.text);
    if (fd < 0) {
        return fail_errno(err, "create %s", temp.text);
    }
    String temp_path = S(temp.text);
    if (fchmod(fd, (mode_t)mode) != 0) {
        e = fail_errno(err, "chmod %s", temp.text);
    }
    if (e == ERR_OK) {
        e = file_write(fd, contents, err);
    }
    file_close(fd);
    if (e == ERR_OK) {
        e = file_rename(temp_path, path, err);
    }
    if (e != ERR_OK) {
        unlink(temp.text);
    }
    return e;
}

Error file_map(String path, FileMap *map, Err *err)
{
    *map = (FileMap){ 0 };
    int fd;
    Error e = file_open_read(path, &fd, err);
    if (e != ERR_OK) {
        return e;
    }
    struct stat st;
    void *data = nullptr;
    if (fstat(fd, &st) != 0) {
        e = fail_errno(err, "stat %.*s", (int)path.len, path.data);
    } else if (st.st_size > 0) {
        data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            e = fail_errno(err, "map %.*s", (int)path.len, path.data);
        }
    }
    close(fd);
    if (e == ERR_OK && data != nullptr) {
        *map = (FileMap){ data, (size_t)st.st_size };
    }
    return e;
}

void file_unmap(FileMap map)
{
    if (map.data != nullptr) {
        munmap((void *)map.data, map.len);
    }
}

// ---- directories ----

// make_directory succeeds when path ends up a directory, following symlinks.
// Something else already at path fails with errno EEXIST.
static bool make_directory(const char *path, unsigned mode)
{
    if (mkdir(path, (mode_t)mode) == 0) {
        return true;
    }
    if (errno != EEXIST) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    errno = EEXIST;
    return false;
}

Error dir_create(String path, unsigned mode, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    return make_directory(cpath.text, mode) ? ERR_OK : fail_errno(err, "create directory %s", cpath.text);
}

Error dir_create_all(String path, unsigned mode, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    for (size_t i = 1; i <= path.len; i++) {
        if (i < path.len && cpath.text[i] != '/') {
            continue;
        }
        char saved = cpath.text[i];
        cpath.text[i] = '\0';
        if (!make_directory(cpath.text, mode)) {
            return fail_errno(err, "create directory %s", cpath.text);
        }
        cpath.text[i] = saved;
    }
    return ERR_OK;
}

Error dir_remove(String path, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    return rmdir(cpath.text) == 0 ? ERR_OK : fail_errno(err, "remove directory %s", cpath.text);
}

static bool is_dot_or_dot_dot(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

// remove_tree removes the path in path->text[0..len), appending child names to the same buffer.
[[nodiscard]] static Error remove_tree(CPath *path, size_t len, Err *err)
{
    struct stat st;
    if (lstat(path->text, &st) != 0) {
        return errno == ENOENT ? ERR_OK : fail_errno(err, "stat %s", path->text);
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path->text) == 0 ? ERR_OK : fail_errno(err, "remove %s", path->text);
    }
    DIR *dir = opendir(path->text);
    if (dir == nullptr) {
        return fail_errno(err, "open directory %s", path->text);
    }
    Error e = ERR_OK;
    struct dirent *entry;
    while (e == ERR_OK && (entry = readdir(dir)) != nullptr) {
        if (is_dot_or_dot_dot(entry->d_name)) {
            continue;
        }
        size_t name_len = strlen(entry->d_name);
        if (len + 1 + name_len >= sizeof path->text) {
            e = err_set(err, ERR_INVALID_ARGUMENT, "path too long below %s", path->text);
            break;
        }
        path->text[len] = '/';
        memcpy(path->text + len + 1, entry->d_name, name_len + 1);
        e = remove_tree(path, len + 1 + name_len, err);
        path->text[len] = '\0';
    }
    closedir(dir);
    if (e != ERR_OK) {
        return e;
    }
    return rmdir(path->text) == 0 ? ERR_OK : fail_errno(err, "remove directory %s", path->text);
}

Error dir_remove_all(String path, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    return remove_tree(&cpath, path.len, err);
}

Error dir_list(Arena *arena, String path, StringList *names, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    DIR *dir = opendir(cpath.text);
    if (dir == nullptr) {
        return fail_errno(err, "open directory %s", cpath.text);
    }
    StringList found = { 0 };
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!is_dot_or_dot_dot(entry->d_name)) {
            strlist_push(arena, &found, str_copy(arena, S(entry->d_name)));
        }
    }
    closedir(dir);
    strlist_sort(&found);
    strlist_push_all(arena, names, found);
    return ERR_OK;
}

static void append_entry(Arena *arena, DirEntryList *entries, const char *name, FileInfo info)
{
    entries->items = arena_grow(arena, entries->items, &entries->capacity, entries->count, sizeof *entries->items);
    entries->items[entries->count++] = (DirEntry){ str_copy(arena, S(name)), info };
}

[[nodiscard]] static Error read_dir_portable(Arena *arena, int dir_fd, const char *cpath, DirEntryList *entries,
                                             Err *err)
{
    int fd = dup(dir_fd);
    DIR *dir = fd >= 0 ? fdopendir(fd) : nullptr;
    if (dir == nullptr) {
        Error e = fail_errno(err, "open directory %s", cpath);
        if (fd >= 0) {
            close(fd);
        }
        return e;
    }
    Error e = ERR_OK;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == nullptr) {
            if (errno != 0) {
                e = fail_errno(err, "read directory %s", cpath);
            }
            break;
        }
        struct stat st;
        if (!is_dot_or_dot_dot(entry->d_name) && fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
            append_entry(arena, entries, entry->d_name, describe(&st));
        }
    }
    closedir(dir);
    return e;
}

#ifdef __APPLE__
// read_dir_bulk packs the attributes of many entries into one buffer per
// call. They come in the order of their bits, except the error, which follows
// the returned-attribute set, and one the file system does not return is
// absent rather than zero. *unsupported asks for read_dir_portable instead.
[[nodiscard]] static Error read_dir_bulk(Arena *arena, int dir_fd, const char *cpath, DirEntryList *entries,
                                         bool *unsupported, Err *err)
{
    struct attrlist request = {
        .bitmapcount = ATTR_BIT_MAP_COUNT,
        .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE | ATTR_CMN_CRTIME |
                      ATTR_CMN_MODTIME | ATTR_CMN_ACCESSMASK,
        .fileattr = ATTR_FILE_DATALENGTH,
    };
    alignas(8) char buffer[64 * 1024];
    *unsupported = false;
    for (bool first = true;; first = false) {
        int count = getattrlistbulk(dir_fd, &request, buffer, sizeof buffer, 0);
        if (count < 0) {
            *unsupported = first && (errno == ENOTSUP || errno == EINVAL);
            return *unsupported ? ERR_OK : fail_errno(err, "read directory %s", cpath);
        }
        if (count == 0) {
            return ERR_OK;
        }
        const char *next = buffer;
        for (int i = 0; i < count; i++) {
            const char *p = next;
            uint32_t length;
            memcpy(&length, p, sizeof length);
            next += length;
            p += sizeof length;
            attribute_set_t returned;
            memcpy(&returned, p, sizeof returned);
            p += sizeof returned;
            if (returned.commonattr & ATTR_CMN_ERROR) {
                uint32_t error;
                memcpy(&error, p, sizeof error);
                p += sizeof error;
                if (error != 0) {
                    continue;
                }
            }
            if (!(returned.commonattr & ATTR_CMN_NAME)) {
                continue;
            }
            attrreference_t name;
            memcpy(&name, p, sizeof name);
            const char *name_text = p + name.attr_dataoffset;
            p += sizeof name;
            FileInfo info = { .exists = true };
            if (returned.commonattr & ATTR_CMN_OBJTYPE) {
                fsobj_type_t type;
                memcpy(&type, p, sizeof type);
                p += sizeof type;
                info.is_dir = type == VDIR;
                info.is_regular = type == VREG;
                info.is_symlink = type == VLNK;
            }
            if (returned.commonattr & ATTR_CMN_CRTIME) {
                struct timespec created;
                memcpy(&created, p, sizeof created);
                p += sizeof created;
                info.birth_ns = timespec_ns(created);
            }
            if (returned.commonattr & ATTR_CMN_MODTIME) {
                struct timespec modified;
                memcpy(&modified, p, sizeof modified);
                p += sizeof modified;
                info.mtime_ns = timespec_ns(modified);
            }
            if (returned.commonattr & ATTR_CMN_ACCESSMASK) {
                uint32_t mask;
                memcpy(&mask, p, sizeof mask);
                p += sizeof mask;
                info.mode = mask & 07777;
            }
            if (returned.fileattr & ATTR_FILE_DATALENGTH) {
                off_t size;
                memcpy(&size, p, sizeof size);
                info.size = (int64_t)size;
            }
            append_entry(arena, entries, name_text, info);
        }
    }
}
#endif

Error dir_read(Arena *arena, String path, DirEntryList *entries, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    int fd = open(cpath.text, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return fail_errno(err, "open directory %s", cpath.text);
    }
#ifdef __APPLE__
    bool unsupported;
    e = read_dir_bulk(arena, fd, cpath.text, entries, &unsupported, err);
    if (e == ERR_OK && unsupported) {
        e = read_dir_portable(arena, fd, cpath.text, entries, err);
    }
#else
    e = read_dir_portable(arena, fd, cpath.text, entries, err);
#endif
    close(fd);
    return e;
}

Error dir_create_temp(Arena *arena, String prefix, String *path, Err *err)
{
    const char *base = getenv("TMPDIR");
    if (base == nullptr || *base == '\0') {
        base = "/tmp";
    }
    String pattern = str_format(arena, "%.*s/%.*sXXXXXX", (int)str_trim_suffix(S(base), S("/")).len, base,
                                (int)prefix.len, prefix.data);
    char *text = (char *)pattern.data;
    if (mkdtemp(text) == nullptr) {
        return fail_errno(err, "create directory %s", text);
    }
    *path = pattern;
    return ERR_OK;
}

// ---- paths the file system decides ----

Error path_resolve(Arena *arena, String path, String *resolved, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
    char buffer[PATH_LIMIT];
    if (realpath(cpath.text, buffer) == nullptr) {
        return fail_errno(err, "resolve %s", cpath.text);
    }
    *resolved = str_copy(arena, S(buffer));
    return ERR_OK;
}

String path_absolute(Arena *arena, String path)
{
    if (path_is_absolute(path)) {
        return path_clean(arena, path);
    }
    char cwd[PATH_LIMIT];
    if (getcwd(cwd, sizeof cwd) == nullptr) {
        return path_clean(arena, path);
    }
    return path_clean(arena, path_join(arena, S(cwd), path));
}

bool path_is_executable(String path)
{
    CPath cpath;
    struct stat st;
    return to_cpath(path, &cpath, nullptr) == ERR_OK && stat(cpath.text, &st) == 0 && S_ISREG(st.st_mode) &&
           access(cpath.text, X_OK) == 0;
}

bool paths_are_same_file(String a, String b)
{
    CPath ca;
    CPath cb;
    struct stat sa;
    struct stat sb;
    return to_cpath(a, &ca, nullptr) == ERR_OK && to_cpath(b, &cb, nullptr) == ERR_OK && stat(ca.text, &sa) == 0 &&
           stat(cb.text, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// ---- processes ----

static char **build_argv(Arena *arena, StringList argv)
{
    char **items = arena_push(arena, arena_size_mul(arena_size_add(argv.count, 1), sizeof *items));
    for (size_t i = 0; i < argv.count; i++) {
        items[i] = (char *)str_cstr(arena, argv.items[i]);
    }
    return items;
}

[[nodiscard]] static Error spawn_failed(Err *err, int rc, const char *program)
{
    return err_set(err, rc == ENOENT ? ERR_NOT_FOUND : ERR_PLATFORM, "run %s: %s", program, strerror(rc));
}

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} Capture;

static void capture_append(Arena *arena, Capture *capture, const char *bytes, size_t n)
{
    size_t needed = arena_size_add(arena_size_add(capture->len, n), 1);
    if (needed > capture->capacity) {
        size_t capacity = max_size(arena_size_mul(capture->capacity, 2), max_size(needed, 4096));
        char *fresh = arena_push_aligned(arena, capacity, 1);
        if (capture->len > 0) {
            memcpy(fresh, capture->data, capture->len);
        }
        capture->data = fresh;
        capture->capacity = capacity;
    }
    memcpy(capture->data + capture->len, bytes, n);
    capture->len += n;
    capture->data[capture->len] = '\0';
}

static String captured(Capture capture)
{
    return capture.data != nullptr ? (String){ .data = capture.data, .len = capture.len } : S("");
}

static void close_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_pipes(int *stdin_fd, int *stdout_fd, int *stderr_fd)
{
    close_if_open(stdin_fd);
    close_if_open(stdout_fd);
    close_if_open(stderr_fd);
}

static bool would_block(void)
{
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

// pump writes stdin_text to the child and reads its output until both output pipes close.
// The descriptors are nonblocking, so a full pipe in one direction never stops the others.
[[nodiscard]] static Error pump(Arena *arena, int stdin_fd, String stdin_text, int stdout_fd, int stderr_fd,
                                Capture *out, Capture *errors, Err *err)
{
    size_t written = 0;
    if (stdin_text.len == 0) {
        close_if_open(&stdin_fd);
    }
    while (stdin_fd >= 0 || stdout_fd >= 0 || stderr_fd >= 0) {
        struct pollfd fds[3];
        nfds_t count = 0;
        if (stdin_fd >= 0) {
            fds[count++] = (struct pollfd){ .fd = stdin_fd, .events = POLLOUT };
        }
        if (stdout_fd >= 0) {
            fds[count++] = (struct pollfd){ .fd = stdout_fd, .events = POLLIN };
        }
        if (stderr_fd >= 0) {
            fds[count++] = (struct pollfd){ .fd = stderr_fd, .events = POLLIN };
        }
        if (poll(fds, count, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            Error e = fail_errno(err, "poll");
            close_pipes(&stdin_fd, &stdout_fd, &stderr_fd);
            return e;
        }
        for (nfds_t i = 0; i < count; i++) {
            if (fds[i].revents == 0) {
                continue;
            }
            if (fds[i].fd == stdin_fd) {
                ssize_t n = write(stdin_fd, stdin_text.data + written, stdin_text.len - written);
                if (n > 0) {
                    written += (size_t)n;
                } else if (n < 0 && errno == EPIPE) {
                    // The child exited or closed its input without reading all of it.
                    close_if_open(&stdin_fd);
                } else if (n < 0 && !would_block()) {
                    Error e = fail_errno(err, "write to process");
                    close_pipes(&stdin_fd, &stdout_fd, &stderr_fd);
                    return e;
                }
                if (written == stdin_text.len) {
                    close_if_open(&stdin_fd);
                }
                continue;
            }
            bool is_stdout = fds[i].fd == stdout_fd;
            char buffer[8192];
            ssize_t n = read(fds[i].fd, buffer, sizeof buffer);
            if (n > 0) {
                capture_append(arena, is_stdout ? out : errors, buffer, (size_t)n);
            } else if (n == 0) {
                close_if_open(is_stdout ? &stdout_fd : &stderr_fd);
            } else if (!would_block()) {
                Error e = fail_errno(err, "read from process");
                close_pipes(&stdin_fd, &stdout_fd, &stderr_fd);
                return e;
            }
        }
    }
    return ERR_OK;
}

static void record_status(int status, int *exit_code, int *signal_number)
{
    *exit_code = -1;
    *signal_number = 0;
    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *signal_number = WTERMSIG(status);
    }
}

[[nodiscard]] static Error wait_for(pid_t pid, int *status, Err *err)
{
    while (waitpid(pid, status, 0) < 0) {
        if (errno != EINTR) {
            return fail_errno(err, "wait for process %d", (int)pid);
        }
    }
    return ERR_OK;
}

static void close_pipe_pairs(int in_pipe[2], int out_pipe[2], int err_pipe[2])
{
    for (size_t i = 0; i < 2; i++) {
        close_if_open(&in_pipe[i]);
        close_if_open(&out_pipe[i]);
        close_if_open(&err_pipe[i]);
    }
}

// open_pipe keeps both ends out of every child; spawning dup2s the child's end to a standard stream.
static bool open_pipe(int fds[2])
{
#ifdef __linux__
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    // Without pipe2, a spawn on another thread between these calls can still inherit the ends.
    return pipe(fds) == 0 && fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0;
#endif
}

static bool make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// disable_sigpipe stops writes to fd raising SIGPIPE where the system allows it.
// macOS may deliver a write's SIGPIPE to any thread, so blocking it in the
// writing thread, as pump_without_sigpipe does, is not enough there.
static bool disable_sigpipe(int fd)
{
#ifdef F_SETNOSIGPIPE
    return fcntl(fd, F_SETNOSIGPIPE, 1) == 0;
#else
    unused(fd);
    return true;
#endif
}

[[nodiscard]] static int redirect_child(posix_spawn_file_actions_t *actions, int stdin_fd, int stdout_fd,
                                        int stderr_fd)
{
    int rc = posix_spawn_file_actions_adddup2(actions, stdin_fd, STDIN_FILENO);
    if (rc == 0) {
        rc = posix_spawn_file_actions_adddup2(actions, stdout_fd, STDOUT_FILENO);
    }
    if (rc == 0) {
        rc = posix_spawn_file_actions_adddup2(actions, stderr_fd, STDERR_FILENO);
    }
    return rc;
}

// pump_without_sigpipe blocks SIGPIPE in this thread while pumping, so a child
// that exits without reading its input cannot end this process, and discards
// the SIGPIPE a failed write left pending. The rest of the process keeps its
// own SIGPIPE handling.
[[nodiscard]] static Error pump_without_sigpipe(Arena *arena, int stdin_fd, String stdin_text, int stdout_fd,
                                                int stderr_fd, Capture *out, Capture *errors, Err *err)
{
    sigset_t sigpipe_only;
    sigemptyset(&sigpipe_only);
    sigaddset(&sigpipe_only, SIGPIPE);
    sigset_t previous;
    pthread_sigmask(SIG_BLOCK, &sigpipe_only, &previous);
    sigset_t pending;
    sigpending(&pending);
    bool was_pending = sigismember(&pending, SIGPIPE);
    Error e = pump(arena, stdin_fd, stdin_text, stdout_fd, stderr_fd, out, errors, err);
    sigpending(&pending);
    if (!was_pending && sigismember(&pending, SIGPIPE)) {
        int number;
        sigwait(&sigpipe_only, &number);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    return e;
}

Error process_run(Arena *arena, StringList argv, String stdin_text, ProcessResult *result, Err *err)
{
    *result = (ProcessResult){ .stdout_text = S(""), .stderr_text = S("") };
    if (argv.count == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "run: no program");
    }
    int in_pipe[2] = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (!open_pipe(in_pipe) || !open_pipe(out_pipe) || !open_pipe(err_pipe)) {
        Error e = fail_errno(err, "pipe");
        close_pipe_pairs(in_pipe, out_pipe, err_pipe);
        return e;
    }
    if (!make_nonblocking(in_pipe[1]) || !make_nonblocking(out_pipe[0]) || !make_nonblocking(err_pipe[0]) ||
        !disable_sigpipe(in_pipe[1])) {
        Error e = fail_errno(err, "configure pipe");
        close_pipe_pairs(in_pipe, out_pipe, err_pipe);
        return e;
    }

    char **items = build_argv(arena, argv);
    pid_t pid;
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc == 0) {
        rc = redirect_child(&actions, in_pipe[0], out_pipe[1], err_pipe[1]);
        if (rc == 0) {
            rc = posix_spawnp(&pid, items[0], &actions, nullptr, items, environ);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    close_if_open(&in_pipe[0]);
    close_if_open(&out_pipe[1]);
    close_if_open(&err_pipe[1]);
    if (rc != 0) {
        close_if_open(&in_pipe[1]);
        close_if_open(&out_pipe[0]);
        close_if_open(&err_pipe[0]);
        return spawn_failed(err, rc, items[0]);
    }

    Capture out = { 0 };
    Capture errors = { 0 };
    Error pumped = pump_without_sigpipe(arena, in_pipe[1], stdin_text, out_pipe[0], err_pipe[0], &out, &errors, err);
    int status = 0;
    Error waited = wait_for(pid, &status, err);
    if (pumped != ERR_OK) {
        return pumped;
    }
    if (waited != ERR_OK) {
        return waited;
    }
    result->stdout_text = captured(out);
    result->stderr_text = captured(errors);
    record_status(status, &result->exit_code, &result->signal);
    return ERR_OK;
}

Error process_run_interactive(Arena *scratch, StringList argv, int *exit_code, Err *err)
{
    if (argv.count == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "run: no program");
    }
    ArenaMark mark = arena_mark(scratch);
    char **items = build_argv(scratch, argv);
    pid_t pid;
    int rc = posix_spawnp(&pid, items[0], nullptr, nullptr, items, environ);
    Error e = rc != 0 ? spawn_failed(err, rc, items[0]) : ERR_OK;
    arena_release(mark);
    if (e != ERR_OK) {
        return e;
    }
    int status = 0;
    e = wait_for(pid, &status, err);
    if (e != ERR_OK) {
        return e;
    }
    int signal_number;
    record_status(status, exit_code, &signal_number);
    if (signal_number != 0) {
        *exit_code = 128 + signal_number;
    }
    return ERR_OK;
}

bool process_look_path(Arena *arena, String name, String *path)
{
    size_t slash;
    if (str_find_char(name, '/', &slash)) {
        if (!path_is_executable(name)) {
            return false;
        }
        *path = str_copy(arena, name);
        return true;
    }
    const char *list = getenv("PATH");
    if (list == nullptr || name.len == 0) {
        return false;
    }
    String rest = S(list);
    bool more = true;
    while (more) {
        String dir;
        more = str_cut(rest, ':', &dir, &rest);
        ArenaMark mark = arena_mark(arena);
        String candidate = path_join(arena, dir.len > 0 ? dir : S("."), name);
        if (path_is_executable(candidate)) {
            *path = candidate;
            return true;
        }
        arena_release(mark);
    }
    return false;
}

Error process_executable_path(Arena *arena, String *path, Err *err)
{
    char buffer[PATH_LIMIT];
#ifdef __APPLE__
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return err_set(err, ERR_PLATFORM, "executable path is longer than %u bytes", size);
    }
#else
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n < 0) {
        return fail_errno(err, "read /proc/self/exe");
    }
    buffer[n] = '\0';
#endif
    return path_resolve(arena, S(buffer), path, err);
}

int process_id(void)
{
    return (int)getpid();
}

int process_user_id(void)
{
    return (int)getuid();
}

static void *reap_child(void *argument)
{
    pid_t pid = (pid_t)(intptr_t)argument;
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
    return nullptr;
}

Error process_open_default(String path, Err *err)
{
    CPath cpath;
    Error e = to_cpath(path, &cpath, err);
    if (e != ERR_OK) {
        return e;
    }
#ifdef __APPLE__
    char opener[] = "open";
#else
    char opener[] = "xdg-open";
#endif
    char *argv[] = { opener, cpath.text, nullptr };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) {
        rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (rc == 0) {
        rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    pid_t pid;
    if (rc == 0) {
        rc = posix_spawnp(&pid, opener, &actions, nullptr, argv, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        return err_set(err, rc == ENOENT ? ERR_NOT_FOUND : ERR_PLATFORM, "start %s: %s", opener, strerror(rc));
    }
    // A thread waits for the opener so it does not linger as a zombie.
    Thread reaper;
    if (thread_start(&reaper, reap_child, (void *)(intptr_t)pid, nullptr) == ERR_OK) {
        thread_detach(&reaper);
    }
    return ERR_OK;
}

bool process_alive(int pid)
{
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

// ---- environment ----

bool env_get(Arena *arena, String name, String *value)
{
    CPath cname;
    if (to_cpath(name, &cname, nullptr) != ERR_OK) {
        return false;
    }
    const char *found = getenv(cname.text);
    if (found == nullptr) {
        return false;
    }
    *value = str_copy(arena, S(found));
    return true;
}

Error env_set(String name, String value, Err *err)
{
    CPath cname;
    CPath cvalue;
    Error e = to_cpath(name, &cname, err);
    if (e == ERR_OK) {
        e = to_cpath(value, &cvalue, err);
    }
    if (e != ERR_OK) {
        return e;
    }
    return setenv(cname.text, cvalue.text, 1) == 0 ? ERR_OK : fail_errno(err, "set %s", cname.text);
}

void env_unset(String name)
{
    CPath cname;
    if (to_cpath(name, &cname, nullptr) == ERR_OK) {
        unsetenv(cname.text);
    }
}

String env_home(Arena *arena)
{
    const char *home = getenv("HOME");
    if (home != nullptr && *home != '\0') {
        return str_copy(arena, S(home));
    }
    struct passwd account;
    struct passwd *found = nullptr;
    char buffer[4096];
    if (getpwuid_r(getuid(), &account, buffer, sizeof buffer, &found) == 0 && found != nullptr &&
        found->pw_dir != nullptr) {
        return str_copy(arena, S(found->pw_dir));
    }
    return str_copy(arena, S("."));
}

String host_short_name(Arena *arena)
{
    char buffer[256];
    if (gethostname(buffer, sizeof buffer) != 0) {
        buffer[0] = '\0';
    }
    buffer[sizeof buffer - 1] = '\0';
    String name = S(buffer);
    String head;
    String tail;
    if (str_cut(name, '.', &head, &tail) && head.len > 0) {
        name = head;
    }
    return str_copy(arena, name);
}

// ---- signals ----

static void stop_and_reload(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGTERM);
    sigaddset(set, SIGHUP);
}

void signals_block(void)
{
    sigset_t set;
    stop_and_reload(&set);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

Signal signals_wait(void)
{
    sigset_t set;
    stop_and_reload(&set);
    for (;;) {
        int number;
        if (sigwait(&set, &number) == 0) {
            return number == SIGHUP ? SIGNAL_RELOAD : SIGNAL_STOP;
        }
    }
}

// ---- network ----

[[nodiscard]] static bool prepare_socket(int fd)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return false;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) == 0;
#else
    return true;
#endif
}

Error net_listen(String address, int *fd, Err *err)
{
    *fd = -1;
    size_t colon;
    if (!str_find_last_char(address, ':', &colon)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "listen %.*s: want host:port", (int)address.len, address.data);
    }
    CPath host;
    CPath port;
    Error e = to_cpath(str_slice(address, 0, colon), &host, err);
    if (e == ERR_OK) {
        e = to_cpath(str_slice(address, colon + 1, address.len), &port, err);
    }
    if (e != ERR_OK) {
        return e;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE };
    struct addrinfo *addresses;
    int rc = getaddrinfo(host.text[0] != '\0' ? host.text : nullptr, port.text, &hints, &addresses);
    if (rc != 0) {
        return err_set(err, ERR_NETWORK, "listen %s:%s: %s", host.text, port.text, gai_strerror(rc));
    }
    int failure = EADDRNOTAVAIL;
    for (struct addrinfo *a = addresses; a != nullptr && *fd < 0; a = a->ai_next) {
        int sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock < 0) {
            failure = errno;
            continue;
        }
        int one = 1;
        if (!prepare_socket(sock) || setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0 ||
            bind(sock, a->ai_addr, a->ai_addrlen) != 0 || listen(sock, 64) != 0) {
            failure = errno;
            close(sock);
            continue;
        }
        *fd = sock;
    }
    freeaddrinfo(addresses);
    if (*fd < 0) {
        return err_set(err, ERR_NETWORK, "listen %s:%s: %s", host.text, port.text, strerror(failure));
    }
    return ERR_OK;
}

bool net_accept(int listener, int timeout_ms, int *client)
{
    *client = -1;
    struct pollfd ready = { .fd = listener, .events = POLLIN };
    if (poll(&ready, 1, timeout_ms) <= 0) {
        return false;
    }
    *client = accept(listener, nullptr, nullptr);
    if (*client < 0) {
        return false;
    }
    if (!prepare_socket(*client)) {
        close(*client);
        *client = -1;
        return false;
    }
    return true;
}

static int milliseconds_until(int64_t deadline_ns)
{
    int64_t remaining = deadline_ns - clock_monotonic_ns();
    if (remaining <= 0) {
        return 0;
    }
    return (int)min_size((size_t)((remaining + NS_PER_MILLISECOND - 1) / NS_PER_MILLISECOND), INT32_MAX);
}

// wait_writable polls until sock is writable or the deadline passes, which sets errno to ETIMEDOUT.
[[nodiscard]] static bool wait_writable(int sock, int64_t deadline_ns)
{
    for (;;) {
        int timeout_ms = milliseconds_until(deadline_ns);
        if (timeout_ms == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        struct pollfd ready = { .fd = sock, .events = POLLOUT };
        int polled = poll(&ready, 1, timeout_ms);
        if (polled > 0) {
            return true;
        }
        if (polled < 0 && errno != EINTR) {
            return false;
        }
    }
}

[[nodiscard]] static int connect_until(int sock, const struct addrinfo *address, int64_t deadline_ns)
{
    int flags = fcntl(sock, F_GETFL);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    int rc = connect(sock, address->ai_addr, address->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS && wait_writable(sock, deadline_ns)) {
        int failure = 0;
        socklen_t len = sizeof failure;
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &failure, &len) != 0) {
            return -1;
        }
        rc = failure == 0 ? 0 : -1;
        errno = failure;
    }
    if (rc == 0 && fcntl(sock, F_SETFL, flags) != 0) {
        return -1;
    }
    return rc;
}

Error net_connect(String host, String port, int timeout_ms, int *fd, Err *err)
{
    *fd = -1;
    CPath chost;
    CPath cport;
    Error e = to_cpath(host, &chost, err);
    if (e == ERR_OK) {
        e = to_cpath(port, &cport, err);
    }
    if (e != ERR_OK) {
        return e;
    }
    int64_t deadline_ns = timeout_ms < 0 ? INT64_MAX : clock_monotonic_ns() + (int64_t)timeout_ms * NS_PER_MILLISECOND;
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *addresses;
    int rc = getaddrinfo(chost.text, cport.text, &hints, &addresses);
    if (rc != 0) {
        return err_set(err, ERR_NETWORK, "resolve %s: %s", chost.text, gai_strerror(rc));
    }
    int failure = EADDRNOTAVAIL;
    for (struct addrinfo *a = addresses; a != nullptr && *fd < 0; a = a->ai_next) {
        int sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock < 0) {
            failure = errno;
            continue;
        }
        if (!prepare_socket(sock) || connect_until(sock, a, deadline_ns) != 0) {
            failure = errno;
            close(sock);
            continue;
        }
        *fd = sock;
    }
    freeaddrinfo(addresses);
    if (*fd < 0) {
        return err_set(err, ERR_NETWORK, "connect %s:%s: %s", chost.text, cport.text, strerror(failure));
    }
    return ERR_OK;
}

Error net_set_timeouts(int fd, int timeout_ms, Err *err)
{
    struct timeval timeout = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) != 0) {
        return fail_errno(err, "set socket timeouts");
    }
    return ERR_OK;
}

void net_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

[[nodiscard]] static Error unix_address(String path, struct sockaddr_un *address, Err *err)
{
    *address = (struct sockaddr_un){ .sun_family = AF_UNIX };
    if (path.len == 0 || memchr(path.data, '\0', path.len) != nullptr) {
        return err_set(err, ERR_INVALID_ARGUMENT, "unusable socket path: %.*s", (int)min_size(path.len, 200),
                       path.data);
    }
    if (path.len >= sizeof address->sun_path) {
        return err_set(err, ERR_INVALID_ARGUMENT, "socket path %.*s is longer than %zu bytes", (int)path.len,
                       path.data, sizeof address->sun_path - 1);
    }
    memcpy(address->sun_path, path.data, path.len);
    return ERR_OK;
}

Error net_listen_unix(String path, unsigned mode, int *fd, Err *err)
{
    *fd = -1;
    struct sockaddr_un address;
    Error e = unix_address(path, &address, err);
    if (e != ERR_OK) {
        return e;
    }
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return fail_errno(err, "listen %s", address.sun_path);
    }
    if (!prepare_socket(sock) || bind(sock, (struct sockaddr *)&address, sizeof address) != 0) {
        e = fail_errno(err, "listen %s", address.sun_path);
        close(sock);
        return e;
    }
    if (chmod(address.sun_path, (mode_t)mode) != 0 || listen(sock, 64) != 0) {
        e = fail_errno(err, "listen %s", address.sun_path);
        close(sock);
        unlink(address.sun_path);
        return e;
    }
    *fd = sock;
    return ERR_OK;
}

Error net_connect_unix(String path, int *fd, Err *err)
{
    *fd = -1;
    struct sockaddr_un address;
    Error e = unix_address(path, &address, err);
    if (e != ERR_OK) {
        return e;
    }
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0 || !prepare_socket(sock) || connect(sock, (struct sockaddr *)&address, sizeof address) != 0) {
        e = err_set(err, ERR_NETWORK, "connect %s: %s", address.sun_path, strerror(errno));
        if (sock >= 0) {
            close(sock);
        }
        return e;
    }
    *fd = sock;
    return ERR_OK;
}

Error net_send(int fd, String data, Err *err)
{
#ifdef MSG_NOSIGNAL
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
    if (!prepare_socket(fd)) {
        return err_set(err, ERR_NETWORK, "send: %s", strerror(errno));
    }
#endif
    size_t sent = 0;
    while (sent < data.len) {
        ssize_t n = send(fd, data.data + sent, data.len - sent, flags);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return err_set(err, ERR_NETWORK, "send: %s", strerror(errno));
        }
        sent += (size_t)n;
    }
    return ERR_OK;
}

Error net_receive(int fd, void *buffer, size_t capacity, size_t *got, Err *err)
{
    *got = 0;
    for (;;) {
        ssize_t n = recv(fd, buffer, capacity, 0);
        if (n >= 0) {
            *got = (size_t)n;
            return ERR_OK;
        }
        if (errno != EINTR) {
            return err_set(err, ERR_NETWORK, "receive: %s", strerror(errno));
        }
    }
}

void net_shutdown(int fd)
{
    shutdown(fd, SHUT_RDWR);
}
