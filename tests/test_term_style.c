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

const TestCase TERM_STYLE_TESTS[] = {
    { "color_depth", test_color_depth },
    { "style_per_depth", test_style_per_depth },
    { "gradient", test_gradient },
    { nullptr, nullptr },
};
