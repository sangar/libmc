#include "test.h"

#include "mc/term/draw.h"
#include "mc/term/layout.h"
#include "mc/term/progress.h"
#include "mc/term/screen.h"
#include "mc/text/utf8.h"

static const Rgb WHITE = { 255, 255, 255 };
static const Rgb BLACK = { 0, 0, 0 };

static String render(Test *test, Screen *screen)
{
    StringBuilder out = str_builder_create(test->arena, 256);
    screen_render(screen, &out);
    return str_builder_finish(&out);
}

static void draw_text(Screen *screen, const char *text)
{
    screen_paint(screen, (Rect) { 0, 0, screen_width(screen), screen_height(screen) }, BLACK);
    screen_text(screen, 0, 0, screen_width(screen), S(text), WHITE, 0);
}

static void screen_writes_only_what_changed(Test *test)
{
    Screen *screen = screen_create(COLOR_DEPTH_TRUE);
    screen_resize(screen, 4, 2);
    draw_text(screen, "ab");
    String first = render(test, screen);
    test_check(test, str_contains(first, S("ab")) && str_contains(first, S("38;2;255;255;255")),
               "the first frame paints everything in true colour");
    draw_text(screen, "ac");
    String second = render(test, screen);
    test_check(test, str_starts_with(second, S("\x1b[1;2H")) && str_ends_with(second, S("c")) &&
                         !str_contains(second, S("a")),
               "the next frame moves to the changed cell and writes it");
    draw_text(screen, "ac");
    test_check(test, render(test, screen).len == 0, "an unchanged frame writes nothing");
    screen_destroy(screen);

    screen = screen_create(COLOR_DEPTH_256);
    screen_resize(screen, 2, 1);
    draw_text(screen, "x");
    test_check(test, str_contains(render(test, screen), S("38;5;231")), "the 256-colour palette");
    screen_destroy(screen);
}

static void screen_keeps_wide_glyphs_whole(Test *test)
{
    Screen *screen = screen_create(COLOR_DEPTH_TRUE);
    screen_resize(screen, 3, 1);
    test_check(test, screen_text(screen, 0, 0, 3, S("界"), WHITE, 0) == 2, "a wide glyph takes two columns");
    test_check(test, screen_cell(screen, 1, 0)->glyph == 0, "the second column is its tail");
    screen_glyph(screen, 1, 0, 'x', WHITE, 0);
    test_check(test, screen_cell(screen, 0, 0)->glyph == ' ', "overwriting the tail blanks the head");
    test_check(test, screen_text(screen, 2, 0, 1, S("界"), WHITE, 0) == 0, "a wide glyph does not fit in one column");
    screen_destroy(screen);
}

static void graphs_plot_in_braille(Test *test)
{
    Screen *screen = screen_create(COLOR_DEPTH_TRUE);
    screen_resize(screen, 1, 2);
    Gradient gradient = { BLACK, BLACK, WHITE };
    History history = { 0 };
    history_push(&history, 0.25f);
    history_push(&history, 1.0f);
    draw_graph(screen, (Rect) { 0, 0, 1, 2 }, &history, 1.0f, gradient, false);
    // The newest sample fills the right column, the one before it a quarter of the left.
    test_check(test, screen_cell(screen, 0, 1)->glyph == 0x2800 + (0xB8 | 0x44), "the bottom cell");
    test_check(test, screen_cell(screen, 0, 0)->glyph == 0x2800 + 0xB8, "the top cell");
    test_check(test, rgb_equal(screen_cell(screen, 0, 0)->fg, gradient_at(gradient, 0.75f)), "coloured by height");
    screen_paint(screen, (Rect) { 0, 0, 1, 2 }, BLACK);
    draw_graph(screen, (Rect) { 0, 0, 1, 2 }, &history, 4.0f, gradient, true);
    test_check(test, screen_cell(screen, 0, 0)->glyph == 0x2800 + (0x08 | 0x10 | 0x01), "a downward graph hangs");
    screen_destroy(screen);
}

// Strings styled for the inline output draw into cells: a box from
// box_render keeps its colours and the backgrounds around it.
static void screen_draws_styled_text(Test *test)
{
    Screen *screen = screen_create(COLOR_DEPTH_TRUE);
    screen_resize(screen, 6, 1);
    screen_paint(screen, (Rect){ 0, 0, 6, 1 }, BLACK);
    Rgb red = rgb_hex(0xff0000);
    StringBuilder out = str_builder_create(test->arena, 64);
    style_append(&out, COLOR_DEPTH_TRUE, (Style){ .fg = red, .has_fg = true, .bold = true }, S("æ"));
    str_builder_append(&out, S("ø\x1b[44må"));
    test_check(test, screen_styled(screen, 0, 0, 6, str_builder_finish(&out), WHITE, 0) == 3, "three columns");
    Cell *ae = screen_cell(screen, 0, 0);
    Cell *oe = screen_cell(screen, 1, 0);
    Cell *aa = screen_cell(screen, 2, 0);
    test_check(test, ae->glyph == 0xe6 && rgb_equal(ae->fg, red) && ae->attrs == ATTR_BOLD, "styled");
    test_check(test, rgb_equal(oe->fg, WHITE) && oe->attrs == 0 && rgb_equal(oe->bg, BLACK), "reset to plain");
    test_check(test, rgb_equal(aa->bg, xterm_to_rgb(4)), "a 16-colour background");

    String box = box_render(test->arena, COLOR_DEPTH_TRUE, (Box){ .border = BORDER_ROUNDED }, S("ø"));
    StringList lines = str_split(test->arena, box, '\n');
    for (size_t row = 0; row < lines.count; row++) {
        screen_styled(screen, 0, 0, 6, lines.items[row], WHITE, 0);
    }
    test_check(test, screen_cell(screen, 0, 0)->glyph == 0x2570 && screen_cell(screen, 2, 0)->glyph == 0x256F,
               "a box's lines draw as cells");
    screen_destroy(screen);
}

