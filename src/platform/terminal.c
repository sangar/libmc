// terminal.h for the POSIX systems: macOS and Linux.
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "mc/platform/terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "mc/core/arena.h"

// Size changes arrive as SIGWINCH. Every thread blocks it, and one thread of
// the terminal's own takes it with sigwait and wakes terminal_wait through a
// pipe, so no handler and no global flag are needed.
struct Terminal {
    Arena *arena;
    int fd;
    struct termios saved;
    sigset_t previous_mask;
    int wake_read;
    int wake_write;
    atomic_bool woken;
    atomic_bool resized;
    atomic_bool closing;
    pthread_t resize_thread;
};

bool terminal_is_terminal(int fd)
{
    return isatty(fd) == 1;
}

static void notify(Terminal *terminal)
{
    char byte = 0;
    // A full pipe already holds a wake-up, so a failed write loses nothing.
    ssize_t written = write(terminal->wake_write, &byte, 1);
    unused(written);
}

static void window_change_only(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGWINCH);
}

static void *take_resizes(void *argument)
{
    Terminal *terminal = argument;
    sigset_t set;
    window_change_only(&set);
    for (;;) {
        int number;
        if (sigwait(&set, &number) != 0) {
            continue;
        }
        if (atomic_load(&terminal->closing)) {
            return nullptr;
        }
        atomic_store(&terminal->resized, true);
        notify(terminal);
    }
}

static void enter_raw_mode(int fd, const struct termios *saved)
{
    struct termios raw = *saved;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSAFLUSH, &raw);
}

static bool open_wake_pipe(Terminal *terminal)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    terminal->wake_read = fds[0];
    terminal->wake_write = fds[1];
    return true;
}

Error terminal_open(Terminal **terminal, Err *err)
{
    *terminal = nullptr;
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return err_set(err, ERR_PLATFORM, "open /dev/tty: %s", strerror(errno));
    }
    struct termios saved;
    if (tcgetattr(fd, &saved) != 0) {
        Error e = err_set(err, ERR_PLATFORM, "terminal: %s", strerror(errno));
        close(fd);
        return e;
    }
    Arena *arena = arena_create(sizeof(Terminal) + 256);
    Terminal *opened = arena_push(arena, sizeof *opened);
    opened->arena = arena;
    opened->fd = fd;
    opened->saved = saved;
    if (!open_wake_pipe(opened)) {
        Error e = err_set(err, ERR_PLATFORM, "pipe: %s", strerror(errno));
        close(fd);
        arena_destroy(arena);
        return e;
    }
    sigset_t window_change;
    window_change_only(&window_change);
    pthread_sigmask(SIG_BLOCK, &window_change, &opened->previous_mask);
    int rc = pthread_create(&opened->resize_thread, nullptr, take_resizes, opened);
    if (rc != 0) {
        Error e = err_set(err, ERR_PLATFORM, "start a thread: %s", strerror(rc));
        pthread_sigmask(SIG_SETMASK, &opened->previous_mask, nullptr);
        close(opened->wake_read);
        close(opened->wake_write);
        close(fd);
        arena_destroy(arena);
        return e;
    }
    enter_raw_mode(fd, &saved);
    *terminal = opened;
    return ERR_OK;
}

void terminal_close(Terminal *terminal)
{
    if (terminal == nullptr) {
        return;
    }
    atomic_store(&terminal->closing, true);
    pthread_kill(terminal->resize_thread, SIGWINCH);
    pthread_join(terminal->resize_thread, nullptr);
    pthread_sigmask(SIG_SETMASK, &terminal->previous_mask, nullptr);
    tcsetattr(terminal->fd, TCSAFLUSH, &terminal->saved);
    close(terminal->fd);
    close(terminal->wake_read);
    close(terminal->wake_write);
    arena_destroy(terminal->arena);
}

TerminalSize terminal_size(Terminal *terminal)
{
    struct winsize size;
    if (ioctl(terminal->fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
        return (TerminalSize){ size.ws_col, size.ws_row };
    }
    return (TerminalSize){ 80, 24 };
}

Error terminal_write(Terminal *terminal, String text, Err *err)
{
    size_t written = 0;
    while (written < text.len) {
        ssize_t n = write(terminal->fd, text.data + written, text.len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return err_set(err, ERR_IO, "write to the terminal: %s", strerror(errno));
        }
        written += (size_t)n;
    }
    return ERR_OK;
}

// wait_readable uses select because poll on macOS does not support terminal devices.
static int wait_readable(int a, int b, int timeout_ms, bool *a_ready, bool *b_ready)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(a, &set);
    if (b >= 0) {
        FD_SET(b, &set);
    }
    struct timeval timeout = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int rc = select((a > b ? a : b) + 1, &set, nullptr, nullptr, timeout_ms < 0 ? nullptr : &timeout);
    *a_ready = rc > 0 && FD_ISSET(a, &set);
    *b_ready = rc > 0 && b >= 0 && FD_ISSET(b, &set);
    return rc;
}

TerminalReady terminal_wait(Terminal *terminal, int timeout_ms)
{
    TerminalReady ready = { 0 };
    bool woken = false;
    wait_readable(terminal->fd, terminal->wake_read, timeout_ms, &ready.input, &woken);
    if (woken) {
        char drain[64];
        while (read(terminal->wake_read, drain, sizeof drain) > 0) {
        }
    }
    ready.woken = atomic_exchange(&terminal->woken, false);
    ready.resized = atomic_exchange(&terminal->resized, false);
    return ready;
}

bool terminal_read_byte(Terminal *terminal, int timeout_ms, unsigned char *byte)
{
    bool ready;
    bool unused_ready;
    if (wait_readable(terminal->fd, -1, timeout_ms, &ready, &unused_ready) <= 0 || !ready) {
        return false;
    }
    return read(terminal->fd, byte, 1) == 1;
}

void terminal_wake(Terminal *terminal)
{
    atomic_store(&terminal->woken, true);
    notify(terminal);
}
