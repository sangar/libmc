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
#include "posix_shared.h"

extern char **environ;

enum { SLAVE_NAME_LIMIT = 256 };

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

static size_t find_variable(char **items, size_t count, String name)
{
    for (size_t i = 0; i < count; i++) {
        if (strncmp(items[i], name.data, name.len) == 0 && items[i][name.len] == '=') {
            return i;
        }
    }
    return count;
}

// build_environment is this process's environment with overrides applied in
// turn: NAME=value replaces the entry of that name or joins the end, and a
// bare NAME removes it.
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
        bool assigns = str_cut(overrides.items[i], '=', &name, &value);
        size_t at = find_variable(items, count, name);
        if (!assigns) {
            if (at < count) {
                memmove(&items[at], &items[at + 1], (count - at - 1) * sizeof *items);
                count--;
            }
            continue;
        }
        items[at] = (char *)str_cstr(scratch, overrides.items[i]);
        count = at == count ? count + 1 : count;
    }
    items[count] = nullptr;
    return items;
}

// child_path is the PATH the child will have: the last override of it, else this process's.
static String child_path(StringList overrides)
{
    String path = S(getenv("PATH"));
    for (size_t i = 0; i < overrides.count; i++) {
        String name;
        String value;
        bool assigns = str_cut(overrides.items[i], '=', &name, &value);
        if (str_equal(name, S("PATH"))) {
            path = assigns ? value : S("");
        }
    }
    return path;
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

// ChildFailure is what a child that could not become the program reports
// through the status pipe: the step that failed and its errno.
typedef struct {
    int step;
    int code;
} ChildFailure;

enum { STEP_CHDIR = 1, STEP_EXEC = 2 };

// run_child never returns: it becomes the program, which closes the status
// pipe, or writes why it could not to the pipe.
[[noreturn]] static void run_child(int terminal, int status, const char *cwd, const char *program, char **argv,
                                   char **envp)
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
    ChildFailure failure = { .step = STEP_CHDIR };
    if (cwd[0] == '\0' || chdir(cwd) == 0) {
        failure.step = STEP_EXEC;
        execve(program, argv, envp);
    }
    failure.code = errno;
    ssize_t written = write(status, &failure, sizeof failure);
    unused(written);
    _exit(127);
}

// await_exec returns once the child has become the program, or has said why
// it could not; that child it reaps. Either way the child has left this
// process's group by then, so its own group can be signalled.
[[nodiscard]] static Error await_exec(int status, pid_t child, String program, String cwd, Err *err)
{
    ChildFailure failure;
    ssize_t got;
    do {
        got = read(status, &failure, sizeof failure);
    } while (got < 0 && errno == EINTR);
    if (got != (ssize_t)sizeof failure) {
        return ERR_OK;
    }
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    bool missing = failure.code == ENOENT || failure.code == ENOTDIR || failure.code == EACCES;
    Error code = missing ? ERR_NOT_FOUND : ERR_PLATFORM;
    String what = failure.step == STEP_CHDIR ? cwd : program;
    return err_set(err, code, "%.*s: %s", (int)what.len, what.data, strerror(failure.code));
}

Error pty_spawn(Arena *scratch, StringList argv, String cwd, StringList env, PtySize size, Pty *pty, Err *err)
{
    if (argv.count == 0 || size.columns <= 0 || size.rows <= 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "pty_spawn needs a program and a positive size");
    }
    String program = argv.items[0];
    if (!str_contains(program, S("/")) && !posix_look_path(scratch, program, child_path(env), &program)) {
        return err_set(err, ERR_NOT_FOUND, "%.*s: not found on PATH", (int)program.len, program.data);
    }
    const char *program_path = str_cstr(scratch, program);
    const char *cwd_path = str_cstr(scratch, cwd);
    char **child_argv = build_argv(scratch, argv, program_path);
    char **child_env = build_environment(scratch, env);

    // Every descriptor is close-on-exec from the start, so a child that another
    // thread spawns meanwhile does not keep this terminal open.
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) {
        return posix_fail_errno(err, "open pseudo-terminal");
    }
    // The parent opens the terminal side too, so the size is set before the
    // child can ask for it, and the child gets it already open.
    char slave_path[SLAVE_NAME_LIMIT];
    int slave = -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0 || !slave_name(master, slave_path) ||
        (slave = open(slave_path, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) {
        Error failed = posix_fail_errno(err, "prepare pseudo-terminal");
        close(master);
        return failed;
    }
    struct winsize dimensions = window_size(size);
    ioctl(slave, TIOCSWINSZ, &dimensions);
    int status[2];
    if (!posix_make_nonblocking(master) || !posix_open_pipe(status)) {
        Error failed = posix_fail_errno(err, "prepare pseudo-terminal");
        close(slave);
        close(master);
        return failed;
    }

    pid_t child = fork();
    if (child < 0) {
        Error failed = posix_fail_errno(err, "fork");
        close(status[0]);
        close(status[1]);
        close(slave);
        close(master);
        return failed;
    }
    if (child == 0) {
        close(master);
        close(status[0]);
        run_child(slave, status[1], cwd_path, program_path, child_argv, child_env);
    }
    close(slave);
    close(status[1]);
    Error started = await_exec(status[0], child, program, cwd, err);
    close(status[0]);
    if (started != ERR_OK) {
        close(master);
        return started;
    }
    *pty = (Pty) { .fd = master, .pid = child };
    return ERR_OK;
}

Error pty_resize(const Pty *pty, PtySize size, Err *err)
{
    struct winsize dimensions = window_size(size);
    if (ioctl(pty->fd, TIOCSWINSZ, &dimensions) != 0) {
        return posix_fail_errno(err, "resize pseudo-terminal");
    }
    return ERR_OK;
}

Error pty_write(const Pty *pty, String data, int timeout_ms, Err *err)
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
            if (poll(&waiting, 1, timeout_ms) == 0) {
                return err_set(err, ERR_TIMEOUT, "the process is not reading its input");
            }
            continue;
        }
        return posix_fail_errno(err, "write to pseudo-terminal");
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
        [PTY_RELOAD] = SIGHUP,
        [PTY_QUIT] = SIGQUIT,
        [PTY_KILL] = SIGKILL,
    };
    if (pty->pid <= 0) {
        return err_set(err, ERR_NOT_FOUND, "the process has been reaped");
    }
    if (kill(-pty->pid, numbers[signal]) != 0) {
        return errno == ESRCH ? err_set(err, ERR_NOT_FOUND, "no process left in the group")
                              : posix_fail_errno(err, "signal process group");
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
