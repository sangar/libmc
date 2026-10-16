#include "test.h"

#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "mc/platform/pty.h"
#include "mc/platform/terminal.h"

#include <signal.h>
#include <stdlib.h>

enum { OUTPUT_DEADLINE_MS = 5000 };

static const PtySize SIZE = { .columns = 100, .rows = 30 };

static StringList shell(Arena *arena, const char *script)
{
    const char *const argv[] = { "sh", "-c", script };
    return strlist_of(arena, countof(argv), argv);
}

// read_until_hangup collects what the child writes until its terminal closes.
static String read_until_hangup(Arena *arena, Pty *pty)
{
    StringBuilder output = str_builder_create(arena, 256);
    int64_t deadline = clock_monotonic_ns() + (int64_t)OUTPUT_DEADLINE_MS * NS_PER_MILLISECOND;
    char buffer[512];
    while (clock_monotonic_ns() < deadline) {
        size_t got;
        PtyEvent event = pty_read(pty, 50, buffer, sizeof buffer, &got);
        if (event == PTY_HANGUP) {
            break;
        }
        str_builder_append(&output, (String) { buffer, got });
    }
    return str_builder_finish(&output);
}

static bool wait_for_exit(Pty *pty, int *exit_code, int *signal)
{
    int64_t deadline = clock_monotonic_ns() + (int64_t)OUTPUT_DEADLINE_MS * NS_PER_MILLISECOND;
    while (clock_monotonic_ns() < deadline) {
        if (pty_reap(pty, exit_code, signal)) {
            return true;
        }
        clock_sleep_ms(10);
    }
    return false;
}

static void pty_runs_a_program_on_a_terminal(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    const char *const env[] = { "MC_PTY_GREETING=hello", "HOME=/nowhere" };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-pty-"), &dir, &err) == ERR_OK, "create a temp dir");
    String resolved;
    test_check(test, path_resolve(a, dir, &resolved, &err) == ERR_OK, "resolve the temp dir");
    Pty pty;
    Error spawned = pty_spawn(a, shell(a, "test -t 1 && echo \"$MC_PTY_GREETING $HOME\"; pwd; stty size; exit 3"),
                              dir, strlist_of(a, countof(env), env), SIZE, &pty, &err);
    test_check(test, spawned == ERR_OK, "spawn sh");
    String output = read_until_hangup(a, &pty);
    test_check(test, str_contains(output, S("hello /nowhere\r\n")), "env entries add to and replace the environment");
    test_check(test, str_contains(output, str_format(a, "%.*s\r\n", (int)resolved.len, resolved.data)),
               "runs in cwd");
    test_check(test, str_contains(output, S("30 100\r\n")), "the terminal has the given size");
    int exit_code;
    int signal;
    test_check(test, wait_for_exit(&pty, &exit_code, &signal) && exit_code == 3 && signal == 0, "reaps the exit code");
    test_check(test, pty.pid == 0 && !pty_reap(&pty, &exit_code, &signal), "reaps only once");
    pty_close(&pty);
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "remove the temp dir");
}

static void pty_passes_input_and_signals_the_group(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    Pty pty;
    test_check(test, pty_spawn(a, shell(a, "read line; echo \"got $line\"; sleep 30 & wait"), S(""), (StringList) { 0 },
                               SIZE, &pty, &err) == ERR_OK,
               "spawn sh");
    test_check(test, pty_write(&pty, S("ping\r"), 1000, &err) == ERR_OK, "type a line");
    String echoed = { 0 };
    int64_t deadline = clock_monotonic_ns() + (int64_t)OUTPUT_DEADLINE_MS * NS_PER_MILLISECOND;
    StringBuilder output = str_builder_create(a, 256);
    while (!str_contains(echoed, S("got ping")) && clock_monotonic_ns() < deadline) {
        char buffer[256];
        size_t got;
        if (pty_read(&pty, 50, buffer, sizeof buffer, &got) == PTY_DATA) {
            str_builder_append(&output, (String) { buffer, got });
            echoed = str_builder_finish(&output);
        }
    }
    test_check(test, str_contains(echoed, S("got ping")), "the program reads what was typed");
    test_check(test, pty_resize(&pty, (PtySize) { .columns = 120, .rows = 40 }, &err) == ERR_OK, "resize");
    test_check(test, pty_signal(&pty, PTY_TERMINATE, &err) == ERR_OK, "signal the group");
    int exit_code;
    int signal;
    test_check(test, wait_for_exit(&pty, &exit_code, &signal) && signal == SIGTERM, "the signal ends the child");
    test_check(test, read_until_hangup(a, &pty).len < 4096, "the terminal hangs up once the group is gone");
    pty_close(&pty);
}

static void pty_env_removes_and_finds_programs_on_the_childs_path(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-pty-path-"), &dir, &err) == ERR_OK, "create a temp dir");
    String tool = path_join(a, dir, S("mc-pty-tool"));
    test_check(test, file_write_all(tool, S("#!/bin/sh\necho \"tool ${HOME-unset}\"\n"), 0755, &err) == ERR_OK,
               "write a program");
    const char *const env[] = { "HOME", "PATH" };
    StringList overrides = strlist_of(a, countof(env), env);
    overrides.items[1] = str_format(a, "PATH=%.*s:/bin:/usr/bin", (int)dir.len, dir.data);
    const char *const argv[] = { "mc-pty-tool" };
    Pty pty;
    Error spawned = pty_spawn(a, strlist_of(a, 1, argv), S(""), overrides, SIZE, &pty, &err);
    test_check(test, spawned == ERR_OK, "a program on the child's PATH");
    if (spawned == ERR_OK) {
        test_check(test, str_contains(read_until_hangup(a, &pty), S("tool unset")), "a bare NAME removes it");
        int exit_code;
        int signal;
        wait_for_exit(&pty, &exit_code, &signal);
        pty_close(&pty);
    }
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "remove the temp dir");
}

