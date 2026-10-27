#include "mc/term/color.h"

static const uint8_t CUBE_LEVELS[6] = { 0, 95, 135, 175, 215, 255 };

static const Rgb SYSTEM_COLORS[16] = {
    { 0, 0, 0 },       { 205, 0, 0 },     { 0, 205, 0 },     { 205, 205, 0 },
    { 0, 0, 238 },     { 205, 0, 205 },   { 0, 205, 205 },   { 229, 229, 229 },
    { 127, 127, 127 }, { 255, 0, 0 },     { 0, 255, 0 },     { 255, 255, 0 },
    { 92, 92, 255 },   { 255, 0, 255 },   { 0, 255, 255 },   { 255, 255, 255 },
};

Rgb rgb_hex(uint32_t hex)
{
    return (Rgb){ (uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex };
}

bool rgb_equal(Rgb a, Rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static double clamp_unit(double t)
{
    return t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
}

static uint8_t channel(double value)
{
    return (uint8_t)(value < 0.0 ? 0.0 : value > 255.0 ? 255.0 : value + 0.5);
}

Rgb rgb_mix(Rgb a, Rgb b, double t)
{
    t = clamp_unit(t);
    return (Rgb){
        channel(a.r + (b.r - a.r) * t),
        channel(a.g + (b.g - a.g) * t),
        channel(a.b + (b.b - a.b) * t),
    };
}

Rgb rgb_scale(Rgb color, double factor)
{
    return (Rgb){ channel(color.r * factor), channel(color.g * factor), channel(color.b * factor) };
}

Rgb gradient_at(Gradient gradient, double t)
{
    t = clamp_unit(t);
    return t < 0.5 ? rgb_mix(gradient.start, gradient.middle, t * 2.0)
                   : rgb_mix(gradient.middle, gradient.end, (t - 0.5) * 2.0);
}

Gradient gradient_of(Rgb start, Rgb end)
{
    return (Gradient){ start, rgb_mix(start, end, 0.5), end };
}

Rgb xterm_to_rgb(uint8_t index)
{
    if (index < 16) {
        return SYSTEM_COLORS[index];
    }
    if (index < 232) {
        unsigned cube = index - 16u;
        return (Rgb){ CUBE_LEVELS[cube / 36], CUBE_LEVELS[cube / 6 % 6], CUBE_LEVELS[cube % 6] };
    }
    uint8_t grey = (uint8_t)(8 + (index - 232) * 10);
    return (Rgb){ grey, grey, grey };
}

static unsigned cube_index(uint8_t value)
{
    return value < 48 ? 0 : value < 115 ? 1 : (unsigned)(value - 35) / 40;
}

static unsigned distance(Rgb a, Rgb b)
{
    int dr = a.r - b.r;
    int dg = a.g - b.g;
    int db = a.b - b.b;
    return (unsigned)(dr * dr + dg * dg + db * db);
}

uint8_t rgb_to_xterm(Rgb color)
{
    unsigned r = cube_index(color.r);
    unsigned g = cube_index(color.g);
    unsigned b = cube_index(color.b);
    uint8_t cube = (uint8_t)(16 + r * 36 + g * 6 + b);
    unsigned average = ((unsigned)color.r + color.g + color.b) / 3;
    uint8_t grey = (uint8_t)(average > 238 ? 255 : average < 8 ? 232 : 232 + (average - 8) / 10);
    return distance(color, xterm_to_rgb(grey)) < distance(color, xterm_to_rgb(cube)) ? grey : cube;
}

uint8_t rgb_to_ansi16(Rgb color)
{
    uint8_t nearest = 0;
    for (uint8_t i = 1; i < 16; i++) {
        if (distance(color, SYSTEM_COLORS[i]) < distance(color, SYSTEM_COLORS[nearest])) {
            nearest = i;
        }
    }
    return nearest;
}

static bool hex_digit(char c, uint32_t *value)
{
    if (c >= '0' && c <= '9') {
        *value = (uint32_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
        *value = (uint32_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
        *value = (uint32_t)(c - 'A' + 10);
    } else {
        return false;
    }
    return true;
}

bool rgb_parse(String text, Rgb *color)
{
    uint64_t index;
    if (str_parse_u64(text, &index)) {
        if (index > 255) {
            return false;
        }
        *color = xterm_to_rgb((uint8_t)index);
        return true;
    }
    if ((text.len != 7 && text.len != 4) || text.data[0] != '#') {
        return false;
    }
    uint32_t hex = 0;
    for (size_t i = 1; i < text.len; i++) {
        uint32_t digit;
        if (!hex_digit(text.data[i], &digit)) {
            return false;
        }
        hex = text.len == 4 ? hex << 8 | digit << 4 | digit : hex << 4 | digit;
    }
    *color = rgb_hex(hex);
    return true;
}
