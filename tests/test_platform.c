#include "test.h"

#include "mc/text/path.h"
#include "mc/platform/platform.h"
#include "mc/platform/terminal.h"

#include <signal.h>

static void files_round_trip_in_a_temp_dir(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    String file = path_join(a, dir, S("nested/deeper/data.txt"));
    test_check(test, file_write_atomic(file, S("hello\nworld"), 0644, &err) == ERR_OK, "atomic write creates parents");
    String contents;
    test_check(test, file_read_all(a, file, &contents, &err) == ERR_OK, "read it back");
    test_check_str(test, contents, S("hello\nworld"), "contents survive the round trip");
    test_check(test, contents.data[contents.len] == '\0', "read_all is NUL-terminated");

    FileInfo info;
    test_check(test, file_info(file, &info, &err) == ERR_OK && info.exists && info.is_regular && info.size == 11,
               "file_info describes the file");
    test_check(test, (info.mode & 0777) == 0644, "atomic write applies the mode");
    String missing = path_join(a, dir, S("missing"));
    test_check(test, file_info(missing, &info, &err) == ERR_OK && !info.exists, "a missing path is not an error");
    test_check(test, file_read_all(a, missing, &contents, &err) == ERR_NOT_FOUND, "reading a missing file");
    test_check(test, file_remove(missing, nullptr) == ERR_NOT_FOUND, "removing a missing file");

    String nested = path_join(a, dir, S("nested"));
    test_check(test, file_write_all(path_join(a, nested, S("b")), S(""), 0600, &err) == ERR_OK, "write_all");
    test_check(test, file_write_all(path_join(a, nested, S("a")), S("x"), 0600, &err) == ERR_OK, "write_all again");
    StringList names = { 0 };
    test_check(test, dir_list(a, nested, &names, &err) == ERR_OK, "list a directory");
    test_check_str(test, str_join(a, names, S(",")), S("a,b,deeper"), "entries come sorted");

    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "remove the tree");
    test_check(test, !file_exists(dir), "the tree is gone");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "removing a missing tree is fine");
}

static bool run_quietly(Arena *arena, String *argv, size_t count)
{
    StringList args = { 0 };
    for (size_t i = 0; i < count; i++) {
        strlist_push(arena, &args, argv[i]);
    }
    ProcessResult result;
    return process_run(arena, args, S(""), &result, nullptr) == ERR_OK && result.exit_code == 0;
}

typedef struct {
    String path;
    String contents;
    bool written;
} FifoWriter;

static void *write_fifo(void *argument)
{
    FifoWriter *writer = argument;
    writer->written = file_write_all(writer->path, writer->contents, 0600, nullptr) == ERR_OK;
    return nullptr;
}

static void file_read_all_sizes_its_buffer(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    String big = str_repeat(a, S("0123456789abcdef"), 64 * 1024);
    String file = path_join(a, dir, S("big"));
    test_check(test, file_write_all(file, big, 0600, &err) == ERR_OK, "write 1 MiB");
    Arena *fresh = arena_create(0);
    String contents;
    test_check(test, file_read_all(fresh, file, &contents, &err) == ERR_OK, "read it back");
    test_check(test, str_equal(contents, big), "contents survive");
    test_check(test, arena_bytes_used(fresh) == big.len + 1, "one allocation of the file size");
    arena_destroy(fresh);

    String empty = path_join(a, dir, S("empty"));
    test_check(test, file_write_all(empty, S(""), 0600, &err) == ERR_OK, "write an empty file");
    test_check(test, file_read_all(a, empty, &contents, &err) == ERR_OK && contents.len == 0, "read an empty file");

    FifoWriter writer = { .path = path_join(a, dir, S("fifo")), .contents = str_repeat(a, S("x"), 10000) };
    test_check(test, run_quietly(a, (String[]){ S("mkfifo"), writer.path }, 2), "make a fifo");
    Thread thread;
    test_check(test, thread_start(&thread, write_fifo, &writer, nullptr) == ERR_OK, "start a writer");
    test_check(test, file_read_all(a, writer.path, &contents, &err) == ERR_OK, "read a stream of unknown size");
    thread_join(&thread);
    test_check(test, writer.written && str_equal(contents, writer.contents), "the whole stream arrives");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "remove the tree");
}

