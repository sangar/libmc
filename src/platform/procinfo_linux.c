#ifdef __linux__
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mc/container/sort.h"
#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/platform/procinfo.h"

// Sockets come from /proc/net, which names each by its inode; the processes
// holding them come from the "socket:[inode]" links under /proc/<pid>/fd.

typedef struct InodeOwner {
    uint64_t inode;
    int pid;
} InodeOwner;

typedef struct InodeOwners {
    InodeOwner *items;
    size_t count;
} InodeOwners;

static int compare_owners(const void *context, const void *a, const void *b)
{
    const InodeOwner *left = a;
    const InodeOwner *right = b;
    return (left->inode > right->inode) - (left->inode < right->inode);
}

static String read_link(Arena *arena, String path)
{
    char target[PATH_MAX];
    ssize_t length = readlink(str_cstr(arena, path), target, sizeof target);
    return length > 0 ? str_copy(arena, (String){ target, (size_t)length }) : (String) { 0 };
}

static void add_owners(Arena *arena, Arena *scratch, String pid_text, InodeOwners *owners, size_t *capacity)
{
    int64_t pid;
    if (!str_parse_i64(pid_text, &pid)) {
        return;
    }
    ArenaMark mark = arena_mark(scratch);
    String directory = str_format(scratch, "/proc/%.*s/fd", (int)pid_text.len, pid_text.data);
    StringList fds = { 0 };
    if (dir_list(scratch, directory, &fds, nullptr) == ERR_OK) {
        for (size_t i = 0; i < fds.count; i++) {
            String target = read_link(scratch, str_format(scratch, "%.*s/%.*s", (int)directory.len, directory.data,
                                                         (int)fds.items[i].len, fds.items[i].data));
            uint64_t inode;
            if (!str_starts_with(target, S("socket:[")) || !str_ends_with(target, S("]")) ||
                !str_parse_u64(str_slice(target, 8, target.len - 1), &inode)) {
                continue;
            }
            owners->items = arena_grow(arena, owners->items, capacity, owners->count, sizeof *owners->items);
            owners->items[owners->count++] = (InodeOwner){ .inode = inode, .pid = (int)pid };
        }
    }
    arena_release(mark);
}

// list_owners maps socket inodes to the readable processes holding them,
// sorted by inode, each pair once.
static InodeOwners list_owners(Arena *arena)
{
    InodeOwners owners = { 0 };
    size_t capacity = 0;
    Arena *scratch = arena_create(0);
    StringList names = { 0 };
    if (dir_list(scratch, S("/proc"), &names, nullptr) == ERR_OK) {
        for (size_t i = 0; i < names.count; i++) {
            if (str_is_digits(names.items[i])) {
                add_owners(arena, scratch, names.items[i], &owners, &capacity);
            }
        }
    }
    sort_stable(scratch, owners.items, owners.count, sizeof *owners.items, compare_owners, nullptr);
    size_t kept = 0;
    for (size_t i = 0; i < owners.count; i++) {
        if (kept == 0 || owners.items[kept - 1].inode != owners.items[i].inode ||
            owners.items[kept - 1].pid != owners.items[i].pid) {
            owners.items[kept++] = owners.items[i];
        }
    }
    owners.count = kept;
    arena_destroy(scratch);
    return owners;
}

