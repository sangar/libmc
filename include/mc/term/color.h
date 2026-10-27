#pragma once

#include <stdint.h>

#include "mc/text/str.h"

typedef struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} Rgb;

// Gradient runs from start at 0 through middle at 0.5 to end at 1.
typedef struct Gradient {
    Rgb start;
    Rgb middle;
    Rgb end;
} Gradient;

Rgb rgb_hex(uint32_t hex);
bool rgb_equal(Rgb a, Rgb b);
// rgb_scale multiplies each channel by factor, clamped to 0..255.
Rgb rgb_scale(Rgb color, double factor);
// rgb_mix is a at 0 and b at 1; t is clamped to that range.
Rgb rgb_mix(Rgb a, Rgb b, double t);
Rgb gradient_at(Gradient gradient, double t);
// gradient_of is the gradient from start to end through their mix.
Gradient gradient_of(Rgb start, Rgb end);
// rgb_parse reads "#rrggbb", "#rgb", or an xterm colour number from 0 to 255.
bool rgb_parse(String text, Rgb *color);

// The xterm 256-colour palette: 16 system colours, a 6x6x6 cube, 24 greys.
Rgb xterm_to_rgb(uint8_t index);
// rgb_to_xterm is the nearest colour of the cube or the grey ramp.
uint8_t rgb_to_xterm(Rgb color);
// rgb_to_ansi16 is the nearest of the 16 system colours, as xterm draws them.
uint8_t rgb_to_ansi16(Rgb color);