static void file_create_unique_validates_its_pattern(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    String with_nul = str_concat(a, path_join(a, dir, S("unique-XXXXXX")), (String){ "\0ignored", 8 });
    String path = { 0 };
    int fd = 0;
    test_check(test, file_create_unique(a, with_nul, &path, &fd, &err) == ERR_INVALID_ARGUMENT,
               "an embedded NUL is ERR_INVALID_ARGUMENT");
    test_check(test, fd == -1, "no descriptor on failure");
    StringList names = { 0 };
    test_check(test, dir_list(a, dir, &names, &err) == ERR_OK && names.count == 0, "no file is created");

    test_check(test, file_create_unique(a, path_join(a, dir, S("unique-XXXXXX")), &path, &fd, &err) == ERR_OK,
               "create a unique file");
    file_close(fd);
    FileInfo info;
    test_check(test, file_info(path, &info, &err) == ERR_OK && info.exists && info.is_regular,
               "file_info accepts the returned path");
    test_check(test, file_remove(path, &err) == ERR_OK, "file_remove accepts the returned path");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "remove the tree");
}

static void dir_create_needs_a_directory(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    String file = path_join(a, dir, S("file"));
    test_check(test, file_write_all(file, S("x"), 0600, &err) == ERR_OK, "write a file");
    test_check(test, dir_create(dir, 0755, &err) == ERR_OK, "an existing directory is fine");
    test_check(test, dir_create_all(dir, 0755, &err) == ERR_OK, "an existing tree is fine");
    test_check(test, dir_create(file, 0755, &err) == ERR_IO, "dir_create on a file fails");
    test_check(test, dir_create_all(file, 0755, &err) == ERR_IO, "dir_create_all on a file fails");
    test_check(test, dir_create_all(path_join(a, file, S("sub")), 0755, &err) != ERR_OK,
               "a file as an intermediate component fails");

    String target = path_join(a, dir, S("target"));
    String link = path_join(a, dir, S("link"));
    String broken = path_join(a, dir, S("broken"));
    test_check(test, dir_create(target, 0755, &err) == ERR_OK, "create a directory");
    test_check(test, run_quietly(a, (String[]){ S("ln"), S("-s"), target, link }, 4), "link to it");
    test_check(test, run_quietly(a, (String[]){ S("ln"), S("-s"), path_join(a, dir, S("missing")), broken }, 4),
               "make a broken link");
    test_check(test, dir_create_all(path_join(a, link, S("sub")), 0755, &err) == ERR_OK,
               "a symlink to a directory counts as one");
    test_check(test, dir_create(broken, 0755, &err) == ERR_IO, "a broken symlink fails");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "remove the tree");
}

static void paths_resolve_against_the_system(Test *test)
{
    Arena *a = test->arena;
    test_check(test, path_is_absolute(path_absolute(a, S("x/../y"))), "absolute joins the working directory");
    test_check_str(test, path_base(path_absolute(a, S("x/../y"))), S("y"), "absolute cleans the path");
    String sh;
    test_check(test, process_look_path(a, S("sh"), &sh) && path_is_executable(sh), "sh is on PATH");
    test_check(test, !process_look_path(a, S("mc-no-such-program"), &sh), "a missing program is not found");
    String executable;
    Err err = { 0 };
    test_check(test, process_executable_path(a, &executable, &err) == ERR_OK && path_is_absolute(executable),
               "the test binary knows its path");
    String home = env_home(a);
    test_check(test, home.len > 0, "a home directory");
}

static void look_path_survives_long_entries(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    String long_dir = path_join(a, dir, str_repeat(a, S("d"), 200));
    String tool = path_join(a, long_dir, S("tool"));
    test_check(test, file_write_atomic(tool, S("#!/bin/sh\n"), 0755, &err) == ERR_OK, "write an executable");
    String saved_path;
    bool had_path = env_get(a, S("PATH"), &saved_path);
    test_check(test, env_set(S("PATH"), str_concat(a, long_dir, S("/")), &err) == ERR_OK, "PATH with a trailing slash");

    Arena *fresh = arena_create(0);
    String found = { 0 };
    bool ok = process_look_path(fresh, S("tool"), &found);
    test_check(test, ok, "the tool is found");
    test_check_str(test, found, tool, "the found path is intact");
    test_check(test, path_is_executable(found), "the found path is executable");
    arena_destroy(fresh);

    if (had_path) {
        test_check(test, env_set(S("PATH"), saved_path, &err) == ERR_OK, "restore PATH");
    }
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "remove the tree");
}

