#include "mc/term/layout.h"

#include "mc/term/ansi.h"

typedef struct BorderChars {
    const char *top_left;
    const char *top;
    const char *top_right;
    const char *side;
    const char *bottom_left;
    const char *bottom_right;
} BorderChars;

static const BorderChars BORDERS[] = {
    [BORDER_NONE] = { "", "", "", "", "", "" },
    [BORDER_NORMAL] = { "┌", "─", "┐", "│", "└", "┘" },
    [BORDER_ROUNDED] = { "╭", "─", "╮", "│", "╰", "╯" },
    [BORDER_THICK] = { "┏", "━", "┓", "┃", "┗", "┛" },
    [BORDER_DOUBLE] = { "╔", "═", "╗", "║", "╚", "╝" },
    [BORDER_HIDDEN] = { " ", " ", " ", " ", " ", " " },
};

static void append_spaces(StringBuilder *out, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        str_builder_append_char(out, ' ');
    }
}

static void append_repeated(StringBuilder *out, String s, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        str_builder_append(out, s);
    }
}

// lead is how much of extra goes before what it places.
static size_t lead(size_t extra, Align align)
{
    return align == ALIGN_START ? 0 : align == ALIGN_CENTER ? extra / 2 : extra;
}

static StringList block_lines(Arena *arena, String block)
{
    return str_split(arena, block, '\n');
}

size_t block_width(String block)
{
    size_t width = 0;
    String rest = block;
    bool more = true;
    while (more) {
        String line;
        more = str_cut(rest, '\n', &line, &rest);
        width = max_size(width, ansi_width(line));
    }
    return width;
}

// append_placed appends line, cut to width, with spaces placing it in width
// by align. A cut line that was styled is reset, as its own reset was cut.
static void append_placed(StringBuilder *out, String line, size_t width, Align align)
{
    String visible = ansi_truncate(line, width);
    size_t extra = width - ansi_width(visible);
    append_spaces(out, lead(extra, align));
    str_builder_append(out, visible);
    if (visible.len < line.len && str_contains(visible, S("\x1b"))) {
        str_builder_append(out, S("\x1b[0m"));
    }
    append_spaces(out, extra - lead(extra, align));
}

// append_framed appends one line of the box: margin, a side of the border,
// content, the other side, margin.
static void append_framed(StringBuilder *out, ColorDepth depth, const Box *box, String content)
{
    String side = S(BORDERS[box->border].side);
    append_spaces(out, box->margin.left);
    style_append(out, depth, box->border_style, side);
    str_builder_append(out, content);
    style_append(out, depth, box->border_style, side);
    append_spaces(out, box->margin.right);
}

static void append_edge(StringBuilder *out, ColorDepth depth, const Box *box, size_t inner, bool top)
{
    const BorderChars *chars = &BORDERS[box->border];
    StringBuilder edge = str_builder_create(out->arena, 64);
    str_builder_append(&edge, S(top ? chars->top_left : chars->bottom_left));
    append_repeated(&edge, S(chars->top), inner);
    str_builder_append(&edge, S(top ? chars->top_right : chars->bottom_right));
    append_spaces(out, box->margin.left);
    style_append(out, depth, box->border_style, str_builder_finish(&edge));
    append_spaces(out, box->margin.right);
}

String box_render(Arena *arena, ColorDepth depth, Box box, String text)
{
    StringList lines = block_lines(arena, text);
    size_t width = box.width > 0 ? box.width : block_width(text);
    size_t inner = box.padding.left + width + box.padding.right;
    size_t outer = box.margin.left + (box.border != BORDER_NONE ? 2 : 0) + inner + box.margin.right;
    size_t rows = box.padding.top + lines.count + box.padding.bottom;

    StringBuilder out = str_builder_create(arena, 256);
    for (size_t i = 0; i < box.margin.top; i++) {
        append_spaces(&out, outer);
        str_builder_append_char(&out, '\n');
    }
    if (box.border != BORDER_NONE) {
        append_edge(&out, depth, &box, inner, true);
        str_builder_append_char(&out, '\n');
    }
    for (size_t row = 0; row < rows; row++) {
        StringBuilder content = str_builder_create(arena, 128);
        append_spaces(&content, box.padding.left);
        bool is_text = row >= box.padding.top && row < box.padding.top + lines.count;
        append_placed(&content, is_text ? lines.items[row - box.padding.top] : S(""), width, box.align);
        append_spaces(&content, box.padding.right);
        StringBuilder styled = str_builder_create(arena, 128);
        style_append(&styled, depth, box.style, str_builder_finish(&content));
        append_framed(&out, depth, &box, str_builder_finish(&styled));
        str_builder_append_char(&out, '\n');
    }
    if (box.border != BORDER_NONE) {
        append_edge(&out, depth, &box, inner, false);
        str_builder_append_char(&out, '\n');
    }
    for (size_t i = 0; i < box.margin.bottom; i++) {
        append_spaces(&out, outer);
        str_builder_append_char(&out, '\n');
    }
    return str_trim_suffix(str_builder_finish(&out), S("\n"));
}

String join_horizontal(Arena *arena, StringList blocks, Align align)
{
    size_t height = 0;
    StringList *lines = arena_push(arena, (blocks.count + 1) * sizeof *lines);
    size_t *widths = arena_push(arena, (blocks.count + 1) * sizeof *widths);
    for (size_t i = 0; i < blocks.count; i++) {
        lines[i] = block_lines(arena, blocks.items[i]);
        widths[i] = block_width(blocks.items[i]);
        height = max_size(height, lines[i].count);
    }
    StringBuilder out = str_builder_create(arena, 256);
    for (size_t row = 0; row < height; row++) {
        if (row > 0) {
            str_builder_append_char(&out, '\n');
        }
        for (size_t i = 0; i < blocks.count; i++) {
            size_t top = lead(height - lines[i].count, align);
            bool inside = row >= top && row < top + lines[i].count;
            append_placed(&out, inside ? lines[i].items[row - top] : S(""), widths[i], ALIGN_START);
        }
    }
    return str_builder_finish(&out);
}

String join_vertical(Arena *arena, StringList blocks, Align align)
{
    size_t width = 0;
    for (size_t i = 0; i < blocks.count; i++) {
        width = max_size(width, block_width(blocks.items[i]));
    }
    StringBuilder out = str_builder_create(arena, 256);
    bool first = true;
    for (size_t i = 0; i < blocks.count; i++) {
        StringList lines = block_lines(arena, blocks.items[i]);
        for (size_t row = 0; row < lines.count; row++) {
            if (!first) {
                str_builder_append_char(&out, '\n');
            }
            first = false;
            append_placed(&out, lines.items[row], width, align);
        }
    }
    return str_builder_finish(&out);
}
