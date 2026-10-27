#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// Everything the library needs from the operating system, in portable types.
// src/platform/ holds the only code that includes OS headers or tests which
// OS it is built for. Paths are String; a path longer than the OS allows, or
// one containing a NUL byte, is ERR_INVALID_ARGUMENT. Files and sockets are
// plain descriptors.

// ---- clock ----

int64_t clock_monotonic_ns(void);
// clock_wall_ns is the time since the Unix epoch.
int64_t clock_wall_ns(void);
void clock_sleep_ms(int64_t milliseconds);
// clock_local_offset is the local zone's offset from UTC in seconds east, at a Unix time.
int32_t clock_local_offset(int64_t unix_seconds);

// ---- numbers ----

// float_parse reads the whole of text as a floating-point number with '.'
// as the decimal point, whatever locale the program has set. A value too
// large for a double is false.
bool float_parse(String text, double *value);
// float_format writes the shortest text that float_parse reads back as value.
String float_format(Arena *arena, double value);

// ---- threads ----

typedef struct Mutex {
    alignas(8) unsigned char storage[64];
} Mutex;

typedef struct Cond {
    alignas(8) unsigned char storage[64];
} Cond;

typedef struct Thread {
    alignas(8) unsigned char storage[16];
} Thread;

// The mutex, condition and join functions abort if the system call fails.
void mutex_init(Mutex *mutex);
void mutex_destroy(Mutex *mutex);
void mutex_lock(Mutex *mutex);
void mutex_unlock(Mutex *mutex);

void cond_init(Cond *cond);
void cond_destroy(Cond *cond);
void cond_signal(Cond *cond);
void cond_broadcast(Cond *cond);
void cond_wait(Cond *cond, Mutex *mutex);
// cond_wait_until returns when signalled or once the clock_monotonic_ns deadline has passed.
void cond_wait_until(Cond *cond, Mutex *mutex, int64_t deadline_ns);

[[nodiscard]] Error thread_start(Thread *thread, void *(*run)(void *argument), void *argument, Err *err);
void thread_join(Thread *thread);
void thread_detach(Thread *thread);
size_t thread_cpu_count(void);

// ---- files ----

typedef struct FileInfo {
    bool exists;
    bool is_dir;
    bool is_regular;
    bool is_symlink;
    int64_t size;
    int64_t mtime_ns;
    // birth_ns is when the file was created, 0 where the system does not
    // report it, as Linux stat does not.
    int64_t birth_ns;
    unsigned mode; // permission bits
} FileInfo;

// file_info describes path without following a final symlink; a missing path is ERR_OK with exists false.
[[nodiscard]] Error file_info(String path, FileInfo *info, Err *err);
[[nodiscard]] Error file_info_follow(String path, FileInfo *info, Err *err);
[[nodiscard]] Error file_info_fd(int fd, FileInfo *info, Err *err);
bool file_exists(String path);