static void processes_capture_output_and_status(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    ProcessResult result;
    const char *const cat[] = { "cat" };
    test_check(test, process_run(a, strlist_of(a, countof(cat), cat), S("piped in"), &result, &err) == ERR_OK,
               "run cat");
    test_check_str(test, result.stdout_text, S("piped in"), "stdin reaches the child and stdout comes back");
    test_check(test, result.exit_code == 0, "cat exits 0");

    const char *const failing[] = { "sh", "-c", "echo oops >&2; exit 3" };
    test_check(test, process_run(a, strlist_of(a, countof(failing), failing), S(""), &result, &err) == ERR_OK,
               "a failing command still runs");
    test_check(test, result.exit_code == 3, "the exit code is reported");
    test_check_str(test, result.stderr_text, S("oops\n"), "stderr is captured apart");

    const char *const missing[] = { "mc-no-such-program" };
    test_check(test, process_run(a, strlist_of(a, countof(missing), missing), S(""), &result, &err) == ERR_NOT_FOUND,
               "a missing program is ERR_NOT_FOUND");
}

static void processes_stream_more_than_a_pipe_holds(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    ProcessResult result;
    String input = str_repeat(a, S("0123456789abcdef"), 256 * 1024);
    const char *const cat[] = { "cat" };
    test_check(test, process_run(a, strlist_of(a, countof(cat), cat), input, &result, &err) == ERR_OK,
               "run cat on 4 MiB");
    test_check(test, str_equal(result.stdout_text, input), "4 MiB round-trips through cat");

    const char *const noisy[] = { "sh", "-c", "head -c 1048576 /dev/zero >&2" };
    test_check(test, process_run(a, strlist_of(a, countof(noisy), noisy), S(""), &result, &err) == ERR_OK,
               "run a child that fills stderr");
    test_check(test, result.stderr_text.len == 1048576, "all of stderr is captured");

    // The suite may inherit SIGPIPE ignored, as GitHub Actions does; the default would end the process.
    void (*inherited)(int) = signal(SIGPIPE, SIG_DFL);
    const char *const deaf[] = { "true" };
    test_check(test, process_run(a, strlist_of(a, countof(deaf), deaf), input, &result, &err) == ERR_OK,
               "a child that exits before reading its input");
    test_check(test, result.exit_code == 0, "its exit code is reported");
    test_check(test, signal(SIGPIPE, inherited) == SIG_DFL, "the process keeps its SIGPIPE handling");
}

static void environment_reads_and_writes(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String value;
    test_check(test, env_set(S("MC_TEST_VAR"), S("set"), &err) == ERR_OK, "set a variable");
    test_check(test, env_get(a, S("MC_TEST_VAR"), &value), "read it");
    test_check_str(test, value, S("set"), "its value");
    env_unset(S("MC_TEST_VAR"));
    test_check(test, !env_get(a, S("MC_TEST_VAR"), &value), "unset removes it");
}

typedef struct {
    Mutex mutex;
    Cond cond;
    int counter;
} Shared;

static void *count_to_thousand(void *argument)
{
    Shared *shared = argument;
    for (int i = 0; i < 1000; i++) {
        mutex_lock(&shared->mutex);
        shared->counter++;
        cond_signal(&shared->cond);
        mutex_unlock(&shared->mutex);
    }
    return nullptr;
}

static void threads_share_state_under_a_mutex(Test *test)
{
    Shared shared = { 0 };
    mutex_init(&shared.mutex);
    cond_init(&shared.cond);
    Thread threads[4];
    bool started = true;
    for (size_t i = 0; i < countof(threads); i++) {
        started = started && thread_start(&threads[i], count_to_thousand, &shared, nullptr) == ERR_OK;
    }
    test_check(test, started, "threads start");
    mutex_lock(&shared.mutex);
    int64_t deadline = clock_monotonic_ns() + 5LL * NS_PER_SECOND;
    while (shared.counter < 4000 && clock_monotonic_ns() < deadline) {
        cond_wait_until(&shared.cond, &shared.mutex, deadline);
    }
    mutex_unlock(&shared.mutex);
    for (size_t i = 0; i < countof(threads); i++) {
        thread_join(&threads[i]);
    }
    test_check(test, shared.counter == 4000, "every increment lands");
    test_check(test, thread_cpu_count() >= 1, "at least one cpu");
    cond_destroy(&shared.cond);
    mutex_destroy(&shared.mutex);
}

