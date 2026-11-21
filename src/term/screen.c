#include "mc/term/screen.h"

#include <string.h>

#include "mc/term/ansi.h"
#include "mc/text/utf8.h"

enum { ESC = 0x1b, SGR_PARAMS = 32, TAB_STOP = 8, LINK_CAPACITY = 1024, LINK_MAX_BYTES = 2048 };

struct Screen {
    Arena *arena;   // the Screen itself
    Arena *buffers; // the cells, for the current size
    ColorDepth depth;
    int width;
    int height;
    Cell *cells;
    Cell *shown; // what the terminal shows, as of the last render
    bool repaint;
    Arena *links; // the uris of links, for link ids 1 to link_count
    String *uris;
    uint16_t link_count;
};

static const Cell BLANK = { .glyph = ' ' };

Screen *screen_create(ColorDepth depth)
{
    Arena *arena = arena_create(0);
    Screen *screen = arena_push(arena, sizeof *screen);
    screen->arena = arena;
    screen->buffers = arena_create(0);
    screen->depth = depth;
    screen->repaint = true;
    screen->links = arena_create(0);
    screen->uris = arena_push(screen->links, LINK_CAPACITY * sizeof(String));
    return screen;
}

void screen_destroy(Screen *screen)
{
    arena_destroy(screen->links);
    arena_destroy(screen->buffers);
    arena_destroy(screen->arena);
}

void screen_resize(Screen *screen, int width, int height)
{
    arena_reset(screen->buffers);
    screen->width = width > 0 ? width : 0;
    screen->height = height > 0 ? height : 0;
    size_t count = arena_size_mul((size_t)screen->width, (size_t)screen->height);
    screen->cells = arena_push(screen->buffers, arena_size_mul(count, sizeof(Cell)));
    screen->shown = arena_push(screen->buffers, arena_size_mul(count, sizeof(Cell)));
    for (size_t i = 0; i < count; i++) {
        screen->cells[i] = BLANK;
    }
    screen->repaint = true;
}

int screen_width(const Screen *screen)
{
    return screen->width;
}

int screen_height(const Screen *screen)
{
    return screen->height;
}

Cell *screen_cell(Screen *screen, int x, int y)
{
    if (x < 0 || y < 0 || x >= screen->width || y >= screen->height) {
        return nullptr;
    }
    return &screen->cells[(size_t)y * (size_t)screen->width + (size_t)x];
}

Rect rect_inner(Rect rect)
{
    return (Rect){ rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2 };
}

bool rect_contains(Rect rect, int x, int y)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

void screen_paint(Screen *screen, Rect rect, Rgb bg)
{
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            Cell *cell = screen_cell(screen, x, y);
            if (cell != nullptr) {
                *cell = (Cell) { .glyph = ' ', .fg = bg, .bg = bg };
            }
        }
    }
}

void screen_paint_default(Screen *screen, Rect rect, Rgb bg)
{
    screen_paint(screen, rect, bg);
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            Cell *cell = screen_cell(screen, x, y);
            if (cell != nullptr) {
                cell->attrs = ATTR_DEFAULT_BG;
            }
        }
    }
}

// set_cell writes one column and keeps wide glyphs whole: a half that loses
// its other half becomes a blank.
static void set_cell(Screen *screen, int x, int y, uint32_t glyph, Rgb fg, uint8_t attrs)
{
    Cell *cell = screen_cell(screen, x, y);
    if (cell->glyph == 0) {
        Cell *head = screen_cell(screen, x - 1, y);
        if (head != nullptr) {
            head->glyph = ' ';
        }
    }
    Cell *next = screen_cell(screen, x + 1, y);
    if (next != nullptr && next->glyph == 0) {
        next->glyph = ' ';
    }
    cell->glyph = glyph;
    cell->fg = fg;
    cell->attrs = (uint8_t)((attrs & ~ATTR_DEFAULT_BG) | (cell->attrs & ATTR_DEFAULT_BG));
    cell->link = 0;
}