// file_open_read is ERR_NOT_FOUND when path or a directory on the way is missing.
[[nodiscard]] Error file_open_read(String path, int *fd, Err *err);
// file_open_append opens an existing file to read anywhere in it and to
// append to it; ERR_NOT_FOUND when it is missing.
[[nodiscard]] Error file_open_append(String path, int *fd, Err *err);
// file_create opens path for writing, creating or truncating it.
[[nodiscard]] Error file_create(String path, unsigned mode, int *fd, Err *err);
// file_create_unique replaces the trailing XXXXXX of pattern with a unique name, creates that file and stores its path.
[[nodiscard]] Error file_create_unique(Arena *arena, String pattern, String *path, int *fd, Err *err);
void file_close(int fd);
// file_lock opens path, creating it with mode 0600 when missing, and waits for
// an exclusive lock on it, held until file_close(*fd). The lock is advisory:
// it keeps out only others that take it, in this process or another.
[[nodiscard]] Error file_lock(String path, int *fd, Err *err);
// file_try_lock is file_lock without waiting: when another holds the lock,
// it is ERR_OK with *locked false and *fd -1.
[[nodiscard]] Error file_try_lock(String path, int *fd, bool *locked, Err *err);
// file_find_upward looks in directory and then each directory above it for
// the first of names that exists, and stores its path.
bool file_find_upward(Arena *arena, String directory, StringList names, String *path);
// file_read reads up to capacity bytes; *got is 0 at end of file.
[[nodiscard]] Error file_read(int fd, void *buffer, size_t capacity, size_t *got, Err *err);
[[nodiscard]] Error file_pread(int fd, void *buffer, size_t capacity, int64_t offset, size_t *got, Err *err);
// file_write writes all of data, retrying short writes.
[[nodiscard]] Error file_write(int fd, String data, Err *err);
[[nodiscard]] Error file_seek(int fd, int64_t offset, Err *err);
[[nodiscard]] Error file_truncate(int fd, int64_t size, Err *err);
[[nodiscard]] Error file_rename(String from, String to, Err *err);
// file_remove is ERR_NOT_FOUND when nothing was there.
[[nodiscard]] Error file_remove(String path, Err *err);
[[nodiscard]] Error file_set_mtime(String path, int64_t mtime_ns, Err *err);

// file_read_to_end reads fd until its end into the arena, NUL-terminated,
// such as a file or standard input from a pipe.
[[nodiscard]] Error file_read_to_end(Arena *arena, int fd, String *contents, Err *err);
// file_read_all reads the whole file into the arena, NUL-terminated.
[[nodiscard]] Error file_read_all(Arena *arena, String path, String *contents, Err *err);
[[nodiscard]] Error file_write_all(String path, String contents, unsigned mode, Err *err);
// file_write_atomic creates the parent directories, writes a temporary file beside path and renames it over path.
[[nodiscard]] Error file_write_atomic(String path, String contents, unsigned mode, Err *err);

// FileMap is a whole file mapped read-only into memory. The mapping keeps
// the bytes the file had when it was mapped, even after the file is renamed
// over or removed, until file_unmap.
typedef struct FileMap {
    const void *data;
    size_t len;
} FileMap;

// file_map is ERR_NOT_FOUND when path is missing. An empty file maps to
// nullptr and length 0.
[[nodiscard]] Error file_map(String path, FileMap *map, Err *err);
void file_unmap(FileMap map);

// ---- directories ----

// dir_create and dir_create_all are ERR_OK when the directory already exists,
// including through a symlink. Anything else in the way, such as a regular
// file or a broken symlink, is ERR_IO.
[[nodiscard]] Error dir_create(String path, unsigned mode, Err *err);
[[nodiscard]] Error dir_create_all(String path, unsigned mode, Err *err);
[[nodiscard]] Error dir_remove(String path, Err *err);
// dir_remove_all removes path and everything below it; a missing path is ERR_OK.
[[nodiscard]] Error dir_remove_all(String path, Err *err);
// dir_list appends the sorted entry names of path, without . and .., to names.
[[nodiscard]] Error dir_list(Arena *arena, String path, StringList *names, Err *err);
// DirEntry is one entry of a directory, described as file_info would.
typedef struct DirEntry {
    String name;
    FileInfo info;
} DirEntry;

typedef struct DirEntryList {
    DirEntry *items;
    size_t count;
    size_t capacity;
} DirEntryList;

// dir_read appends the entries of path, without . and .., in the order the
// system lists them, each described without following a final symlink. It
// is dir_list and file_info in one pass: on macOS one getattrlistbulk call
// describes many entries, elsewhere each costs a stat. An entry that
// disappears while the directory is read is left out. On an error, entries
// keeps what was read before it.
[[nodiscard]] Error dir_read(Arena *arena, String path, DirEntryList *entries, Err *err);
// dir_create_temp makes a fresh directory named prefix plus a random suffix in the temporary directory.
[[nodiscard]] Error dir_create_temp(Arena *arena, String prefix, String *path, Err *err);
// dir_runtime is a directory only this user can use, for sockets and locks:
// $XDG_RUNTIME_DIR/name where that is set, else name-<uid> in the temporary
// directory. It is made with mode 0700 when missing. One that is a symlink,
// belongs to another user or lets others in is ERR_IO, since in a shared
// temporary directory another user could have made it first.
[[nodiscard]] Error dir_runtime(Arena *arena, String name, String *path, Err *err);

