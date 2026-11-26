#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// What lsof shows: the system's TCP and UDP sockets with the processes that
// hold them, and what a process is running, where and with which files open.
// A process of another user is only readable as root; what cannot be read is
// left out or left empty, never an error.

typedef enum SocketProtocol { SOCKET_TCP, SOCKET_UDP } SocketProtocol;

// SocketState is SOCKET_LISTEN for a listening TCP socket and for a UDP
// socket bound without a peer, SOCKET_ESTABLISHED for a connected one.
typedef enum SocketState { SOCKET_LISTEN, SOCKET_ESTABLISHED, SOCKET_OTHER } SocketState;

// SocketEntry is one socket held by one process; a socket that several
// processes hold, as after a fork, is one entry for each. Addresses are
// numeric, "0.0.0.0" or "::" for every interface, and ports are 0 when unset.
typedef struct SocketEntry {
    SocketProtocol protocol;
    SocketState state;
    String local_host;
    int local_port;
    String remote_host;
    int remote_port;
    // pid is 0 when the process cannot be read. Linux lists such sockets,
    // with the owner's user_id; macOS cannot see them.
    int pid;
    int user_id;
} SocketEntry;

typedef struct SocketList {
    SocketEntry *items;
    size_t count;
} SocketList;

// sockets_list lists every TCP and UDP socket, IPv4 and IPv6, in arena.
[[nodiscard]] Error sockets_list(Arena *arena, SocketList *sockets, Err *err);

typedef struct ProcessInfo {
    int pid;
    int parent_pid;
    int group_id;
    int user_id;
    // name is the program's name as the system keeps it, which Linux cuts
    // to 15 bytes.
    String name;
    // arguments is the command line, the program first; empty when unreadable.
    StringList arguments;
    // working_directory is empty when unreadable.
    String working_directory;
} ProcessInfo;

// process_info describes pid in arena; a process that is gone is ERR_NOT_FOUND.
[[nodiscard]] Error process_info(Arena *arena, int pid, ProcessInfo *info, Err *err);
// process_open_files lists the paths of the files and directories pid holds
// open, in arena; sockets and pipes are left out.
[[nodiscard]] Error process_open_files(Arena *arena, int pid, StringList *paths, Err *err);
// user_name is the account name of user_id, or the number when it has none.
String user_name(Arena *arena, int user_id);
