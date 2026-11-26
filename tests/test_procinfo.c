#include "test.h"

#include "mc/platform/platform.h"
#include "mc/platform/procinfo.h"
#include "mc/platform/pty.h"

static size_t count_sockets(SocketList sockets, SocketState state, int port, int pid)
{
    size_t count = 0;
    for (size_t i = 0; i < sockets.count; i++) {
        const SocketEntry *entry = &sockets.items[i];
        count += entry->protocol == SOCKET_TCP && entry->state == state && entry->local_port == port && entry->pid == pid;
    }
    return count;
}

static int port_of(String address)
{
    size_t colon;
    int64_t port = 0;
    if (str_find_last_char(address, ':', &colon)) {
        (void)str_parse_i64(str_slice(address, colon + 1, address.len), &port);
    }
    return (int)port;
}

static void sockets_list_finds_our_listener_and_its_connections(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    int listener;
    String address;
    test_check(test, net_listen(S("127.0.0.1:0"), &listener, &err) == ERR_OK, "listen");
    test_check(test, net_local_address(a, listener, &address, &err) == ERR_OK, "name the listener");
    int port = port_of(address);
    int clients[2];
    int accepted[2];
    for (size_t i = 0; i < countof(clients); i++) {
        test_check(test, net_connect(S("127.0.0.1"), str_format(a, "%d", port), 1000, &clients[i], &err) == ERR_OK,
                   "connect");
        test_check(test, net_accept(listener, 1000, &accepted[i]), "accept");
    }

    SocketList sockets;
    test_check(test, sockets_list(a, &sockets, &err) == ERR_OK, "list sockets");
    test_check(test, count_sockets(sockets, SOCKET_LISTEN, port, process_id()) == 1, "our listener, once");
    test_check(test, count_sockets(sockets, SOCKET_ESTABLISHED, port, process_id()) == 2, "two accepted connections");
    for (size_t i = 0; i < sockets.count; i++) {
        if (sockets.items[i].state == SOCKET_LISTEN && sockets.items[i].local_port == port) {
            test_check_str(test, sockets.items[i].local_host, S("127.0.0.1"), "the address it listens on");
            test_check(test, sockets.items[i].user_id == process_user_id(), "owned by us");
        }
    }

    int dual;
    String dual_address;
    test_check(test, net_listen(S(":0"), &dual, &err) == ERR_OK, "listen on every interface");
    test_check(test, net_local_address(a, dual, &dual_address, &err) == ERR_OK, "name it");
    test_check(test, sockets_list(a, &sockets, &err) == ERR_OK, "list sockets again");
    bool every_interface = false;
    for (size_t i = 0; i < sockets.count; i++) {
        const SocketEntry *entry = &sockets.items[i];
        every_interface = every_interface || (entry->state == SOCKET_LISTEN && entry->pid == process_id() &&
                                              entry->local_port == port_of(dual_address) &&
                                              (str_equal(entry->local_host, S("::")) ||
                                               str_equal(entry->local_host, S("0.0.0.0"))));
    }
    test_check(test, every_interface, "a listener on every interface");
    for (size_t i = 0; i < sockets.count; i++) {
        const SocketEntry *entry = &sockets.items[i];
        if (entry->pid == process_id() && entry->local_port == port_of(dual_address) && str_contains(dual_address, S("::"))) {
            test_check_str(test, entry->local_host, S("::"), "a dual-stack listener is IPv6's every interface");
        }
    }
    net_close(dual);

    for (size_t i = 0; i < countof(clients); i++) {
        net_close(clients[i]);
        net_close(accepted[i]);
    }
    net_close(listener);
}

static void process_info_describes_a_group_that_can_be_killed(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    const char *const argv[] = { "sh", "-c", "sleep 30 & sleep 30; :" };
    Pty pty;
    test_check(test, pty_spawn(a, strlist_of(a, countof(argv), argv), S(""), (StringList) { 0 },
                               (PtySize) { .columns = 80, .rows = 24 }, &pty, &err) == ERR_OK,
               "spawn a group");
    int group = pty.pid;
    if (group <= 0) {
        return;
    }
    ProcessInfo info = { 0 };
    int64_t deadline = clock_monotonic_ns() + 5 * (int64_t)NS_PER_SECOND;
    while (info.arguments.count < 3 && clock_monotonic_ns() < deadline) {
        test_check(test, process_info(a, pty.pid, &info, &err) == ERR_OK, "describe the shell");
        clock_sleep_ms(10);
    }
    test_check(test, info.pid == pty.pid && info.group_id == pty.pid, "the shell leads its group");
    test_check(test, info.parent_pid == process_id(), "we started it");
    test_check(test, info.user_id == process_user_id(), "as us");
    test_check(test, info.arguments.count == 3 && str_equal(info.arguments.items[1], S("-c")), "its command line");
    test_check(test, info.name.len > 0 && info.working_directory.len > 0, "its name and directory");

    test_check(test, process_signal_group(group, SIGNAL_KILL, &err) == ERR_OK, "kill the group");
    int exit_code;
    int signal;
    while (!pty_reap(&pty, &exit_code, &signal) && clock_monotonic_ns() < deadline) {
        clock_sleep_ms(10);
    }
    test_check(test, signal == 9, "killed");
    pty_close(&pty);
    // The orphaned sleeps linger as zombies until the system reaps them.
    Error signalled = ERR_OK;
    while ((signalled = process_signal_group(group, SIGNAL_KILL, &err)) == ERR_OK &&
           clock_monotonic_ns() < deadline) {
        clock_sleep_ms(10);
    }
    test_check(test, signalled == ERR_NOT_FOUND, "the group is gone");
    test_check(test, process_signal_group(0, SIGNAL_STOP, &err) == ERR_INVALID_ARGUMENT, "never our own group");
    test_check(test, process_signal(-1, SIGNAL_STOP, &err) == ERR_INVALID_ARGUMENT, "never every process");
    test_check(test, process_info(a, group, &info, &err) == ERR_NOT_FOUND, "a gone process is ERR_NOT_FOUND");
}

static void process_open_files_lists_a_file_we_hold(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String path;
    int fd;
    test_check(test, file_create_unique(a, S("/tmp/libmc-procinfo-æøå-XXXXXX"), &path, &fd, &err) == ERR_OK,
               "create a file");
    StringList paths;
    test_check(test, process_open_files(a, process_id(), &paths, &err) == ERR_OK, "list open files");
    size_t slash;
    test_check(test, str_find_last_char(path, '/', &slash), "the file has a name");
    String name = str_slice(path, slash, path.len);
    bool found = false;
    for (size_t i = 0; i < paths.count; i++) {
        found = found || str_ends_with(paths.items[i], name);
    }
    test_check(test, found, "the file is listed");
    file_close(fd);
    (void)file_remove(path, &err);
}

static void user_name_names_us(Test *test)
{
    test_check(test, user_name(test->arena, process_user_id()).len > 0, "our account has a name");
    test_check_str(test, user_name(test->arena, 0x7ffffff0), S("2147483632"), "an unknown user is a number");
}

const TestCase PROCINFO_TESTS[] = {
    { "sockets_list_finds_our_listener_and_its_connections", sockets_list_finds_our_listener_and_its_connections },
    { "process_info_describes_a_group_that_can_be_killed", process_info_describes_a_group_that_can_be_killed },
    { "process_open_files_lists_a_file_we_hold", process_open_files_lists_a_file_we_hold },
    { "user_name_names_us", user_name_names_us },
    { nullptr, nullptr },
};
