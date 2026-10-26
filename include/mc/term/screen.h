#pragma once

#include "mc/term/style.h"

// Screen is the picture to show, a grid of cells drawn from scratch every
// frame. screen_render compares it with what the terminal shows and writes
// only the cells that changed, the way btop draws.
typedef struct Screen Screen;

enum {
    ATTR_BOLD = 1 << 0,
    ATTR_DIM = 1 << 1,
    ATTR_ITALIC = 1 << 2,
    ATTR_UNDERLINE = 1 << 3,
};

// Cell is one column of one row. A glyph of 0 is the right half of the wide
// glyph to its left.
typedef struct Cell {
    uint32_t glyph;
    Rgb fg;
    Rgb bg;
    uint8_t attrs;
} Cell;

typedef struct Rect {
    int x;
    int y;
    int width;
    int height;
} Rect;

// screen_create makes an empty screen that renders colours at depth; it
// owns its memory until screen_destroy.
Screen *screen_create(ColorDepth depth);
void screen_destroy(Screen *screen);
// screen_resize discards the picture; the next render repaints every cell.
void screen_resize(Screen *screen, int width, int height);
int screen_width(const Screen *screen);
int screen_height(const Screen *screen);
// screen_cell is nullptr outside the screen.
Cell *screen_cell(Screen *screen, int x, int y);
// screen_paint fills rect with blanks on bg.
void screen_paint(Screen *screen, Rect rect, Rgb bg);
// screen_glyph puts one glyph and keeps the background already there.
void screen_glyph(Screen *screen, int x, int y, uint32_t glyph, Rgb fg, uint8_t attrs);
// screen_text puts UTF-8 text from (x, y), at most max_columns wide, over
// the backgrounds already there, and returns the columns it took.
int screen_text(Screen *screen, int x, int y, int max_columns, String text, Rgb fg, uint8_t attrs);
// screen_styled is screen_text for text styled with SGR sequences, such as
// what style_append, box_render and progress_append write: they set the
// colours and attributes, and a reset goes back to fg and attrs over the
// backgrounds already there. Other escape sequences are skipped.
int screen_styled(Screen *screen, int x, int y, int max_columns, String text, Rgb fg, uint8_t attrs);
// screen_shade darkens every colour in rect, as under an overlay.
void screen_shade(Screen *screen, Rect rect, double factor);
// screen_render appends to out what brings the terminal from the last
// rendered picture to this one.
void screen_render(Screen *screen, StringBuilder *out);
// screen_invalidate makes the next render repaint every cell, for when the
// terminal's contents are unknown.
void screen_invalidate(Screen *screen);