static int put(Screen *screen, int x, int y, uint32_t glyph, Rgb fg, uint8_t attrs)
{
    int width = utf8_codepoint_width(glyph);
    if (width == 0 || screen_cell(screen, x, y) == nullptr) {
        return 0;
    }
    if (glyph < 0x20 || glyph == 0x7f) {
        glyph = ' ';
    }
    if (width == 2 && screen_cell(screen, x + 1, y) == nullptr) {
        set_cell(screen, x, y, ' ', fg, attrs);
        return 1;
    }
    set_cell(screen, x, y, glyph, fg, attrs);
    if (width == 2) {
        set_cell(screen, x + 1, y, 0, fg, attrs);
    }
    return width;
}

void screen_glyph(Screen *screen, int x, int y, uint32_t glyph, Rgb fg, uint8_t attrs)
{
    put(screen, x, y, glyph, fg, attrs);
}

int screen_text(Screen *screen, int x, int y, int max_columns, String text, Rgb fg, uint8_t attrs)
{
    int used = 0;
    size_t position = 0;
    while (position < text.len) {
        uint32_t codepoint;
        utf8_decode(text, &position, &codepoint);
        int width = utf8_codepoint_width(codepoint);
        if (used + width > max_columns) {
            break;
        }
        put(screen, x + used, y, codepoint, fg, attrs);
        used += width;
    }
    return used;
}

// Ink is what styled text writes with; a background only where it set one.
typedef struct Ink {
    Rgb fg;
    Rgb bg;
    bool has_bg;
    bool reverse;
    uint8_t attrs;
} Ink;

static uint8_t sgr_value(unsigned value)
{
    return (uint8_t)(value > 255 ? 255 : value);
}

// extended_color reads the colour that follows the 38 or 48 at params[*i],
// 5;n or 2;r;g;b, and leaves *i on its last parameter.
static bool extended_color(const unsigned *params, size_t count, size_t *i, Rgb *color)
{
    if (*i + 2 < count && params[*i + 1] == 5) {
        *color = xterm_to_rgb(sgr_value(params[*i + 2]));
        *i += 2;
        return true;
    }
    if (*i + 4 < count && params[*i + 1] == 2) {
        *color = (Rgb){ sgr_value(params[*i + 2]), sgr_value(params[*i + 3]), sgr_value(params[*i + 4]) };
        *i += 4;
        return true;
    }
    *i = count;
    return false;
}

static Rgb basic_color(const Rgb *ansi, unsigned index)
{
    return ansi != nullptr ? ansi[index] : xterm_to_rgb((uint8_t)index);
}

static void apply_sgr(Ink *ink, String text, Ink plain, const Rgb *ansi)
{
    unsigned params[SGR_PARAMS];
    size_t count = 0;
    bool more = true;
    while (more && count < SGR_PARAMS) {
        String field;
        more = str_cut(text, ';', &field, &text);
        uint64_t value = 0;
        params[count++] = str_parse_u64(field, &value) && value < 1000 ? (unsigned)value : 0;
    }
    for (size_t i = 0; i < count; i++) {
        unsigned p = params[i];
        if (p == 0) {
            *ink = plain;
        } else if (p >= 1 && p <= 4) {
            ink->attrs = (uint8_t)(ink->attrs | 1u << (p - 1));
        } else if (p == 22) {
            ink->attrs = (uint8_t)(ink->attrs & ~(ATTR_BOLD | ATTR_DIM));
        } else if (p == 23) {
            ink->attrs = (uint8_t)(ink->attrs & ~ATTR_ITALIC);
        } else if (p == 24) {
            ink->attrs = (uint8_t)(ink->attrs & ~ATTR_UNDERLINE);
        } else if (p == 7 || p == 27) {
            ink->reverse = p == 7;
        } else if (p >= 30 && p <= 37) {
            ink->fg = basic_color(ansi, p - 30);
            ink->attrs = (uint8_t)(ink->attrs & ~ATTR_DEFAULT_FG);
        } else if (p >= 90 && p <= 97) {
            ink->fg = basic_color(ansi, p - 90 + 8);
            ink->attrs = (uint8_t)(ink->attrs & ~ATTR_DEFAULT_FG);
        } else if (p == 39) {
            ink->fg = plain.fg;
            ink->attrs = (uint8_t)((ink->attrs & ~ATTR_DEFAULT_FG) | (plain.attrs & ATTR_DEFAULT_FG));
        } else if (p >= 40 && p <= 47) {
            ink->bg = basic_color(ansi, p - 40);
            ink->has_bg = true;
        } else if (p >= 100 && p <= 107) {
            ink->bg = basic_color(ansi, p - 100 + 8);
            ink->has_bg = true;
        } else if (p == 49) {
            ink->has_bg = false;
        } else if (p == 38) {
            if (extended_color(params, count, &i, &ink->fg)) {
                ink->attrs = (uint8_t)(ink->attrs & ~ATTR_DEFAULT_FG);
            }
        } else if (p == 48) {
            ink->has_bg = extended_color(params, count, &i, &ink->bg) || ink->has_bg;
        }
    }
}