static String row_text(Test *test, Screen *screen, int y)
{
    StringBuilder out = str_builder_create(test->arena, 64);
    for (int x = 0; x < screen_width(screen); x++) {
        uint32_t glyph = screen_cell(screen, x, y)->glyph;
        char bytes[UTF8_MAX_BYTES];
        str_builder_append(&out, (String){ bytes, glyph != 0 ? utf8_encode(glyph, bytes) : 0 });
    }
    return str_builder_finish(&out);
}

static void screen_wraps_log_lines(Test *test)
{
    Screen *screen = screen_create(COLOR_DEPTH_TRUE);
    screen_resize(screen, 4, 3);
    screen_paint(screen, (Rect){ 0, 0, 4, 3 }, BLACK);
    String line = S("\x1b[31mabcdefg\x1b[0m界");
    test_check(test, wrapped_rows(line, 4) == 3, "the wide glyph goes to the next row whole");
    TextStyle style = { .fg = WHITE };
    test_check(test, screen_wrapped(screen, (Rect){ 0, 0, 4, 3 }, 0, line, style) == 3, "three rows drawn");
    test_check_str(test, row_text(test, screen, 0), S("abcd"), "the first row");
    test_check_str(test, row_text(test, screen, 1), S("efg "), "the rest of the text wraps");
    test_check_str(test, row_text(test, screen, 2), S("界  "), "and the wide glyph that does not fit");
    test_check(test, rgb_equal(screen_cell(screen, 0, 1)->fg, xterm_to_rgb(1)), "colour carries over the wrap");

    screen_paint(screen, (Rect){ 0, 0, 4, 3 }, BLACK);
    test_check(test, screen_wrapped(screen, (Rect){ 0, 0, 4, 1 }, 1, line, style) == 1, "rows past rect stay out");
    test_check_str(test, row_text(test, screen, 0), S("efg "), "skipped rows stay out");

    int row, column;
    wrapped_end(S("abcd"), 4, &row, &column);
    test_check(test, row == 1 && column == 0, "a full row puts the end on the next");
    wrapped_end(S("a\tb"), 20, &row, &column);
    test_check(test, row == 0 && column == 9, "a tab goes to the eighth column");
    screen_destroy(screen);
}

static void screen_wraps_with_the_theme_colours(Test *test)
{
    Screen *screen = screen_create(COLOR_DEPTH_TRUE);
    screen_resize(screen, 3, 1);
    screen_paint(screen, (Rect){ 0, 0, 3, 1 }, BLACK);
    Rgb ansi[16] = { 0 };
    ansi[2] = rgb_hex(0x00aa55);
    TextStyle style = { .fg = WHITE, .ansi = ansi };
    screen_wrapped(screen, (Rect){ 0, 0, 3, 1 }, 0, S("\x1b[32ma\x1b[7mb\x1b[0mc"), style);
    Cell *a = screen_cell(screen, 0, 0);
    Cell *b = screen_cell(screen, 1, 0);
    Cell *c = screen_cell(screen, 2, 0);
    test_check(test, rgb_equal(a->fg, ansi[2]), "a basic colour comes from the palette");
    test_check(test, rgb_equal(b->fg, BLACK) && rgb_equal(b->bg, ansi[2]), "reverse video swaps the colours");
    test_check(test, rgb_equal(c->fg, WHITE) && rgb_equal(c->bg, BLACK), "a reset ends it");
    screen_destroy(screen);
}

static void rects_hold_their_cells(Test *test)
{
    Rect rect = { 2, 3, 4, 5 };
    Rect inner = rect_inner(rect);
    test_check(test, inner.x == 3 && inner.y == 4 && inner.width == 2 && inner.height == 3, "inside the border");
    test_check(test, rect_contains(rect, 2, 3) && rect_contains(rect, 5, 7), "the corners are in");
    test_check(test, !rect_contains(rect, 6, 3) && !rect_contains(rect, 2, 8), "the far edges are out");
}

const TestCase TERM_SCREEN_TESTS[] = {
    { "screen_writes_only_what_changed", screen_writes_only_what_changed },
    { "screen_keeps_wide_glyphs_whole", screen_keeps_wide_glyphs_whole },
    { "graphs_plot_in_braille", graphs_plot_in_braille },
    { "screen_draws_styled_text", screen_draws_styled_text },
    { "screen_wraps_log_lines", screen_wraps_log_lines },
    { "screen_wraps_with_the_theme_colours", screen_wraps_with_the_theme_colours },
    { "rects_hold_their_cells", rects_hold_their_cells },
    { nullptr, nullptr },
};
