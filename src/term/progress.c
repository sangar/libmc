#include "mc/term/progress.h"

#include <math.h>

// PARTIAL[n] is a block n eighths of a column wide, from the left.
static const char *const PARTIAL[8] = { "", "▏", "▎", "▍", "▌", "▋", "▊", "▉" };
static const Rgb EMPTY_COLOR = { 0x60, 0x60, 0x60 };

static Style cell_style(Gradient gradient, size_t cell, size_t width)
{
    double t = width > 1 ? (double)cell / (double)(width - 1) : 0.0;
    return style_fg(gradient_at(gradient, t));
}

void progress_append(StringBuilder *out, ColorDepth depth, size_t width, double fraction, Gradient gradient)
{
    fraction = fraction < 0.0 ? 0.0 : fraction > 1.0 ? 1.0 : fraction;
    size_t eighths = (size_t)lround(fraction * (double)width * 8.0);
    size_t full = eighths / 8;
    size_t partial = eighths % 8;
    for (size_t cell = 0; cell < full; cell++) {
        style_append(out, depth, cell_style(gradient, cell, width), S("█"));
    }
    if (partial > 0) {
        style_append(out, depth, cell_style(gradient, full, width), S(PARTIAL[partial]));
    }
    size_t empty = width - full - (partial > 0 ? 1 : 0);
    if (empty > 0) {
        style_append(out, depth, style_fg(EMPTY_COLOR), str_repeat(out->arena, S("░"), empty));
    }
}
