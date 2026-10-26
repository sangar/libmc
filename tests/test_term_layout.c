#include "mc/term/layout.h"
#include "test.h"

static void test_box(Test *test)
{
    Box box = { .border = BORDER_ROUNDED, .padding = { 0, 1, 0, 1 }, .align = ALIGN_CENTER };
    test_check_str(test, box_render(test->arena, COLOR_DEPTH_NONE, box, S("æøå\nÆ")),
                   S("╭─────╮\n│ æøå │\n│  Æ  │\n╰─────╯"), "border, padding and centring");

    Box narrow = { .width = 2, .margin = { 1, 0, 0, 2 } };
    test_check_str(test, box_render(test->arena, COLOR_DEPTH_NONE, narrow, S("ålesund")), S("    \n  ål"),
                   "margins, and lines cut to the width");

    Box filled = { .style = { .bg = rgb_hex(0x000000), .has_bg = true }, .padding = { 0, 1, 0, 1 } };
    test_check_str(test, box_render(test->arena, COLOR_DEPTH_TRUE, filled, S("ø")),
                   S("\x1b[0;48;2;0;0;0m ø \x1b[0m"), "the background fills the padding");
}

static void test_join(Test *test)
{
    const char *const blocks[] = { "a\nb\nc", "øl" };
    StringList list = strlist_of(test->arena, countof(blocks), blocks);
    test_check_str(test, join_horizontal(test->arena, list, ALIGN_CENTER), S("a  \nbøl\nc  "),
                   "side by side, the shorter in the middle");
    test_check_str(test, join_vertical(test->arena, list, ALIGN_END), S(" a\n b\n c\nøl"),
                   "one above the other, right-aligned");
}

static void test_rgb_parse(Test *test)
{
    Rgb color;
    test_check(test, rgb_parse(S("#ff87d7"), &color) && color.r == 0xff && color.g == 0x87 && color.b == 0xd7,
               "#rrggbb");
    test_check(test, rgb_parse(S("#f0a"), &color) && color.r == 0xff && color.g == 0x00 && color.b == 0xaa, "#rgb");
    test_check(test, rgb_parse(S("212"), &color) && color.r == 0xff && color.g == 0x87 && color.b == 0xd7,
               "an xterm number");
    test_check(test, !rgb_parse(S("256"), &color) && !rgb_parse(S("#ff87dz"), &color), "rejects the rest");
}

const TestCase TERM_LAYOUT_TESTS[] = {
    { "box", test_box },
    { "join", test_join },
    { "rgb_parse", test_rgb_parse },
    { nullptr, nullptr },
};
