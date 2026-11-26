#ifdef __APPLE__

#include <arpa/inet.h>
#include <errno.h>
#include <libproc.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/platform/procinfo.h"
#include "posix_shared.h"

// Spare slots for what starts between asking for a count and reading the list.
enum { LIST_SPARE = 16 };

static pid_t *list_pids(Arena *arena, size_t *count)
{
    *count = 0;
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0) {
        return nullptr;
    }
    size_t capacity = (size_t)bytes / sizeof(pid_t) + LIST_SPARE;
    pid_t *pids = arena_push(arena, arena_size_mul(capacity, sizeof *pids));
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)(capacity * sizeof *pids));
    *count = bytes > 0 ? (size_t)bytes / sizeof(pid_t) : 0;
    return pids;
}

// list_fds is empty for a process this user cannot read.
static struct proc_fdinfo *list_fds(Arena *arena, pid_t pid, size_t *count)
{
    *count = 0;
    int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0) {
        return nullptr;
    }
    size_t capacity = (size_t)bytes / sizeof(struct proc_fdinfo) + LIST_SPARE;
    struct proc_fdinfo *fds = arena_push(arena, arena_size_mul(capacity, sizeof *fds));
    bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, (int)(capacity * sizeof *fds));
    *count = bytes > 0 ? (size_t)bytes / sizeof(struct proc_fdinfo) : 0;
    return fds;
}

static String address_text(Arena *arena, const struct in_sockinfo *in, bool local)
{
    char text[INET6_ADDRSTRLEN] = { 0 };
    // A dual-stack IPv6 socket sets both flags; only a socket that is IPv4
    // alone, or an IPv6 one carrying an IPv4 connection, has an IPv4 address.
    if (!(in->insi_vflag & INI_IPV6)) {
        const struct in_addr *address = local ? &in->insi_laddr.ina_46.i46a_addr4 : &in->insi_faddr.ina_46.i46a_addr4;
        inet_ntop(AF_INET, address, text, sizeof text);
    } else {
        const struct in6_addr *address = local ? &in->insi_laddr.ina_6 : &in->insi_faddr.ina_6;
        inet_ntop(AF_INET6, address, text, sizeof text);
    }
    return str_copy(arena, S(text));
}

static int port_of(int network_order)
{
    return ntohs((uint16_t)network_order);
}

// read_socket fills entry from the socket at fd of pid and reports whether it
// is a TCP or UDP socket over IP. *kernel_socket tells sockets apart.
static bool read_socket(Arena *arena, pid_t pid, int fd, SocketEntry *entry, uint64_t *kernel_socket)
{
    struct socket_fdinfo info;
    if (proc_pidfdinfo(pid, fd, PROC_PIDFDSOCKETINFO, &info, sizeof info) != (int)sizeof info) {
        return false;
    }
    const struct socket_info *socket = &info.psi;
    if (socket->soi_family != AF_INET && socket->soi_family != AF_INET6) {
        return false;
    }
    const struct in_sockinfo *in;
    if (socket->soi_kind == SOCKINFO_TCP) {
        in = &socket->soi_proto.pri_tcp.tcpsi_ini;
        int state = socket->soi_proto.pri_tcp.tcpsi_state;
        entry->protocol = SOCKET_TCP;
        entry->state = state == TSI_S_LISTEN        ? SOCKET_LISTEN
                       : state == TSI_S_ESTABLISHED ? SOCKET_ESTABLISHED
                                                    : SOCKET_OTHER;
    } else if (socket->soi_kind == SOCKINFO_IN && socket->soi_type == SOCK_DGRAM) {
        in = &socket->soi_proto.pri_in;
        entry->protocol = SOCKET_UDP;
        entry->state = in->insi_fport == 0 ? SOCKET_LISTEN : SOCKET_ESTABLISHED;
    } else {
        return false;
    }
    entry->local_host = address_text(arena, in, true);
    entry->local_port = port_of(in->insi_lport);
    entry->remote_host = address_text(arena, in, false);
    entry->remote_port = port_of(in->insi_fport);
    entry->pid = pid;
    *kernel_socket = socket->soi_so;
    return true;
}

static bool seen_before(const uint64_t *sockets, size_t count, uint64_t socket)
{
    for (size_t i = 0; i < count; i++) {
        if (sockets[i] == socket) {
            return true;
        }
    }
    return false;
}

static int user_of(pid_t pid)
{
    struct proc_bsdshortinfo bsd;
    if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &bsd, sizeof bsd) != (int)sizeof bsd) {
        return -1;
    }
    return (int)bsd.pbsi_uid;
}