// next_glyph decodes the glyph at *position, applying the SGR sequences
// before it to ink and skipping other escape sequences, and is false at the
// end of text.
static bool next_glyph(String text, size_t *position, Ink *ink, Ink plain, const Rgb *ansi, uint32_t *glyph)
{
    while (*position < text.len && text.data[*position] == ESC) {
        size_t end = ansi_escape_end(text, *position);
        if (end - *position >= 3 && text.data[*position + 1] == '[' && text.data[end - 1] == 'm') {
            apply_sgr(ink, str_slice(text, *position + 2, end - 1), plain, ansi);
        }
        *position = end;
    }
    if (*position >= text.len) {
        return false;
    }
    utf8_decode(text, position, glyph);
    return true;
}

// put_ink is put with ink's background, or with reverse video the colours
// swapped over the background already there. Reverse video swaps what the
// cells hold, so a terminal's own colour becomes the guess at it.
static int put_ink(Screen *screen, int x, int y, uint32_t glyph, Ink ink)
{
    Cell *cell = screen_cell(screen, x, y);
    if (cell == nullptr) {
        return 0;
    }
    Rgb bg = ink.has_bg ? ink.bg : cell->bg;
    bool default_bg = !ink.has_bg && (cell->attrs & ATTR_DEFAULT_BG) != 0;
    if (ink.reverse) {
        Rgb fg = ink.fg;
        ink.fg = bg;
        bg = fg;
        default_bg = false;
        ink.attrs = (uint8_t)(ink.attrs & ~ATTR_DEFAULT_FG);
    }
    int written = put(screen, x, y, glyph, ink.fg, ink.attrs);
    for (int i = 0; i < written; i++) {
        Cell *placed = screen_cell(screen, x + i, y);
        placed->bg = bg;
        placed->attrs = (uint8_t)(default_bg ? placed->attrs | ATTR_DEFAULT_BG : placed->attrs & ~ATTR_DEFAULT_BG);
    }
    return written;
}

int screen_styled(Screen *screen, int x, int y, int max_columns, String text, Rgb fg, uint8_t attrs)
{
    Ink plain = { .fg = fg, .attrs = attrs };
    Ink ink = plain;
    int used = 0;
    size_t position = 0;
    uint32_t codepoint;
    while (next_glyph(text, &position, &ink, plain, nullptr, &codepoint)) {
        int width = utf8_codepoint_width(codepoint);
        if (used + width > max_columns) {
            break;
        }
        put_ink(screen, x + used, y, codepoint, ink);
        used += width;
    }
    return used;
}

// Wrap places the glyphs of styled text in rows of rect.width columns and,
// with a screen, draws the rows from skip_rows on that fit in rect.
typedef struct Wrap {
    Screen *screen;
    Rect rect;
    int skip_rows;
    int row;
    int column;
    int line_column; // for tab stops, which count from the start of the text
} Wrap;

static void wrap_place(Wrap *wrap, uint32_t glyph, int width, Ink ink)
{
    if (wrap->column + width > wrap->rect.width && wrap->column > 0) {
        wrap->row++;
        wrap->column = 0;
    }
    int shown_row = wrap->row - wrap->skip_rows;
    if (wrap->screen != nullptr && shown_row >= 0 && shown_row < wrap->rect.height) {
        put_ink(wrap->screen, wrap->rect.x + wrap->column, wrap->rect.y + shown_row, glyph, ink);
    }
    wrap->column += width;
    wrap->line_column += width;
}