static void clock_moves_forward(Test *test)
{
    int64_t before = clock_monotonic_ns();
    clock_sleep_ms(2);
    test_check(test, clock_monotonic_ns() - before >= 2LL * NS_PER_MILLISECOND, "sleep waits");
    test_check(test, clock_wall_ns() > 1700000000LL * NS_PER_SECOND, "wall clock is past 2023");
}

static void network_rejects_a_bad_address(Test *test)
{
    int fd;
    Err err = { 0 };
    test_check(test, net_listen(S("no-port"), &fd, &err) == ERR_INVALID_ARGUMENT, "listen wants host:port");
    test_check(test, net_connect(S("127.0.0.1"), S("1"), 1000, &fd, &err) == ERR_NETWORK,
               "connecting to a closed port is ERR_NETWORK");
}

static const DirEntry *find_entry(const DirEntryList *entries, String name)
{
    for (size_t i = 0; i < entries->count; i++) {
        if (str_equal(entries->items[i].name, name)) {
            return &entries->items[i];
        }
    }
    return nullptr;
}

static void dir_read_describes_each_entry(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    test_check(test, file_write_all(path_join(a, dir, S("data.txt")), S("12345"), 0640, &err) == ERR_OK, "a file");
    test_check(test, dir_create(path_join(a, dir, S("sub")), 0755, &err) == ERR_OK, "a directory");
    String link = path_join(a, dir, S("link"));
    String ln[] = { S("ln"), S("-s"), S("missing-target"), link };
    test_check(test, run_quietly(a, ln, countof(ln)), "a dangling symlink");

    DirEntryList entries = { 0 };
    test_check(test, dir_read(a, dir, &entries, &err) == ERR_OK, "read the directory");
    test_check(test, entries.count == 3, "three entries, without . and ..");
    const DirEntry *file = find_entry(&entries, S("data.txt"));
    test_check(test, file != nullptr && file->info.is_regular && file->info.size == 5 && file->info.mode == 0640,
               "the file's type, size and mode");
    FileInfo stat_info;
    test_check(test, file_info(path_join(a, dir, S("data.txt")), &stat_info, &err) == ERR_OK, "stat the file");
    test_check(test, file != nullptr && file->info.mtime_ns == stat_info.mtime_ns &&
                         file->info.birth_ns == stat_info.birth_ns,
               "times agree with file_info");
    const DirEntry *sub = find_entry(&entries, S("sub"));
    test_check(test, sub != nullptr && sub->info.is_dir, "the directory is one");
    const DirEntry *symlink = find_entry(&entries, S("link"));
    test_check(test, symlink != nullptr && symlink->info.is_symlink, "a symlink is not followed");

    test_check(test, dir_read(a, path_join(a, dir, S("missing")), &entries, &err) == ERR_NOT_FOUND,
               "a missing directory");
    test_check(test, entries.count == 3, "a failed read appends nothing");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "clean up");
}

static void files_map_and_append(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    String file = path_join(a, dir, S("log"));
    test_check(test, file_write_all(file, S("head"), 0600, &err) == ERR_OK, "write a file");

    FileMap map;
    test_check(test, file_map(file, &map, &err) == ERR_OK, "map it");
    test_check_str(test, (String){ map.data, map.len }, S("head"), "the mapping holds the contents");
    test_check(test, file_write_atomic(file, S("replaced"), 0600, &err) == ERR_OK, "replace the file");
    test_check_str(test, (String){ map.data, map.len }, S("head"), "the mapping keeps the old contents");
    file_unmap(map);

    int fd;
    test_check(test, file_open_append(file, &fd, &err) == ERR_OK, "open for appending");
    test_check(test, file_write(fd, S("+tail"), &err) == ERR_OK, "append");
    char start[8] = { 0 };
    size_t got;
    test_check(test, file_pread(fd, start, 8, 0, &got, &err) == ERR_OK && got == 8, "read from the start");
    test_check_str(test, (String){ start, got }, S("replaced"), "reading is not appending");
    file_close(fd);
    String contents;
    test_check(test, file_read_all(a, file, &contents, &err) == ERR_OK, "read it back");
    test_check_str(test, contents, S("replaced+tail"), "writes land at the end");

    String empty = path_join(a, dir, S("empty"));
    test_check(test, file_write_all(empty, S(""), 0600, &err) == ERR_OK, "an empty file");
    test_check(test, file_map(empty, &map, &err) == ERR_OK && map.data == nullptr && map.len == 0,
               "an empty file maps to nothing");
    file_unmap(map);
    String missing = path_join(a, dir, S("missing"));
    test_check(test, file_map(missing, &map, &err) == ERR_NOT_FOUND, "mapping a missing file");
    test_check(test, file_open_append(missing, &fd, &err) == ERR_NOT_FOUND, "appending needs the file");
    test_check(test, !terminal_is_terminal(-1), "no descriptor is no terminal");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "clean up");
}

