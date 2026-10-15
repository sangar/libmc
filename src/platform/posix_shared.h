#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// What posix.c shares with the other POSIX files of the platform layer.

// posix_fail_errno records the message followed by the description of errno, and maps errno to an Error.
[[nodiscard]] [[gnu::format(printf, 2, 3)]] Error posix_fail_errno(Err *err, const char *format, ...);
// posix_open_pipe keeps both ends out of every child; spawning dup2s the child's end to a standard stream.
bool posix_open_pipe(int fds[2]);
bool posix_make_nonblocking(int fd);
// posix_look_path finds name in the directories of path_list, a value of PATH.
bool posix_look_path(Arena *arena, String name, String path_list, String *path);
