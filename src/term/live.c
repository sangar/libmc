#include "mc/term/live.h"

#include <assert.h>
#include <string.h>

#include "mc/term/ansi.h"

struct Live {
    Arena *arena;
    bool interactive;
    bool cursor_hidden;
    char *previous;
    size_t previous_len;
    size_t previous_capacity;
    size_t previous_columns;
};

Live *live_create(Arena *arena, bool interactive)
{
    Live *live = arena_push(arena, sizeof *live);
    *live = (Live){ .arena = arena, .interactive = interactive };
    return live;
}

// remember copies frame, growing the copy by doubling, so the arena holds at
// most twice the largest frame.
static void remember(Live *live, String frame)
{
    if (frame.len > live->previous_capacity) {
        live->previous_capacity = max_size(frame.len, live->previous_capacity * 2);
        live->previous = arena_push(live->arena, live->previous_capacity);
    }
    if (frame.len > 0) {
        memcpy(live->previous, frame.data, frame.len);
    }
    live->previous_len = frame.len;
}

static String previous_frame(const Live *live)
{
    return (String){ live->previous, live->previous_len };
}

static StringList split_lines(Arena *arena, String text)
{
    return text.len == 0 ? (StringList){ 0 } : str_split(arena, text, '\n');
}

// append_line erases the line before drawing it: erasing after would also
// erase the last column of a line that fills the width. A styled line ends
// with a reset, so its style cannot reach the lines drawn after it. Lines end
// in \r\n, as a terminal in raw mode does not turn \n into both.
static void append_line(StringBuilder *out, String line, size_t columns)
{
    String visible = ansi_truncate(line, columns);
    str_builder_append(out, S("\x1b[2K"));
    str_builder_append(out, visible);
    if (str_contains(visible, S("\x1b")) && !str_ends_with(visible, S("\x1b[0m"))) {
        str_builder_append(out, S("\x1b[0m"));
    }
    str_builder_append(out, S("\r\n"));
}

void live_render(Live *live, StringBuilder *out, String frame, size_t columns)
{
    assert(columns > 0);
    frame = str_trim_suffix(frame, S("\n"));
    if (!live->interactive) {
        remember(live, frame);
        return;
    }
    bool resized = columns != live->previous_columns;
    if (!resized && str_equal(frame, previous_frame(live))) {
        return;
    }
    StringList lines = split_lines(out->arena, frame);
    StringList previous = split_lines(out->arena, previous_frame(live));

    str_builder_append(out, S("\x1b[?2026h"));
    if (!live->cursor_hidden) {
        str_builder_append(out, S("\x1b[?25l"));
        live->cursor_hidden = true;
    }
    if (previous.count > 0) {
        str_builder_append_format(out, "\x1b[%zuF", previous.count);
    }
    size_t unchanged = 0;
    for (size_t i = 0; i < lines.count; i++) {
        if (!resized && i < previous.count && str_equal(lines.items[i], previous.items[i])) {
            unchanged++;
            continue;
        }
        if (unchanged > 0) {
            str_builder_append_format(out, "\x1b[%zuE", unchanged);
            unchanged = 0;
        }
        append_line(out, lines.items[i], columns);
    }
    if (unchanged > 0) {
        str_builder_append_format(out, "\x1b[%zuE", unchanged);
    }
    if (lines.count < previous.count) {
        str_builder_append(out, S("\x1b[J"));
    }
    str_builder_append(out, S("\x1b[?2026l"));
    live->previous_columns = columns;
    remember(live, frame);
}

static void forget(Live *live)
{
    live->cursor_hidden = false;
    live->previous_len = 0;
    live->previous_columns = 0;
}

void live_finish(Live *live, StringBuilder *out)
{
    if (!live->interactive && live->previous_len > 0) {
        str_builder_append(out, previous_frame(live));
        str_builder_append_char(out, '\n');
    }
    if (live->cursor_hidden) {
        str_builder_append(out, S("\x1b[?25h"));
    }
    forget(live);
}

void live_clear(Live *live, StringBuilder *out)
{
    if (live->interactive && live->previous_len > 0) {
        size_t lines = str_count_char(previous_frame(live), '\n') + 1;
        str_builder_append_format(out, "\x1b[?2026h\x1b[%zuF\x1b[J\x1b[?2026l", lines);
    }
    if (live->cursor_hidden) {
        str_builder_append(out, S("\x1b[?25h"));
    }
    forget(live);
}