static void wrap_text(Wrap *wrap, String text, TextStyle style)
{
    if (wrap->rect.width < 1) {
        wrap->rect.width = 1;
    }
    Ink plain = { .fg = style.fg, .attrs = style.attrs };
    Ink ink = plain;
    size_t position = 0;
    uint32_t codepoint;
    while (next_glyph(text, &position, &ink, plain, style.ansi, &codepoint)) {
        if (codepoint == '\t') {
            for (int spaces = TAB_STOP - wrap->line_column % TAB_STOP; spaces > 0; spaces--) {
                wrap_place(wrap, ' ', 1, ink);
            }
            continue;
        }
        int width = utf8_codepoint_width(codepoint);
        if (width > 0) {
            wrap_place(wrap, codepoint, width, ink);
        }
    }
}

int screen_wrapped(Screen *screen, Rect rect, int skip_rows, String text, TextStyle style)
{
    Wrap wrap = { .screen = screen, .rect = rect, .skip_rows = skip_rows };
    wrap_text(&wrap, text, style);
    int rows = wrap.row + 1 - skip_rows;
    return rows < 0 ? 0 : rows > rect.height ? rect.height : rows;
}

int wrapped_rows(String text, int width)
{
    Wrap wrap = { .rect.width = width };
    wrap_text(&wrap, text, (TextStyle){ 0 });
    return wrap.row + 1;
}

void wrapped_end(String text, int width, int *row, int *column)
{
    Wrap wrap = { .rect.width = width };
    wrap_text(&wrap, text, (TextStyle){ 0 });
    bool full = wrap.column >= wrap.rect.width;
    *row = wrap.row + (full ? 1 : 0);
    *column = full ? 0 : wrap.column;
}

uint16_t screen_link(Screen *screen, String uri)
{
    if (uri.len == 0 || uri.len > LINK_MAX_BYTES) {
        return 0;
    }
    for (size_t i = 0; i < uri.len; i++) {
        // OSC 8 takes printable ASCII; anything else could end the
        // sequence, as a C1 ST in UTF-8 does, and must be percent-encoded.
        unsigned char byte = (unsigned char)uri.data[i];
        if (byte < 0x20 || byte > 0x7e) {
            return 0;
        }
    }
    for (uint16_t i = 0; i < screen->link_count; i++) {
        if (str_equal(screen->uris[i], uri)) {
            return (uint16_t)(i + 1);
        }
    }
    if (screen->link_count == LINK_CAPACITY) {
        return 0;
    }
    screen->uris[screen->link_count++] = str_copy(screen->links, uri);
    return screen->link_count;
}

void screen_mark_link(Screen *screen, Rect rect, uint16_t link)
{
    if (link > screen->link_count) {
        link = 0;
    }
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            Cell *cell = screen_cell(screen, x, y);
            if (cell != nullptr) {
                cell->link = link;
            }
        }
    }
}

// forget_links keeps only the links the cells show, so that a screen that
// shows many links over time does not run out of ids. It runs after a
// render, when the cells are what the terminal shows.
static void forget_links(Screen *screen)
{
    uint16_t kept_ids[LINK_CAPACITY + 1] = { 0 };
    Arena *kept = arena_create(0);
    String *uris = arena_push(kept, LINK_CAPACITY * sizeof(String));
    uint16_t count = 0;
    size_t cells = (size_t)screen->width * (size_t)screen->height;
    for (size_t i = 0; i < cells; i++) {
        uint16_t link = screen->cells[i].link;
        if (link != 0 && kept_ids[link] == 0) {
            uris[count] = str_copy(kept, screen->uris[link - 1]);
            kept_ids[link] = ++count;
        }
        screen->cells[i].link = kept_ids[link];
        screen->shown[i].link = kept_ids[link];
    }
    arena_destroy(screen->links);
    screen->links = kept;
    screen->uris = uris;
    screen->link_count = count;
}

void screen_shade(Screen *screen, Rect rect, double factor)
{
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            Cell *cell = screen_cell(screen, x, y);
            if (cell != nullptr) {
                cell->fg = rgb_scale(cell->fg, factor);
                cell->bg = rgb_scale(cell->bg, factor);
                cell->attrs = (uint8_t)(cell->attrs & ~(ATTR_DEFAULT_FG | ATTR_DEFAULT_BG));
            }
        }
    }
}

void screen_fade(Screen *screen, Rect rect, Rgb toward, double t)
{
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            Cell *cell = screen_cell(screen, x, y);
            if (cell != nullptr) {
                cell->fg = rgb_mix_oklab(cell->fg, toward, t);
                cell->bg = rgb_mix_oklab(cell->bg, toward, t);
                cell->attrs = (uint8_t)(cell->attrs & ~(ATTR_DEFAULT_FG | ATTR_DEFAULT_BG));
            }
        }
    }
}