// ---- paths the file system decides ----

// path_resolve follows every symlink to the canonical absolute path.
[[nodiscard]] Error path_resolve(Arena *arena, String path, String *resolved, Err *err);
// path_absolute joins a relative path to the working directory and cleans it.
String path_absolute(Arena *arena, String path);
bool path_is_executable(String path);
bool paths_are_same_file(String a, String b);

// ---- processes ----

typedef struct ProcessResult {
    String stdout_text;
    String stderr_text;
    int exit_code; // -1 when a signal ended the process
    int signal;
} ProcessResult;

// process_run runs argv, argv.items[0] looked up on PATH, feeding it stdin_text
// and capturing its output into the arena. A non-zero exit is not an error;
// a program that cannot be started is ERR_NOT_FOUND or ERR_PLATFORM.
[[nodiscard]] Error process_run(Arena *arena, StringList argv, String stdin_text, ProcessResult *result, Err *err);
// process_run_interactive runs argv on this process's terminal; *exit_code is 128 plus the signal when one ended it.
[[nodiscard]] Error process_run_interactive(Arena *scratch, StringList argv, int *exit_code, Err *err);
bool process_look_path(Arena *arena, String name, String *path);
[[nodiscard]] Error process_executable_path(Arena *arena, String *path, Err *err);
int process_id(void);
// process_user_id is the real user id, which tells apart the per-user
// resources of a shared machine, such as sockets in a shared directory.
int process_user_id(void);
// process_open_default opens path in the desktop's default application for
// it, with open on macOS and xdg-open elsewhere, and does not wait for it.
// The program's input and output are the null device.
[[nodiscard]] Error process_open_default(String path, Err *err);
// process_spawn_detached starts argv, argv.items[0] looked up on PATH, in a
// session of its own, so it outlives this process and its terminal, and does
// not wait for it. Its input is the null device; its output and errors are
// appended to output_path, created with mode 0600 when missing, or go to the
// null device when output_path is empty. It starts with no signal blocked and
// every signal's default action. A thread of this process reaps it once it
// ends. A program that cannot be started is ERR_NOT_FOUND or ERR_PLATFORM.
[[nodiscard]] Error process_spawn_detached(Arena *scratch, StringList argv, String output_path, int *pid, Err *err);
bool process_alive(int pid);
// process_raise_file_limit lifts this process's soft limit on open descriptors to the hard limit.
[[nodiscard]] Error process_raise_file_limit(Err *err);

// ---- environment ----

bool env_get(Arena *arena, String name, String *value);
[[nodiscard]] Error env_set(String name, String value, Err *err);
void env_unset(String name);
// env_all lists the environment as NAME=value entries copied into the arena.
StringList env_all(Arena *arena);
// env_home is $HOME, else the account's home directory, else ".".
String env_home(Arena *arena);
String host_short_name(Arena *arena);

// ---- signals ----

typedef enum Signal { SIGNAL_STOP, SIGNAL_RELOAD } Signal;

// signals_block keeps the stop (SIGINT, SIGTERM) and reload (SIGHUP) signals
// from interrupting any thread; call it before starting threads.
void signals_block(void);
// signals_wait blocks until a stop or reload signal arrives.
Signal signals_wait(void);

// SignalWatch calls on_signal(context, signal) on a thread of its own for
// each stop or reload signal, as signals_wait reports them. The signals must
// be blocked in every thread, with signals_block before any thread starts.
typedef struct SignalWatch SignalWatch;

[[nodiscard]] Error signal_watch_start(void (*on_signal)(void *context, Signal signal), void *context,
                                       SignalWatch **watch, Err *err);
