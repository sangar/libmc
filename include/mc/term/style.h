#pragma once

#include "mc/term/color.h"
#include "mc/core/arena.h"
#include "mc/text/str.h"

// ColorDepth is how many colours the output shows. COLOR_DEPTH_NONE is plain
// text without any escape sequence, for pipes, files and NO_COLOR.
typedef enum ColorDepth { COLOR_DEPTH_NONE, COLOR_DEPTH_16, COLOR_DEPTH_256, COLOR_DEPTH_TRUE } ColorDepth;

// ColorEnvironment is what decides the depth: whether the output is a
// terminal, and the variables NO_COLOR, FORCE_COLOR, COLORTERM and TERM.
typedef struct ColorEnvironment {
    bool is_terminal;
    String no_color;
    bool force_color_set;
    String force_color;
    String colorterm;
    String term;
} ColorEnvironment;

// color_depth_of decides as Node.js does. FORCE_COLOR wins: 0 or false is
// none, 2 is 256 colours, 3 is true colour, anything else 16. Then a
// non-empty NO_COLOR (https://no-color.org), output that is not a terminal
// and TERM=dumb are none; COLORTERM truecolor or 24bit is true colour; a TERM
// with 256color is 256; any other terminal is 16.
ColorDepth color_depth_of(ColorEnvironment environment);
// color_depth_detect is color_depth_of with this process's environment, read
// into arena; is_terminal says whether the output is a terminal.
ColorDepth color_depth_detect(Arena *arena, bool is_terminal);

// Style is how text looks; Style style = { 0 } is plain.
typedef struct Style {
    Rgb fg;
    Rgb bg;
    bool has_fg;
    bool has_bg;
    bool bold;
    bool dim;
    bool italic;
    bool underline;
} Style;

Style style_fg(Rgb fg);

// sgr_append_color appends the SGR parameters that make color the
// foreground, or the background, at depth: 38;2;r;g;b, 38;5;n or 31, and
// nothing at COLOR_DEPTH_NONE.
void sgr_append_color(StringBuilder *out, ColorDepth depth, Rgb color, bool background);
// style_append appends text in style, then a reset when style set anything.
// At COLOR_DEPTH_NONE it appends text alone.
void style_append(StringBuilder *out, ColorDepth depth, Style style, String text);
// gradient_append appends text with its foreground along gradient, start on
// the first column and end on the last, in bold when bold is set.
void gradient_append(StringBuilder *out, ColorDepth depth, Gradient gradient, bool bold, String text);
