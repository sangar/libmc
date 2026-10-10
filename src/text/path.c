#include "mc/text/path.h"

bool path_is_absolute(String path)
{
    return path.len > 0 && path.data[0] == '/';
}

static String trim_trailing_slashes(String path)
{
    while (path.len > 1 && path.data[path.len - 1] == '/') {
        path.len--;
    }
    return path;
}

String path_base(String path)
{
    if (path.len == 0) {
        return S(".");
    }
    path = trim_trailing_slashes(path);
    size_t slash;
    if (path.len > 1 && str_find_last_char(path, '/', &slash)) {
        return str_slice(path, slash + 1, path.len);
    }
    return path;
}

String path_dir(String path)
{
    size_t slash;
    if (!str_find_last_char(path, '/', &slash)) {
        return S(".");
    }
    return slash == 0 ? S("/") : str_slice(path, 0, slash);
}

String path_ext(String path)
{
    size_t slash;
    String base = str_find_last_char(path, '/', &slash) ? str_slice(path, slash + 1, path.len) : path;
    size_t dot;
    return str_find_last_char(base, '.', &dot) ? str_slice(base, dot, base.len) : str_slice(base, base.len, base.len);
}

bool path_relative(String root, String path, String *relative)
{
    root = trim_trailing_slashes(root);
    if (!str_starts_with(path, root)) {
        return false;
    }
    String rest = str_slice(path, root.len, path.len);
    if (rest.len == 0 || (rest.data[0] != '/' && !str_equal(root, S("/")))) {
        return false;
    }
    while (rest.len > 0 && rest.data[0] == '/') {
        rest = str_slice(rest, 1, rest.len);
    }
    if (rest.len == 0) {
        return false;
    }
    *relative = rest;
    return true;
}

String path_join(Arena *arena, String dir, String name)
{
    if (dir.len == 0) {
        return str_copy(arena, name);
    }
    StringBuilder builder = str_builder_create(arena, dir.len + name.len + 1);
    str_builder_append(&builder, trim_trailing_slashes(dir));
    if (!str_equal(trim_trailing_slashes(dir), S("/"))) {
        str_builder_append_char(&builder, '/');
    }
    str_builder_append(&builder, str_trim_prefix(name, S("/")));
    return str_builder_finish(&builder);
}

static bool element_is(String path, size_t at, String element)
{
    String rest = str_slice(path, at, path.len);
    return str_starts_with(rest, element) && (rest.len == element.len || rest.data[element.len] == '/');
}

String path_clean(Arena *arena, String path)
{
    if (path.len == 0) {
        return str_copy(arena, S("."));
    }
    bool rooted = path.data[0] == '/';
    char *out = arena_push_aligned(arena, arena_size_add(path.len, 2), 1);
    size_t written = 0;
    size_t backtrack_limit = 0;
    size_t read = 0;
    if (rooted) {
        out[written++] = '/';
        backtrack_limit = 1;
        read = 1;
    }
    while (read < path.len) {
        if (path.data[read] == '/') {
            read++;
        } else if (element_is(path, read, S("."))) {
            read++;
        } else if (element_is(path, read, S(".."))) {
            read += 2;
            if (written > backtrack_limit) {
                written--;
                while (written > backtrack_limit && out[written] != '/') {
                    written--;
                }
            } else if (!rooted) {
                if (written > 0) {
                    out[written++] = '/';
                }
                out[written++] = '.';
                out[written++] = '.';
                backtrack_limit = written;
            }
        } else {
            if (written != (rooted ? 1 : 0)) {
                out[written++] = '/';
            }
            while (read < path.len && path.data[read] != '/') {
                out[written++] = path.data[read++];
            }
        }
    }
    if (written == 0) {
        out[written++] = '.';
    }
    out[written] = '\0';
    return (String){ .data = out, .len = written };
}

String path_expand_home(Arena *arena, String path, String home)
{
    if (str_equal(path, S("~"))) {
        return str_copy(arena, home);
    }
    if (str_starts_with(path, S("~/"))) {
        return path_join(arena, home, str_slice(path, 2, path.len));
    }
    return str_copy(arena, path);
}