// first_owner is the index of the first pair for inode, or owners.count.
static size_t first_owner(InodeOwners owners, uint64_t inode)
{
    size_t low = 0;
    size_t high = owners.count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (owners.items[middle].inode < inode) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

static bool parse_hex(String text, uint64_t *value)
{
    if (text.len == 0 || text.len > 16) {
        return false;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < text.len; i++) {
        char c = text.data[i];
        unsigned digit;
        if (c >= '0' && c <= '9') { // modern-c: allow ascii-text
            digit = (unsigned)(c - '0');
        } else if (c >= 'A' && c <= 'F') { // modern-c: allow ascii-text
            digit = (unsigned)(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') { // modern-c: allow ascii-text
            digit = (unsigned)(c - 'a' + 10);
        } else {
            return false;
        }
        result = result << 4 | digit;
    }
    *value = result;
    return true;
}

// parse_endpoint reads "0100007F:1F90": the address as the kernel's 32-bit
// words in this machine's byte order, then the port.
static bool parse_endpoint(Arena *arena, String text, String *host, int *port)
{
    String address;
    String port_text;
    uint64_t port_value;
    if (!str_cut(text, ':', &address, &port_text) || !parse_hex(port_text, &port_value) || port_value > 65535 ||
        (address.len != 8 && address.len != 32)) {
        return false;
    }
    unsigned char bytes[16];
    for (size_t word = 0; word < address.len / 8; word++) {
        uint64_t value;
        if (!parse_hex(str_slice(address, word * 8, word * 8 + 8), &value)) {
            return false;
        }
        uint32_t native = (uint32_t)value;
        memcpy(bytes + word * 4, &native, sizeof native);
    }
    char buffer[INET6_ADDRSTRLEN] = { 0 };
    inet_ntop(address.len == 8 ? AF_INET : AF_INET6, bytes, buffer, sizeof buffer);
    *host = str_copy(arena, S(buffer));
    *port = (int)port_value;
    return true;
}

enum { TCP_STATE_ESTABLISHED = 0x01, TCP_STATE_LISTEN = 0x0A };

static void add_table(Arena *arena, String path, SocketProtocol protocol, InodeOwners owners, SocketEntry **items,
                      size_t *capacity, size_t *count)
{
    String text;
    if (file_read_all(arena, path, &text, nullptr) != ERR_OK) {
        return;
    }
    StringList lines = strlist_from_lines(arena, text);
    // After the header: sl local_address rem_address st tx_queue:rx_queue
    // tr:tm->when retrnsmt uid timeout inode
    for (size_t i = 1; i < lines.count; i++) {
        StringList fields = str_fields(arena, lines.items[i]);
        SocketEntry entry = { .protocol = protocol };
        uint64_t state;
        uint64_t inode;
        int64_t user_id;
        if (fields.count < 10 || !parse_endpoint(arena, fields.items[1], &entry.local_host, &entry.local_port) ||
            !parse_endpoint(arena, fields.items[2], &entry.remote_host, &entry.remote_port) ||
            !parse_hex(fields.items[3], &state) || !str_parse_i64(fields.items[7], &user_id) ||
            !str_parse_u64(fields.items[9], &inode)) {
            continue;
        }
        entry.user_id = (int)user_id;
        if (protocol == SOCKET_UDP) {
            entry.state = entry.remote_port == 0 ? SOCKET_LISTEN : SOCKET_ESTABLISHED;
        } else {
            entry.state = state == TCP_STATE_LISTEN        ? SOCKET_LISTEN
                          : state == TCP_STATE_ESTABLISHED ? SOCKET_ESTABLISHED
                                                           : SOCKET_OTHER;
        }
        size_t at = first_owner(owners, inode);
        bool owned = inode != 0 && at < owners.count && owners.items[at].inode == inode;
        do {
            entry.pid = owned ? owners.items[at].pid : 0;
            *items = arena_grow(arena, *items, capacity, *count, sizeof **items);
            (*items)[(*count)++] = entry;
            at++;
        } while (owned && at < owners.count && owners.items[at].inode == inode);
    }
}

Error sockets_list(Arena *arena, SocketList *sockets, Err *err)
{
    *sockets = (SocketList) { 0 };
    if (!file_exists(S("/proc/net/tcp"))) {
        return err_set(err, ERR_PLATFORM, "read /proc/net/tcp: procfs is not mounted");
    }
    InodeOwners owners = list_owners(arena);
    static const struct {
        const char *path;
        SocketProtocol protocol;
    } TABLES[] = {
        { "/proc/net/tcp", SOCKET_TCP },
        { "/proc/net/tcp6", SOCKET_TCP },
        { "/proc/net/udp", SOCKET_UDP },
        { "/proc/net/udp6", SOCKET_UDP },
    };
    size_t capacity = 0;
    for (size_t i = 0; i < countof(TABLES); i++) {
        add_table(arena, S(TABLES[i].path), TABLES[i].protocol, owners, &sockets->items, &capacity, &sockets->count);
    }
    return ERR_OK;
}

Error process_info(Arena *arena, int pid, ProcessInfo *info, Err *err)
{
    *info = (ProcessInfo) { .pid = pid };
    String stat_text;
    size_t open;
    size_t close;
    if (file_read_all(arena, str_format(arena, "/proc/%d/stat", pid), &stat_text, nullptr) != ERR_OK ||
        !str_find_char(stat_text, '(', &open) || !str_find_last_char(stat_text, ')', &close) || close < open) {
        return err_set(err, ERR_NOT_FOUND, "process %d is gone", pid);
    }
    // After "pid (comm) ": state ppid pgrp
    StringList fields = str_fields(arena, str_slice(stat_text, close + 1, stat_text.len));
    int64_t parent = 0;
    int64_t group = 0;
    if (fields.count < 3 || !str_parse_i64(fields.items[1], &parent) || !str_parse_i64(fields.items[2], &group)) {
        return err_set(err, ERR_PARSE, "read /proc/%d/stat: too few fields", pid);
    }
    info->name = str_slice(stat_text, open + 1, close);
    info->parent_pid = (int)parent;
    info->group_id = (int)group;
    struct stat status;
    if (stat(str_cstr(arena, str_format(arena, "/proc/%d", pid)), &status) == 0) {
        info->user_id = (int)status.st_uid;
    }
    String command_line;
    if (file_read_all(arena, str_format(arena, "/proc/%d/cmdline", pid), &command_line, nullptr) == ERR_OK &&
        command_line.len > 0) {
        info->arguments = str_split(arena, str_trim_suffix(command_line, (String){ "", 1 }), '\0');
    }
    info->working_directory = read_link(arena, str_format(arena, "/proc/%d/cwd", pid));
    return ERR_OK;
}

Error process_open_files(Arena *arena, int pid, StringList *paths, Err *err)
{
    *paths = (StringList) { 0 };
    if (!process_alive(pid)) {
        return err_set(err, ERR_NOT_FOUND, "process %d is gone", pid);
    }
    Arena *scratch = arena_create(0);
    String directory = str_format(scratch, "/proc/%d/fd", pid);
    StringList fds = { 0 };
    if (dir_list(scratch, directory, &fds, nullptr) == ERR_OK) {
        for (size_t i = 0; i < fds.count; i++) {
            String target = read_link(scratch, str_format(scratch, "%.*s/%.*s", (int)directory.len, directory.data,
                                                         (int)fds.items[i].len, fds.items[i].data));
            if (str_starts_with(target, S("/"))) {
                strlist_push(arena, paths, str_copy(arena, target));
            }
        }
    }
    arena_destroy(scratch);
    return ERR_OK;
}

#endif
