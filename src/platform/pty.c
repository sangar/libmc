// pty.h for the POSIX systems: macOS and Linux.
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include "mc/platform/pty.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mc/platform/platform.h"

extern char **environ;

enum { SLAVE_NAME_LIMIT = 256, WRITE_TIMEOUT_MS = 1000 };

[[nodiscard]] static Error fail_errno(Err *err, const char *what)
{
    int saved = errno;
    return err_set(err, ERR_PLATFORM, "%s: %s", what, strerror(saved));
}

static struct winsize window_size(PtySize size)
{
    return (struct winsize) {
        .ws_col = (unsigned short)size.columns,
        .ws_row = (unsigned short)size.rows,
    };
}

[[nodiscard]] static bool slave_name(int master, char name[SLAVE_NAME_LIMIT])
{
#ifdef __APPLE__
    return ioctl(master, TIOCPTYGNAME, name) == 0;
#else
    return ptsname_r(master, name, SLAVE_NAME_LIMIT) == 0;
#endif
}

// build_environment is this process's environment with each NAME=value of
// overrides replacing the entry of that name or joining the end.
static char **build_environment(Arena *scratch, StringList overrides)
{
    size_t inherited = 0;
    while (environ[inherited] != nullptr) {
        inherited++;
    }
    size_t capacity = arena_size_add(arena_size_add(inherited, overrides.count), 1);
    char **items = arena_push(scratch, arena_size_mul(capacity, sizeof *items));
    size_t count = 0;
    for (size_t i = 0; i < inherited; i++) {
        items[count++] = environ[i];
    }
    for (size_t i = 0; i < overrides.count; i++) {
        String name;
        String value;
        if (!str_cut(overrides.items[i], '=', &name, &value)) {
            continue;
        }
        char *entry = (char *)str_cstr(scratch, overrides.items[i]);
        size_t at = count;
        for (size_t j = 0; j < count; j++) {
            if (strncmp(items[j], name.data, name.len) == 0 && items[j][name.len] == '=') {
                at = j;
                break;
            }
        }
        items[at] = entry;
        count = at == count ? count + 1 : count;
    }
    items[count] = nullptr;
    return items;
}

static char **build_argv(Arena *scratch, StringList argv, const char *program)
{
    char **items = arena_push(scratch, arena_size_mul(arena_size_add(argv.count, 1), sizeof *items));
    items[0] = (char *)program;
    for (size_t i = 1; i < argv.count; i++) {
        items[i] = (char *)str_cstr(scratch, argv.items[i]);
    }
    return items;
}

static void reset_signals(void)
{
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    const int handled[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGWINCH };
    for (size_t i = 0; i < countof(handled); i++) {
        signal(handled[i], SIG_DFL);
    }
}

// run_child never returns: it becomes the program, or says why it could not.
[[noreturn]] static void run_child(int terminal, const char *cwd, const char *program, char **argv, char **envp,
                                   const char *failure, size_t failure_len)
{
    setsid();
    ioctl(terminal, TIOCSCTTY, 0);
    dup2(terminal, STDIN_FILENO);
    dup2(terminal, STDOUT_FILENO);
    dup2(terminal, STDERR_FILENO);
    if (terminal > STDERR_FILENO) {
        close(terminal);
    }
    reset_signals();
    if (cwd[0] == '\0' || chdir(cwd) == 0) {
        execve(program, argv, envp);
    }
    ssize_t written = write(STDERR_FILENO, failure, failure_len);
    unused(written);
    _exit(127);
}

