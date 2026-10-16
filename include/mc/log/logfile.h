#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// LogFile appends lines to a file, each after its local time as
// 2024-03-13T10:00:00.250+01:00. A file that would pass its limit is renamed
// to path.1, replacing the one before, and a new file is started. The file
// and its directory are made at the first line.
typedef struct LogFile LogFile;

LogFile *logfile_create(String path, size_t limit);
void logfile_destroy(LogFile *file);

// logfile_write adds line, which holds no newline, written at wall_ns.
[[nodiscard]] Error logfile_write(LogFile *file, Arena *scratch, String line, int64_t wall_ns, Err *err);
