#pragma once

#include "mc/term/history.h"
#include "mc/term/screen.h"

// Panel is a rounded frame with a title on its top edge and optional info at
// the right of that edge. The top and left edges blend from glow into border,
// as if lit from the top-left corner.
typedef struct Panel {
    String title;
    Rgb title_color;
    String info;
    Rgb info_color;
    Rgb border;
    Rgb glow;
} Panel;

void draw_panel(Screen *screen, Rect rect, Panel panel);

// draw_meter draws width squares, the first value of them in the colours of
// gradient along the meter and the rest in empty. value runs from 0 to 1.
void draw_meter(Screen *screen, int x, int y, int width, float value, Gradient gradient, Rgb empty);

// draw_graph plots history in braille dots, two samples per column, the
// newest at the right. A sample of scale fills the height. Rows take the
// colours of gradient from the baseline outward; a downward graph hangs from
// the top edge.
void draw_graph(Screen *screen, Rect rect, const History *history, float scale, Gradient gradient, bool downward);

// draw_sparkline plots one sample per column in eighth blocks.
void draw_sparkline(Screen *screen, int x, int y, int width, const History *history, float scale, Gradient gradient,
                    Rgb baseline);

// draw_text_right puts text so that it ends just before x_end and returns where it starts.
int draw_text_right(Screen *screen, int x_end, int y, String text, Rgb fg, uint8_t attrs);