Error pty_spawn(Arena *scratch, StringList argv, String cwd, StringList env, PtySize size, Pty *pty, Err *err)
{
    if (argv.count == 0 || size.columns <= 0 || size.rows <= 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "pty_spawn needs a program and a positive size");
    }
    String program = argv.items[0];
    if (!str_contains(program, S("/")) && !process_look_path(scratch, program, &program)) {
        return err_set(err, ERR_NOT_FOUND, "%.*s: not found on PATH", (int)argv.items[0].len, argv.items[0].data);
    }
    if (cwd.len > 0) {
        FileInfo info;
        Error found = file_info_follow(cwd, &info, err);
        if (found != ERR_OK) {
            return found;
        }
        if (!info.is_dir) {
            return err_set(err, ERR_NOT_FOUND, "%.*s: not a directory", (int)cwd.len, cwd.data);
        }
    }

    const char *program_path = str_cstr(scratch, program);
    const char *cwd_path = str_cstr(scratch, cwd);
    char **child_argv = build_argv(scratch, argv, program_path);
    char **child_env = build_environment(scratch, env);
    String failure = str_format(scratch, "cannot run %s\r\n", program_path);

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return fail_errno(err, "open pseudo-terminal");
    }
    // The parent opens the terminal side too, so the size is set before the
    // child can ask for it, and the child gets it already open.
    char slave_path[SLAVE_NAME_LIMIT];
    int slave = -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0 || !slave_name(master, slave_path) ||
        (slave = open(slave_path, O_RDWR | O_NOCTTY)) < 0) {
        Error failed = fail_errno(err, "prepare pseudo-terminal");
        close(master);
        return failed;
    }
    struct winsize dimensions = window_size(size);
    ioctl(slave, TIOCSWINSZ, &dimensions);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    pid_t child = fork();
    if (child < 0) {
        Error failed = fail_errno(err, "fork");
        close(slave);
        close(master);
        return failed;
    }
    if (child == 0) {
        close(master);
        run_child(slave, cwd_path, program_path, child_argv, child_env, failure.data, failure.len);
    }
    close(slave);
    *pty = (Pty) { .fd = master, .pid = child };
    return ERR_OK;
}

Error pty_resize(const Pty *pty, PtySize size, Err *err)
{
    struct winsize dimensions = window_size(size);
    if (ioctl(pty->fd, TIOCSWINSZ, &dimensions) != 0) {
        return fail_errno(err, "resize pseudo-terminal");
    }
    return ERR_OK;
}

Error pty_write(const Pty *pty, String data, Err *err)
{
    size_t done = 0;
    while (done < data.len) {
        ssize_t n = write(pty->fd, data.data + done, data.len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN) {
            struct pollfd waiting = { .fd = pty->fd, .events = POLLOUT };
            if (poll(&waiting, 1, WRITE_TIMEOUT_MS) == 0) {
                return err_set(err, ERR_TIMEOUT, "the process is not reading its input");
            }
            continue;
        }
        return fail_errno(err, "write to pseudo-terminal");
    }
    return ERR_OK;
}

PtyEvent pty_read(const Pty *pty, int timeout_ms, void *buffer, size_t capacity, size_t *got)
{
    *got = 0;
    struct pollfd waiting = { .fd = pty->fd, .events = POLLIN };
    int ready = poll(&waiting, 1, timeout_ms);
    if (ready < 0) {
        return errno == EINTR ? PTY_IDLE : PTY_HANGUP;
    }
    if (ready == 0) {
        return PTY_IDLE;
    }
    ssize_t n = read(pty->fd, buffer, capacity);
    if (n > 0) {
        *got = (size_t)n;
        return PTY_DATA;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        return PTY_IDLE;
    }
    return PTY_HANGUP;
}

Error pty_signal(const Pty *pty, PtySignal signal, Err *err)
{
    const int numbers[] = {
        [PTY_INTERRUPT] = SIGINT,
        [PTY_TERMINATE] = SIGTERM,
        [PTY_KILL] = SIGKILL,
    };
    if (kill(-pty->pid, numbers[signal]) != 0) {
        return errno == ESRCH ? err_set(err, ERR_NOT_FOUND, "no process left in the group")
                              : fail_errno(err, "signal process group");
    }
    return ERR_OK;
}

bool pty_reap(Pty *pty, int *exit_code, int *signal)
{
    *exit_code = -1;
    *signal = 0;
    if (pty->pid <= 0) {
        return false;
    }
    int status = 0;
    pid_t done = waitpid(pty->pid, &status, WNOHANG);
    if (done == 0) {
        return false;
    }
    pty->pid = 0;
    if (done > 0 && WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else if (done > 0 && WIFSIGNALED(status)) {
        *signal = WTERMSIG(status);
    }
    return true;
}

void pty_close(Pty *pty)
{
    if (pty->fd >= 0) {
        close(pty->fd);
    }
    pty->fd = -1;
}
