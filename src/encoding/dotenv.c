#include "mc/encoding/dotenv.h"

static bool is_name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_name_char(char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

// rest_is_comment reports whether what follows a closing quote is only blanks and a comment.
static bool rest_is_comment(String rest)
{
    rest = str_trim(rest);
    return rest.len == 0 || rest.data[0] == '#';
}

[[nodiscard]] static Error invalid(Err *err, size_t line, const char *problem)
{
    return err_set(err, ERR_PARSE, "dotenv: line %zu: %s", line, problem);
}

[[nodiscard]] static Error read_double_quoted(Arena *arena, String value, size_t line, String *decoded, Err *err)
{
    StringBuilder out = str_builder_create(arena, value.len);
    for (size_t i = 1; i < value.len; i++) {
        char c = value.data[i];
        if (c == '"') {
            if (!rest_is_comment(str_slice(value, i + 1, value.len))) {
                return invalid(err, line, "text after the closing quote");
            }
            *decoded = str_builder_finish(&out);
            return ERR_OK;
        }
        if (c == '\\' && i + 1 < value.len) {
            char next = value.data[++i];
            c = next == 'n' ? '\n' : next == 't' ? '\t' : next;
            if (next != 'n' && next != 't' && next != '"' && next != '\\' && next != '$') {
                return invalid(err, line, "unknown escape in double quotes");
            }
        }
        str_builder_append_char(&out, c);
    }
    return invalid(err, line, "missing closing quote");
}

[[nodiscard]] static Error read_value(Arena *arena, String value, size_t line, String *result, Err *err)
{
    if (value.len > 0 && value.data[0] == '\'') {
        size_t close;
        String inside = str_slice(value, 1, value.len);
        if (!str_find_char(inside, '\'', &close)) {
            return invalid(err, line, "missing closing quote");
        }
        if (!rest_is_comment(str_slice(inside, close + 1, inside.len))) {
            return invalid(err, line, "text after the closing quote");
        }
        *result = str_slice(inside, 0, close);
        return ERR_OK;
    }
    if (value.len > 0 && value.data[0] == '"') {
        return read_double_quoted(arena, value, line, result, err);
    }
    for (size_t i = 1; i < value.len; i++) {
        if (value.data[i] == '#' && is_blank(value.data[i - 1])) {
            value = str_slice(value, 0, i);
            break;
        }
    }
    *result = str_trim(value);
    return ERR_OK;
}

Error dotenv_parse(Arena *arena, String text, StringList *entries, Err *err)
{
    StringList lines = str_split(arena, text, '\n');
    for (size_t n = 0; n < lines.count; n++) {
        size_t line = n + 1;
        String rest = str_trim(str_trim_suffix(lines.items[n], S("\r")));
        if (rest.len == 0 || rest.data[0] == '#') {
            continue;
        }
        if (str_starts_with(rest, S("export")) && rest.len > 6 && is_blank(rest.data[6])) {
            rest = str_trim(str_slice(rest, 6, rest.len));
        }
        size_t end = 0;
        while (end < rest.len && is_name_char(rest.data[end])) {
            end++;
        }
        String name = str_slice(rest, 0, end);
        rest = str_trim(str_slice(rest, end, rest.len));
        if (name.len == 0 || !is_name_start(name.data[0]) || rest.len == 0 || rest.data[0] != '=') {
            return invalid(err, line, "expected NAME=value");
        }
        String value;
        Error e = read_value(arena, str_trim(str_slice(rest, 1, rest.len)), line, &value, err);
        if (e != ERR_OK) {
            return e;
        }
        strlist_push(arena, entries,
                     str_format(arena, "%.*s=%.*s", (int)name.len, name.data, (int)value.len, value.data));
    }
    return ERR_OK;
}
