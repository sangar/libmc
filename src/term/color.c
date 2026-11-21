#include "mc/term/color.h"

#include <math.h>

#include "mc/term/style.h"

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

typedef struct Oklab {
    double l;
    double a;
    double b;
} Oklab;

static double to_linear(uint8_t value)
{
    double c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static uint8_t from_linear(double c)
{
    double v = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return channel(v * 255.0);
}

// The matrices are Björn Ottosson's, https://bottosson.github.io/posts/oklab/.
static Oklab oklab_of(Rgb color)
{
    double r = to_linear(color.r);
    double g = to_linear(color.g);
    double b = to_linear(color.b);
    double l = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    double m = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return (Oklab){
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

static Rgb rgb_of(Oklab lab)
{
    double l = lab.l + 0.3963377774 * lab.a + 0.2158037573 * lab.b;
    double m = lab.l - 0.1055613458 * lab.a - 0.0638541728 * lab.b;
    double s = lab.l - 0.0894841775 * lab.a - 1.2914855480 * lab.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    return (Rgb){
        from_linear(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
        from_linear(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
        from_linear(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s),
    };
}

Rgb rgb_mix_oklab(Rgb a, Rgb b, double t)
{
    t = clamp_unit(t);
    if (t == 0.0) {
        return a;
    }
    if (t == 1.0) {
        return b;
    }
    Oklab x = oklab_of(a);
    Oklab y = oklab_of(b);
    return rgb_of((Oklab){ x.l + (y.l - x.l) * t, x.a + (y.a - x.a) * t, x.b + (y.b - x.b) * t });
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

// Bayer's 4x4 threshold matrix.
static const uint8_t BAYER[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

// dither_level is the index into levels, low to high, of the level just
// below value or the one just above it, picking the one above for the share
// of cells that matches how close value is to it.
static unsigned dither_level(const uint8_t *levels, unsigned count, unsigned value, double threshold)
{
    unsigned level = 0;
    while (level + 1 < count && levels[level + 1] <= value) {
        level++;
    }
    if (level + 1 == count) {
        return level;
    }
    double share = (double)(value - levels[level]) / (double)(levels[level + 1] - levels[level]);
    return share > threshold ? level + 1 : level;
}

Rgb rgb_dither(Rgb color, ColorDepth depth, int x, int y)
{
    if (depth != COLOR_DEPTH_256) {
        return color;
    }
    double threshold = (BAYER[y & 3][x & 3] + 0.5) / 16.0;
    if (rgb_to_xterm(color) >= 232) {
        // The grey ramp, between the cube's black and white.
        static const uint8_t GREYS[] = { 0,   8,   18,  28,  38,  48,  58,  68,  78,  88,  98,  108, 118,
                                         128, 138, 148, 158, 168, 178, 188, 198, 208, 218, 228, 238, 255 };
        unsigned average = ((unsigned)color.r + color.g + color.b) / 3;
        unsigned level = dither_level(GREYS, countof(GREYS), average, threshold);
        uint8_t index = level == 0 ? 16 : level == countof(GREYS) - 1 ? 231 : (uint8_t)(232 + level - 1);
        return xterm_to_rgb(index);
    }
    unsigned r = dither_level(CUBE_LEVELS, 6, color.r, threshold);
    unsigned g = dither_level(CUBE_LEVELS, 6, color.g, threshold);
    unsigned b = dither_level(CUBE_LEVELS, 6, color.b, threshold);
    return xterm_to_rgb((uint8_t)(16 + r * 36 + g * 6 + b));
}