// add_sockets appends the sockets of pid, each once however many of its
// descriptors share it.
static void add_sockets(Arena *arena, Arena *scratch, pid_t pid, SocketEntry **items, size_t *capacity, size_t *count)
{
    ArenaMark mark = arena_mark(scratch);
    size_t fd_count;
    struct proc_fdinfo *fds = list_fds(scratch, pid, &fd_count);
    uint64_t *seen = arena_push(scratch, arena_size_mul(fd_count + 1, sizeof *seen));
    size_t seen_count = 0;
    int user_id = -1;
    for (size_t i = 0; i < fd_count; i++) {
        if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) {
            continue;
        }
        SocketEntry entry = { 0 };
        uint64_t socket;
        if (!read_socket(arena, pid, fds[i].proc_fd, &entry, &socket) || seen_before(seen, seen_count, socket)) {
            continue;
        }
        seen[seen_count++] = socket;
        if (user_id < 0) {
            user_id = user_of(pid);
        }
        entry.user_id = user_id;
        *items = arena_grow(arena, *items, capacity, *count, sizeof **items);
        (*items)[(*count)++] = entry;
    }
    arena_release(mark);
}

Error sockets_list(Arena *arena, SocketList *sockets, Err *err)
{
    *sockets = (SocketList) { 0 };
    Arena *scratch = arena_create(0);
    size_t pid_count;
    pid_t *pids = list_pids(scratch, &pid_count);
    if (pid_count == 0) {
        arena_destroy(scratch);
        return posix_fail_errno(err, "list processes");
    }
    size_t capacity = 0;
    for (size_t i = 0; i < pid_count; i++) {
        if (pids[i] > 0) {
            add_sockets(arena, scratch, pids[i], &sockets->items, &capacity, &sockets->count);
        }
    }
    arena_destroy(scratch);
    return ERR_OK;
}

// read_arguments splits KERN_PROCARGS2: the argument count, the executable's
// path, padding, then the arguments, each ending in a NUL.
static StringList read_arguments(Arena *arena, pid_t pid)
{
    StringList found = { 0 };
    int max_mib[2] = { CTL_KERN, KERN_ARGMAX };
    int max = 0;
    size_t size = sizeof max;
    if (sysctl(max_mib, 2, &max, &size, nullptr, 0) != 0 || max <= 0) {
        return found;
    }
    // The buffer is KERN_ARGMAX, a megabyte, so it is not left in arena.
    Arena *scratch = arena_create(0);
    char *buffer = arena_push(scratch, (size_t)max);
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, pid };
    size = (size_t)max;
    int count = 0;
    if (sysctl(mib, 3, buffer, &size, nullptr, 0) != 0 || size < sizeof count) {
        arena_destroy(scratch);
        return found;
    }
    memcpy(&count, buffer, sizeof count);
    size_t at = sizeof count;
    while (at < size && buffer[at] != '\0') {
        at++;
    }
    while (at < size && buffer[at] == '\0') {
        at++;
    }
    for (int i = 0; i < count && at < size; i++) {
        size_t start = at;
        while (at < size && buffer[at] != '\0') {
            at++;
        }
        strlist_push(arena, &found, str_copy(arena, (String){ buffer + start, at - start }));
        at++;
    }
    arena_destroy(scratch);
    return found;
}

Error process_info(Arena *arena, int pid, ProcessInfo *info, Err *err)
{
    *info = (ProcessInfo) { .pid = pid };
    struct kinfo_proc process;
    size_t size = sizeof process;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    if (sysctl(mib, 4, &process, &size, nullptr, 0) != 0) {
        return posix_fail_errno(err, "read process %d", pid);
    }
    if (size == 0) {
        return err_set(err, ERR_NOT_FOUND, "process %d is gone", pid);
    }
    info->parent_pid = process.kp_eproc.e_ppid;
    info->group_id = process.kp_eproc.e_pgid;
    info->user_id = (int)process.kp_eproc.e_ucred.cr_uid;
    char name[2 * MAXCOMLEN + 1] = { 0 };
    info->name = str_copy(arena, S(proc_name(pid, name, sizeof name) > 0 ? name : process.kp_proc.p_comm));
    info->arguments = read_arguments(arena, pid);
    struct proc_vnodepathinfo paths;
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &paths, sizeof paths) == (int)sizeof paths) {
        info->working_directory = str_copy(arena, S(paths.pvi_cdir.vip_path));
    }
    return ERR_OK;
}

Error process_open_files(Arena *arena, int pid, StringList *paths, Err *err)
{
    *paths = (StringList) { 0 };
    if (!process_alive(pid)) {
        return err_set(err, ERR_NOT_FOUND, "process %d is gone", pid);
    }
    Arena *scratch = arena_create(0);
    size_t count;
    struct proc_fdinfo *fds = list_fds(scratch, pid, &count);
    for (size_t i = 0; i < count; i++) {
        struct vnode_fdinfowithpath vnode;
        if (fds[i].proc_fdtype == PROX_FDTYPE_VNODE &&
            proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDVNODEPATHINFO, &vnode, sizeof vnode) == (int)sizeof vnode &&
            vnode.pvip.vip_path[0] != '\0') {
            strlist_push(arena, paths, str_copy(arena, S(vnode.pvip.vip_path)));
        }
    }
    arena_destroy(scratch);
    return ERR_OK;
}

#endif
