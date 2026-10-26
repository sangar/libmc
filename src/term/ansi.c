#include "mc/term/ansi.h"

#include <stdint.h>

#include "mc/text/utf8.h"

enum { ESC = 0x1b, BEL = 0x07 };

size_t ansi_escape_end(String text, size_t start)
{
    size_t at = start + 1;
    if (at >= text.len) {
        return at;
    }
    char kind = text.data[at++];
    if (kind == '[') {
        while (at < text.len) {
            unsigned char c = (unsigned char)text.data[at++];
            if (c >= 0x40 && c <= 0x7e) {
                return at;
            }
        }
        return at;
    }
    if (kind == ']') {
        while (at < text.len) {
            char c = text.data[at++];
            if (c == BEL) {
                return at;
            }
            if (c == ESC && at < text.len && text.data[at] == '\\') {
                return at + 1;
            }
        }
    }
    return at;
}

// fit is the offset of the first character that would take text past columns,
// with the columns taken before it in *used.
static size_t fit(String text, size_t columns, size_t *used)
{
    size_t position = 0;
    *used = 0;
    while (position < text.len) {
        if (text.data[position] == ESC) {
            position = ansi_escape_end(text, position);
            continue;
        }
        size_t next = position;
        uint32_t codepoint;
        utf8_decode(text, &next, &codepoint);
        size_t width = (size_t)utf8_codepoint_width(codepoint);
        if (*used + width > columns) {
            break;
        }
        *used += width;
        position = next;
    }
    return position;
}

size_t ansi_width(String text)
{
    size_t used;
    fit(text, SIZE_MAX, &used);
    return used;
}

String ansi_truncate(String text, size_t columns)
{
    size_t used;
    return str_slice(text, 0, fit(text, columns, &used));
}

String ansi_strip(Arena *arena, String text)
{
    StringBuilder plain = str_builder_create(arena, text.len + 1);
    size_t position = 0;
    while (position < text.len) {
        size_t escape;
        if (!str_find_char(str_slice(text, position, text.len), ESC, &escape)) {
            escape = text.len - position;
        }
        str_builder_append(&plain, str_slice(text, position, position + escape));
        position += escape;
        if (position < text.len) {
            position = ansi_escape_end(text, position);
        }
    }
    return str_builder_finish(&plain);
}