typedef struct {
    int fd;
    size_t got;
    Error result;
} Receiver;

static void *receive_until_shutdown(void *argument)
{
    Receiver *receiver = argument;
    char buffer[16];
    receiver->result = net_receive(receiver->fd, buffer, sizeof buffer, &receiver->got, nullptr);
    return nullptr;
}

static void unix_sockets_carry_bytes(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, &err) == ERR_OK, "create a temp dir");
    String path = path_join(a, dir, S("s.sock"));
    int listener;
    test_check(test, net_listen_unix(path, 0600, &listener, &err) == ERR_OK, "listen");
    FileInfo info;
    test_check(test, file_info(path, &info, &err) == ERR_OK && info.exists && (info.mode & 0777) == 0600,
               "the socket gets the mode");
    int other;
    test_check(test, net_listen_unix(path, 0600, &other, &err) == ERR_IO, "a taken path");
    test_check(test, net_listen_unix(str_repeat(a, S("x"), 200), 0600, &other, &err) == ERR_INVALID_ARGUMENT,
               "a path too long for a socket");

    int client;
    test_check(test, net_connect_unix(path, &client, &err) == ERR_OK, "connect");
    int server;
    test_check(test, net_accept(listener, 1000, &server), "accept");
    test_check(test, net_send(client, S("ping"), &err) == ERR_OK, "send");
    char buffer[16];
    size_t got;
    test_check(test, net_receive(server, buffer, sizeof buffer, &got, &err) == ERR_OK, "receive");
    test_check_str(test, (String){ buffer, got }, S("ping"), "the bytes arrive");

    Receiver receiver = { .fd = server, .got = 99 };
    Thread thread;
    test_check(test, thread_start(&thread, receive_until_shutdown, &receiver, &err) == ERR_OK, "start a receiver");
    clock_sleep_ms(20);
    net_shutdown(server);
    thread_join(&thread);
    test_check(test, receiver.result == ERR_OK && receiver.got == 0, "shutdown wakes a blocked receive");

    net_close(server);
    Error sent = ERR_OK;
    for (int i = 0; i < 100 && sent == ERR_OK; i++) {
        sent = net_send(client, S("data"), nullptr);
    }
    test_check(test, sent == ERR_NETWORK, "sending to a closed peer fails without SIGPIPE");
    net_close(client);
    net_close(listener);
    test_check(test, net_connect_unix(path, &client, &err) == ERR_NETWORK, "nothing listens any more");
    test_check(test, process_user_id() >= 0, "a user id");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "clean up");
}

const TestCase PLATFORM_TESTS[] = {
    { "files_round_trip_in_a_temp_dir", files_round_trip_in_a_temp_dir },
    { "file_read_all_sizes_its_buffer", file_read_all_sizes_its_buffer },
    { "file_create_unique_validates_its_pattern", file_create_unique_validates_its_pattern },
    { "dir_create_needs_a_directory", dir_create_needs_a_directory },
    { "paths_resolve_against_the_system", paths_resolve_against_the_system },
    { "look_path_survives_long_entries", look_path_survives_long_entries },
    { "processes_capture_output_and_status", processes_capture_output_and_status },
    { "processes_stream_more_than_a_pipe_holds", processes_stream_more_than_a_pipe_holds },
    { "environment_reads_and_writes", environment_reads_and_writes },
    { "threads_share_state_under_a_mutex", threads_share_state_under_a_mutex },
    { "clock_moves_forward", clock_moves_forward },
    { "network_rejects_a_bad_address", network_rejects_a_bad_address },
    { "dir_read_describes_each_entry", dir_read_describes_each_entry },
    { "files_map_and_append", files_map_and_append },
    { "unix_sockets_carry_bytes", unix_sockets_carry_bytes },
    { nullptr, nullptr },
};
