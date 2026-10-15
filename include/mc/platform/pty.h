#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// Pty is a child process running on a pseudo-terminal of its own. The child
// leads a new session and process group, so a signal sent to the group
// reaches the child and everything it started. What the child writes to its
// terminal is read from fd, and what is written to fd is typed to the child.
//
// One thread can read and reap while another writes, resizes and signals; the
// fields do not change between pty_spawn and pty_close.
typedef struct Pty {
    int fd;
    int pid;
} Pty;

typedef struct PtySize {
    int columns;
    int rows;
} PtySize;

typedef enum PtySignal {
    PTY_INTERRUPT, // SIGINT, what Ctrl-C sends
    PTY_TERMINATE, // SIGTERM
    PTY_KILL,      // SIGKILL
} PtySignal;

// pty_spawn runs argv, argv.items[0] looked up on PATH, on a new terminal of
// the given size. cwd is the working directory, this process's when empty.
// env holds NAME=value entries that replace or add to this process's
// environment. Signals blocked in the calling thread are unblocked in the
// child. A program that is not found, or a missing cwd, is ERR_NOT_FOUND.
// The strings for the system calls are allocated in scratch.
[[nodiscard]] Error pty_spawn(Arena *scratch, StringList argv, String cwd, StringList env, PtySize size, Pty *pty,
                              Err *err);

[[nodiscard]] Error pty_resize(const Pty *pty, PtySize size, Err *err);

// pty_write types data to the child and returns once all of it is accepted,
// ERR_TIMEOUT when the child has stopped reading for a second.
[[nodiscard]] Error pty_write(const Pty *pty, String data, Err *err);

typedef enum PtyEvent {
    PTY_DATA,   // *got bytes were read
    PTY_IDLE,   // nothing arrived within the timeout
    PTY_HANGUP, // every holder has closed the terminal: no more output will come
} PtyEvent;

// pty_read waits up to timeout_ms for output and reads up to capacity bytes.
// Output written before a hang-up is still delivered first.
PtyEvent pty_read(const Pty *pty, int timeout_ms, void *buffer, size_t capacity, size_t *got);

// pty_signal sends the signal to the child's whole process group; a group
// that is gone is ERR_NOT_FOUND.
[[nodiscard]] Error pty_signal(const Pty *pty, PtySignal signal, Err *err);

// pty_reap collects the child's exit without waiting. It returns false while
// the child runs, and true once, with the exit code, or -1 and the signal
// number when a signal ended it. After it returns true pid is 0.
bool pty_reap(Pty *pty, int *exit_code, int *signal);

// pty_close closes the terminal, which hangs up whatever still runs on it.
void pty_close(Pty *pty);