// signal_watch_stop ends the watching thread; on_signal is not called after it returns.
void signal_watch_stop(SignalWatch *watch);
// process_signal sends pid the stop (SIGTERM) or reload (SIGHUP) signal; a
// process that is gone is ERR_NOT_FOUND.
[[nodiscard]] Error process_signal(int pid, Signal signal, Err *err);

// ---- network ----

// net_listen binds host:port and listens. An empty host is every interface,
// IPv6 and IPv4 on one socket where the system has IPv6, IPv4 alone where not.
[[nodiscard]] Error net_listen(String address, int *fd, Err *err);
// net_local_address is the host:port fd is bound to, with the port chosen
// when it listened on port 0, in the form net_listen and net_connect take.
[[nodiscard]] Error net_local_address(Arena *arena, int fd, String *address, Err *err);
// net_accept waits up to timeout_ms for a connection and reports whether one arrived.
bool net_accept(int listener, int timeout_ms, int *client);
// net_connect is ERR_NETWORK when the host cannot be resolved or reached within timeout_ms.
// The timeout, unlimited when negative, is one deadline shared by every
// address tried. Name resolution counts toward it but blocks in the system
// resolver, which cannot be cut short.
[[nodiscard]] Error net_connect(String host, String port, int timeout_ms, int *fd, Err *err);
// net_set_timeouts bounds each later send and receive on fd.
[[nodiscard]] Error net_set_timeouts(int fd, int timeout_ms, Err *err);
void net_close(int fd);

// net_listen_unix creates a Unix domain socket at path with the permission
// bits mode and listens. Something already at path is ERR_IO; a path too long
// for a socket address is ERR_INVALID_ARGUMENT.
[[nodiscard]] Error net_listen_unix(String path, unsigned mode, int *fd, Err *err);
// net_connect_unix is ERR_NETWORK when nothing listens at path.
[[nodiscard]] Error net_connect_unix(String path, int *fd, Err *err);
// net_send writes all of data to a connected socket. A peer that has gone is
// ERR_NETWORK, never a SIGPIPE.
[[nodiscard]] Error net_send(int fd, String data, Err *err);
// net_receive reads up to capacity bytes; *got is 0 once the peer has closed.
[[nodiscard]] Error net_receive(int fd, void *buffer, size_t capacity, size_t *got, Err *err);
// net_pair creates two connected stream sockets in this process, for
// driving a server in-process without a port. Close each with net_close.
[[nodiscard]] Error net_pair(int fds[2], Err *err);
// net_receive_nowait reads up to capacity bytes that have already arrived,
// without waiting: *got is 0 when nothing has, or once the peer has closed.
[[nodiscard]] Error net_receive_nowait(int fd, void *buffer, size_t capacity, size_t *got, Err *err);
// net_shutdown ends both directions of fd, which wakes a thread blocked in
// net_receive on it. The descriptor stays open until net_close.
void net_shutdown(int fd);

// NetResult is how far a call that does not wait got, for event loops on
// mc/platform/poll.h.
typedef enum NetResult {
    NET_DONE,        // some bytes moved
    NET_WOULD_BLOCK, // none can move until the socket is ready
    NET_CLOSED,      // the peer closed, or the connection failed
} NetResult;

// net_set_nonblocking makes calls on fd return instead of waiting, as a
// listener for net_try_accept needs.
[[nodiscard]] Error net_set_nonblocking(int fd, Err *err);
// net_try_accept takes a connection that is already waiting and reports
// whether there was one. The connection is non-blocking, never raises
// SIGPIPE, and over TCP sends without Nagle's delay.
bool net_try_accept(int listener, int *client);
// net_try_receive reads up to capacity bytes that have arrived. Unlike
// net_receive_nowait it tells a peer that closed from one that is quiet.
NetResult net_try_receive(int fd, void *buffer, size_t capacity, size_t *got);
// net_try_send writes what of data the socket takes now. On a socket libmc
// opened or accepted it never raises SIGPIPE.
NetResult net_try_send(int fd, String data, size_t *sent);
