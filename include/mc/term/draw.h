#pragma once

#include "mc/term/history.h"
#include "mc/term/screen.h"

// BoxGlyphs draw a frame: its edges, its corners, and the two that hold a
// title on the top edge. Each takes one column, as do the glyphs of meters
// and sparklines. A glyph left 0 is the rounded one: ─ │ ╭ ╮ ╰ ╯ ┤ ├.
typedef struct BoxGlyphs {
    uint32_t horizontal;
    uint32_t vertical;
    uint32_t top_left;
    uint32_t top_right;
    uint32_t bottom_left;
    uint32_t bottom_right;
    uint32_t title_open;
    uint32_t title_close;
} BoxGlyphs;

// Panel is a frame, rounded unless glyphs say otherwise, with a title on its top edge and optional info at
// the right of that edge. The top and left edges blend from glow into border,
// as if lit from the top-left corner.
typedef struct Panel {
    String title;
    Rgb title_color;
    String info;
    Rgb info_color;
    Rgb border;
    Rgb glow;
    BoxGlyphs glyphs;
} Panel;

void draw_panel(Screen *screen, Rect rect, Panel panel);

// MeterGlyphs are a meter's filled and empty cells; a glyph left 0 is ■.
typedef struct MeterGlyphs {
    uint32_t filled;
    uint32_t empty;
} MeterGlyphs;

// draw_meter draws width cells, the first value of them filled in the
// colours of gradient along the meter and the rest empty, in empty. value
// runs from 0 to 1.
void draw_meter(Screen *screen, int x, int y, int width, float value, Gradient gradient, Rgb empty,
                MeterGlyphs glyphs);

// draw_graph plots history in braille dots, two samples per column, the
// newest at the right. A sample of scale fills the height. Rows take the
// colours of gradient from the baseline outward; a downward graph hangs from
// the top edge.
void draw_graph(Screen *screen, Rect rect, const History *history, float scale, Gradient gradient, bool downward);

// draw_sparkline plots one sample per column in the glyphs of levels, UTF-8
// from lowest to highest and at most 16, or in eighth blocks, ▁ to █, when
// it is empty. A sample too small for the lowest level shows it in baseline.
void draw_sparkline(Screen *screen, int x, int y, int width, const History *history, float scale, Gradient gradient,
                    Rgb baseline, String levels);

// draw_text_right puts text so that it ends just before x_end and returns where it starts.
int draw_text_right(Screen *screen, int x_end, int y, String text, Rgb fg, uint8_t attrs);
