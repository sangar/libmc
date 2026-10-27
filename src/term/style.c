#include "mc/term/style.h"

#include "mc/platform/platform.h"
#include "mc/text/utf8.h"

static ColorDepth forced_depth(String force_color)
{
    if (str_equal(force_color, S("0")) || str_equal(force_color, S("false"))) {
        return COLOR_DEPTH_NONE;
    }
    if (str_equal(force_color, S("2"))) {
        return COLOR_DEPTH_256;
    }
    if (str_equal(force_color, S("3"))) {
        return COLOR_DEPTH_TRUE;
    }
    return COLOR_DEPTH_16;
}

ColorDepth color_depth_of(ColorEnvironment environment)
{
    if (environment.force_color_set) {
        return forced_depth(environment.force_color);
    }
    if (environment.no_color.len > 0 || !environment.is_terminal || str_equal(environment.term, S("dumb"))) {
        return COLOR_DEPTH_NONE;
    }
    if (str_equal(environment.colorterm, S("truecolor")) || str_equal(environment.colorterm, S("24bit"))) {
        return COLOR_DEPTH_TRUE;
    }
    if (str_contains(environment.term, S("256color"))) {
        return COLOR_DEPTH_256;
    }
    return COLOR_DEPTH_16;
}

static String env_value(Arena *arena, const char *name, bool *set)
{
    String value = { 0 };
    bool found = env_get(arena, S(name), &value);
    if (set != nullptr) {
        *set = found;
    }
    return found ? value : (String){ 0 };
}

ColorDepth color_depth_detect(Arena *arena, bool is_terminal)
{
    ColorEnvironment environment = {
        .is_terminal = is_terminal,
        .no_color = env_value(arena, "NO_COLOR", nullptr),
        .colorterm = env_value(arena, "COLORTERM", nullptr),
        .term = env_value(arena, "TERM", nullptr),
    };
    environment.force_color = env_value(arena, "FORCE_COLOR", &environment.force_color_set);
    return color_depth_of(environment);
}

Style style_fg(Rgb fg)
{
    return (Style){ .fg = fg, .has_fg = true };
}

void sgr_append_color(StringBuilder *out, ColorDepth depth, Rgb color, bool background)
{
    unsigned extended = background ? 48 : 38;
    switch (depth) {
    case COLOR_DEPTH_NONE:
        return;
    case COLOR_DEPTH_16: {
        unsigned index = rgb_to_ansi16(color);
        unsigned base = background ? (index < 8 ? 40 : 100) : (index < 8 ? 30 : 90);
        str_builder_append_format(out, "%u", base + index % 8);
        return;
    }
    case COLOR_DEPTH_256:
        str_builder_append_format(out, "%u;5;%u", extended, (unsigned)rgb_to_xterm(color));
        return;
    case COLOR_DEPTH_TRUE:
        str_builder_append_format(out, "%u;2;%u;%u;%u", extended, color.r, color.g, color.b);
        return;
    }
}

static void append_color(StringBuilder *out, ColorDepth depth, Rgb color, bool background)
{
    if (depth != COLOR_DEPTH_NONE) {
        str_builder_append_char(out, ';');
        sgr_append_color(out, depth, color, background);
    }
}

// append_sgr starts from a reset, so a style never inherits the one before it.
static void append_sgr(StringBuilder *out, ColorDepth depth, Style style)
{
    str_builder_append(out, S("\x1b[0"));
    if (style.bold) {
        str_builder_append(out, S(";1"));
    }
    if (style.dim) {
        str_builder_append(out, S(";2"));
    }
    if (style.italic) {
        str_builder_append(out, S(";3"));
    }
    if (style.underline) {
        str_builder_append(out, S(";4"));
    }
    if (style.has_fg) {
        append_color(out, depth, style.fg, false);
    }
    if (style.has_bg) {
        append_color(out, depth, style.bg, true);
    }
    str_builder_append_char(out, 'm');
}

static bool style_is_plain(Style style)
{
    return !style.has_fg && !style.has_bg && !style.bold && !style.dim && !style.italic && !style.underline;
}

void style_append(StringBuilder *out, ColorDepth depth, Style style, String text)
{
    if (depth == COLOR_DEPTH_NONE || style_is_plain(style)) {
        str_builder_append(out, text);
        return;
    }
    append_sgr(out, depth, style);
    str_builder_append(out, text);
    str_builder_append(out, S("\x1b[0m"));
}

void gradient_append(StringBuilder *out, ColorDepth depth, Gradient gradient, bool bold, String text)
{
    if (depth == COLOR_DEPTH_NONE || text.len == 0) {
        str_builder_append(out, text);
        return;
    }
    size_t width = utf8_width(text);
    size_t column = 0;
    size_t position = 0;
    while (position < text.len) {
        size_t start = position;
        uint32_t codepoint;
        utf8_decode(text, &position, &codepoint);
        int columns = utf8_codepoint_width(codepoint);
        if (columns > 0) {
            double t = width > 1 ? (double)column / (double)(width - 1) : 0.0;
            append_sgr(out, depth, (Style){ .fg = gradient_at(gradient, t), .has_fg = true, .bold = bold });
            column += (size_t)columns;
        }
        str_builder_append(out, str_slice(text, start, position));
    }
    str_builder_append(out, S("\x1b[0m"));
}
