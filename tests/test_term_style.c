#include "mc/term/style.h"
#include "test.h"

static void test_color_depth(Test *test)
{
    ColorEnvironment terminal = { .is_terminal = true, .term = S("xterm") };
    test_check(test, color_depth_of(terminal) == COLOR_DEPTH_16, "a terminal has 16 colours");
    test_check(test, color_depth_of((ColorEnvironment){ .term = S("xterm") }) == COLOR_DEPTH_NONE,
               "a pipe has none");

    ColorEnvironment with = terminal;
    with.term = S("xterm-256color");
    test_check(test, color_depth_of(with) == COLOR_DEPTH_256, "TERM with 256color");
    with.term = S("xterm-kitty");
    test_check(test, color_depth_of(with) == COLOR_DEPTH_TRUE, "a true-colour terminal by TERM");
    with.colorterm = S("truecolor");
    test_check(test, color_depth_of(with) == COLOR_DEPTH_TRUE, "COLORTERM=truecolor");
    with.no_color = S("1");
    test_check(test, color_depth_of(with) == COLOR_DEPTH_NONE, "NO_COLOR");
    with.force_color_set = true;
    with.force_color = S("3");
    test_check(test, color_depth_of(with) == COLOR_DEPTH_TRUE, "FORCE_COLOR wins over NO_COLOR");
    with.force_color = S("");
    test_check(test, color_depth_of(with) == COLOR_DEPTH_16, "an empty FORCE_COLOR is 16");
    with.force_color = S("0");
    test_check(test, color_depth_of(with) == COLOR_DEPTH_NONE, "FORCE_COLOR=0");

    ColorEnvironment dumb = { .is_terminal = true, .term = S("dumb") };
    test_check(test, color_depth_of(dumb) == COLOR_DEPTH_NONE, "TERM=dumb");
}

static String styled(Test *test, ColorDepth depth, Style style, String text)
{
    StringBuilder out = str_builder_create(test->arena, 64);
    style_append(&out, depth, style, text);
    return str_builder_finish(&out);
}

static void test_style_per_depth(Test *test)
{
    Style red = { .fg = rgb_hex(0xff0000), .has_fg = true, .bold = true };
    test_check_str(test, styled(test, COLOR_DEPTH_TRUE, red, S("æøå")), S("\x1b[0;1;38;2;255;0;0mæøå\x1b[0m"),
                   "true colour");
    test_check_str(test, styled(test, COLOR_DEPTH_256, red, S("æøå")), S("\x1b[0;1;38;5;196mæøå\x1b[0m"), "256");
    test_check_str(test, styled(test, COLOR_DEPTH_16, red, S("æøå")), S("\x1b[0;1;91mæøå\x1b[0m"), "16");
    test_check_str(test, styled(test, COLOR_DEPTH_NONE, red, S("ÆØÅ")), S("ÆØÅ"), "none is plain");
    test_check_str(test, styled(test, COLOR_DEPTH_TRUE, (Style){ 0 }, S("ÆØÅ")), S("ÆØÅ"), "a plain style");
}

// The combining ring of a decomposed å gets no colour of its own.
static void test_gradient(Test *test)
{
    Gradient grey = gradient_of(rgb_hex(0x000000), rgb_hex(0xffffff));
    StringBuilder out = str_builder_create(test->arena, 64);
    gradient_append(&out, COLOR_DEPTH_TRUE, grey, false, S("øa\xcc\x8a"));
    test_check_str(test, str_builder_finish(&out),
                   S("\x1b[0;38;2;0;0;0mø\x1b[0;38;2;255;255;255ma\xcc\x8a\x1b[0m"), "first to last column");
}

static void test_mix_oklab(Test *test)
{
    Rgb black = rgb_hex(0x000000);
    Rgb white = rgb_hex(0xffffff);
    test_check(test, rgb_equal(rgb_mix_oklab(black, white, 0.0), black) &&
                         rgb_equal(rgb_mix_oklab(black, white, 1.0), white),
               "the ends are the colours");
    test_check(test, rgb_equal(rgb_mix_oklab(black, white, 0.5), rgb_hex(0x636363)), "the middle is lightness 0.5");
    const uint32_t colors[] = { 0xff0000, 0x00ff00, 0x0000ff, 0x1e1e2e, 0xf5c2e7, 0x808080 };
    bool kept = true;
    for (size_t i = 0; i < countof(colors); i++) {
        Rgb color = rgb_hex(colors[i]);
        kept = kept && rgb_equal(rgb_mix_oklab(color, color, 0.5), color);
    }
    test_check(test, kept, "a colour survives the trip through OKLab");
    Rgb red_blue = rgb_mix_oklab(rgb_hex(0xff0000), rgb_hex(0x0000ff), 0.5);
    Rgb linear = rgb_mix(rgb_hex(0xff0000), rgb_hex(0x0000ff), 0.5);
    test_check(test, red_blue.r + red_blue.g + red_blue.b > linear.r + linear.g + linear.b,
               "the middle of red and blue is not the muddy sRGB one");
}

static void test_dither(Test *test)
{
    Rgb between = rgb_mix(xterm_to_rgb(16 + 36), xterm_to_rgb(16 + 72), 0.5);
    int high = 0;
    bool palette = true;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            Rgb cell = rgb_dither(between, COLOR_DEPTH_256, x, y);
            high += rgb_equal(cell, xterm_to_rgb(16 + 72));
            palette = palette && (rgb_equal(cell, xterm_to_rgb(16 + 72)) || rgb_equal(cell, xterm_to_rgb(16 + 36)));
        }
    }
    test_check(test, palette && high == 8, "halfway between two levels, half the cells pick each");
    Rgb grey = rgb_hex(0x535353);
    test_check(test, rgb_to_xterm(rgb_dither(grey, COLOR_DEPTH_256, 1, 0)) >= 232, "a grey stays on the grey ramp");
    int black = 0;
    int white = 0;
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 4; y++) {
            black += rgb_equal(rgb_dither(rgb_hex(0x050505), COLOR_DEPTH_256, x, y), xterm_to_rgb(16));
            white += rgb_equal(rgb_dither(rgb_hex(0xf6f6f6), COLOR_DEPTH_256, x, y), xterm_to_rgb(231));
        }
    }
    test_check(test, black > 0 && black < 16 && white > 0 && white < 16,
               "the ends of the grey ramp dither with black and white");
    Rgb exact = xterm_to_rgb(16 + 36 * 2 + 6 * 3 + 4);
    bool same = true;
    for (int x = 0; x < 4; x++) {
        same = same && rgb_equal(rgb_dither(exact, COLOR_DEPTH_256, x, 0), exact);
    }
    test_check(test, same, "a palette colour does not dither");
    test_check(test, rgb_equal(rgb_dither(between, COLOR_DEPTH_TRUE, 1, 2), between) &&
                         rgb_equal(rgb_dither(between, COLOR_DEPTH_16, 1, 2), between),
               "other depths keep the colour");
}

const TestCase TERM_STYLE_TESTS[] = {
    { "color_depth", test_color_depth },
    { "style_per_depth", test_style_per_depth },
    { "gradient", test_gradient },
    { "mix_oklab", test_mix_oklab },
    { "dither", test_dither },
    { nullptr, nullptr },
};