static void pty_reports_what_cannot_start(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    const char *const missing[] = { "mc-no-such-program" };
    Pty pty;
    Error spawned = pty_spawn(a, strlist_of(a, 1, missing), S(""), (StringList) { 0 }, SIZE, &pty, &err);
    test_check(test, spawned == ERR_NOT_FOUND, "a program not on PATH");
    const char *const missing_path[] = { "/mc/no/such/program" };
    spawned = pty_spawn(a, strlist_of(a, 1, missing_path), S(""), (StringList) { 0 }, SIZE, &pty, &err);
    test_check(test, spawned == ERR_NOT_FOUND, "a path to nothing");
    const char *const relative[] = { "./sh" };
    spawned = pty_spawn(a, strlist_of(a, 1, relative), S("/bin"), (StringList) { 0 }, SIZE, &pty, &err);
    test_check(test, spawned == ERR_OK, "a relative path starts in cwd");
    int exit_code;
    int signal;
    test_check(test, pty_signal(&pty, PTY_KILL, &err) == ERR_OK && wait_for_exit(&pty, &exit_code, &signal),
               "it ran");
    pty_close(&pty);
    test_check(test, pty_spawn(a, shell(a, "true"), S("/mc/no/such/dir"), (StringList) { 0 }, SIZE, &pty, &err) ==
                         ERR_NOT_FOUND,
               "a missing working directory");
}

// terminal_round_trip opens and closes the controlling terminal, then
// aborts to show it got that far.
void terminal_round_trip(void)
{
    Terminal *terminal;
    if (terminal_open(&terminal, nullptr) != ERR_OK) {
        return;
    }
    terminal_close(terminal);
    abort();
}

static void terminal_closes_on_a_pseudo_terminal(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    const char *const args[] = { "", "--abort-case", "terminal_round_trip" };
    StringList argv = strlist_of(a, countof(args), args);
    test_check(test, process_executable_path(a, &argv.items[0], &err) == ERR_OK, "find the test binary");
    Pty pty;
    test_check(test, pty_spawn(a, argv, S(""), (StringList) { 0 }, SIZE, &pty, &err) == ERR_OK, "spawn it");
    read_until_hangup(a, &pty);
    int exit_code;
    int signal;
    bool ended = wait_for_exit(&pty, &exit_code, &signal);
    if (!ended) {
        Error killed = pty_signal(&pty, PTY_KILL, nullptr);
        unused(killed);
        wait_for_exit(&pty, &exit_code, &signal);
    }
    test_check(test, ended && signal == SIGABRT, "terminal_close returns");
    pty_close(&pty);
}

// caught runs a shell that says which of HUP and QUIT it gets, sends it
// signal, and returns what it said.
static String caught(Test *test, PtySignal signal)
{
    Arena *a = test->arena;
    Pty pty;
    const char *script = "trap 'echo got HUP; exit 0' HUP; trap 'echo got QUIT; exit 0' QUIT; echo ready; "
                         "while :; do sleep 0.05; done";
    if (pty_spawn(a, shell(a, script), S(""), (StringList) { 0 }, SIZE, &pty, nullptr) != ERR_OK) {
        return S("");
    }
    StringBuilder output = str_builder_create(a, 256);
    String said = S("");
    bool sent = false;
    int64_t deadline = clock_monotonic_ns() + (int64_t)OUTPUT_DEADLINE_MS * NS_PER_MILLISECOND;
    while (!str_contains(said, S("got ")) && clock_monotonic_ns() < deadline) {
        char buffer[256];
        size_t got;
        if (pty_read(&pty, 50, buffer, sizeof buffer, &got) == PTY_DATA) {
            str_builder_append(&output, (String) { buffer, got });
            said = str_builder_finish(&output);
        }
        if (!sent && str_contains(said, S("ready"))) {
            sent = pty_signal(&pty, signal, nullptr) == ERR_OK;
        }
    }
    int exit_code;
    int ended;
    if (!wait_for_exit(&pty, &exit_code, &ended)) {
        Error killed = pty_signal(&pty, PTY_KILL, nullptr);
        unused(killed);
        wait_for_exit(&pty, &exit_code, &ended);
    }
    pty_close(&pty);
    return said;
}

static void pty_sends_reload_and_quit(Test *test)
{
    test_check(test, str_contains(caught(test, PTY_RELOAD), S("got HUP")), "SIGHUP");
    test_check(test, str_contains(caught(test, PTY_QUIT), S("got QUIT")), "SIGQUIT");
}

const TestCase PTY_TESTS[] = {
    { "pty_runs_a_program_on_a_terminal", pty_runs_a_program_on_a_terminal },
    { "pty_passes_input_and_signals_the_group", pty_passes_input_and_signals_the_group },
    { "pty_env_removes_and_finds_programs_on_the_childs_path", pty_env_removes_and_finds_programs_on_the_childs_path },
    { "pty_reports_what_cannot_start", pty_reports_what_cannot_start },
    { "terminal_closes_on_a_pseudo_terminal", terminal_closes_on_a_pseudo_terminal },
    { "pty_sends_reload_and_quit", pty_sends_reload_and_quit },
    { nullptr, nullptr },
};