void screen_invalidate(Screen *screen)
{
    screen->repaint = true;
}

static bool cell_equal(const Cell *a, const Cell *b)
{
    return a->glyph == b->glyph && a->attrs == b->attrs && a->link == b->link && rgb_equal(a->fg, b->fg) &&
           rgb_equal(a->bg, b->bg);
}

// Pen is the style the terminal writes with.
typedef struct {
    Rgb fg;
    Rgb bg;
    uint8_t attrs;
    bool known;
    uint16_t link;
} Pen;

static void use_link(StringBuilder *out, const Screen *screen, Pen *pen, uint16_t link)
{
    if (pen->link == link) {
        return;
    }
    str_builder_append(out, S("\x1b]8;;"));
    if (link != 0) {
        str_builder_append(out, screen->uris[link - 1]);
    }
    str_builder_append(out, S("\x1b\\"));
    pen->link = link;
}

static void use_pen(StringBuilder *out, ColorDepth depth, Pen *pen, const Cell *cell)
{
    bool colored = depth != COLOR_DEPTH_NONE;
    bool own_fg = colored && (cell->attrs & ATTR_DEFAULT_FG) == 0;
    bool own_bg = colored && (cell->attrs & ATTR_DEFAULT_BG) == 0;
    if (!pen->known || pen->attrs != cell->attrs) {
        str_builder_append(out, S("\x1b[0"));
        static const char *const CODES[] = { ";1", ";2", ";3", ";4" };
        for (size_t i = 0; i < countof(CODES); i++) {
            if ((cell->attrs & (1u << i)) != 0) {
                str_builder_append(out, S(CODES[i]));
            }
        }
        if (own_fg) {
            str_builder_append_char(out, ';');
            sgr_append_color(out, depth, cell->fg, false);
        }
        if (own_bg) {
            str_builder_append_char(out, ';');
            sgr_append_color(out, depth, cell->bg, true);
        }
        str_builder_append_char(out, 'm');
        *pen = (Pen){ .fg = cell->fg, .bg = cell->bg, .attrs = cell->attrs, .known = true };
        return;
    }
    if (own_fg && !rgb_equal(pen->fg, cell->fg)) {
        str_builder_append(out, S("\x1b["));
        sgr_append_color(out, depth, cell->fg, false);
        str_builder_append_char(out, 'm');
        pen->fg = cell->fg;
    }
    if (own_bg && !rgb_equal(pen->bg, cell->bg)) {
        str_builder_append(out, S("\x1b["));
        sgr_append_color(out, depth, cell->bg, true);
        str_builder_append_char(out, 'm');
        pen->bg = cell->bg;
    }
}

void screen_render(Screen *screen, StringBuilder *out)
{
    Pen pen = { 0 };
    int cursor_x = -1;
    int cursor_y = -1;
    for (int y = 0; y < screen->height; y++) {
        for (int x = 0; x < screen->width; x++) {
            size_t index = (size_t)y * (size_t)screen->width + (size_t)x;
            const Cell *cell = &screen->cells[index];
            if (cell->glyph == 0 || (!screen->repaint && cell_equal(cell, &screen->shown[index]))) {
                continue;
            }
            if (cursor_x != x || cursor_y != y) {
                str_builder_append_format(out, "\x1b[%d;%dH", y + 1, x + 1);
            }
            use_link(out, screen, &pen, cell->link);
            use_pen(out, screen->depth, &pen, cell);
            char bytes[UTF8_MAX_BYTES];
            str_builder_append(out, (String) { bytes, utf8_encode(cell->glyph, bytes) });
            cursor_x = x + utf8_codepoint_width(cell->glyph);
            cursor_y = y;
            if (cursor_x >= screen->width) {
                cursor_x = -1;
            }
        }
    }
    use_link(out, screen, &pen, 0);
    size_t count = (size_t)screen->width * (size_t)screen->height;
    if (count > 0) {
        memcpy(screen->shown, screen->cells, count * sizeof(Cell));
    }
    screen->repaint = false;
    if (screen->link_count > LINK_CAPACITY / 2) {
        forget_links(screen);
    }
}
