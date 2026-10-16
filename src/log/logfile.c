#include "mc/log/logfile.h"

#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"

struct LogFile {
    Arena *arena;
    String path;
    size_t limit;
    int fd; // -1 until the first line
    size_t size;
};

LogFile *logfile_create(String path, size_t limit)
{
    Arena *arena = arena_create(0);
    LogFile *file = arena_push(arena, sizeof *file);
    file->arena = arena;
    file->path = str_copy(arena, path);
    file->limit = limit;
    file->fd = -1;
    return file;
}

void logfile_destroy(LogFile *file)
{
    if (file == nullptr) {
        return;
    }
    if (file->fd >= 0) {
        file_close(file->fd);
    }
    arena_destroy(file->arena);
}

[[nodiscard]] static Error open_file(LogFile *file, Err *err)
{
    Error e = dir_create_all(path_dir(file->path), 0755, err);
    if (e != ERR_OK) {
        return e;
    }
    e = file_open_append(file->path, &file->fd, err);
    if (e == ERR_NOT_FOUND) {
        e = file_create(file->path, 0644, &file->fd, err);
    }
    FileInfo info = { 0 };
    if (e == ERR_OK) {
        e = file_info_fd(file->fd, &info, err);
    }
    file->size = info.size > 0 ? (size_t)info.size : 0;
    return e;
}

[[nodiscard]] static Error rotate(LogFile *file, Arena *scratch, Err *err)
{
    file_close(file->fd);
    file->fd = -1;
    file->size = 0;
    Error e = file_rename(file->path, str_concat(scratch, file->path, S(".1")), err);
    if (e == ERR_OK) {
        e = file_create(file->path, 0644, &file->fd, err);
    }
    return e;
}

Error logfile_write(LogFile *file, Arena *scratch, String line, int64_t wall_ns, Err *err)
{
    if (file->fd < 0) {
        Error e = open_file(file, err);
        if (e != ERR_OK) {
            return e;
        }
    }
    ArenaMark mark = arena_mark(scratch);
    String time = fmt_rfc3339(scratch, wall_ns, clock_local_offset(wall_ns / NS_PER_SECOND), true);
    String text = str_format(scratch, "%.*s %.*s\n", (int)time.len, time.data, (int)line.len, line.data);
    Error e = ERR_OK;
    if (file->size > 0 && file->size + text.len > file->limit) {
        e = rotate(file, scratch, err);
    }
    if (e == ERR_OK) {
        e = file_write(file->fd, text, err);
        file->size += text.len;
    }
    arena_release(mark);
    return e;
}
