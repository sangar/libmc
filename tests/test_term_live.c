#include "mc/term/live.h"
#include "test.h"

static String render_live(Test *test, Live *live, String frame, size_t columns)
{
    StringBuilder out = str_builder_create(test->arena, 64);
    live_render(live, &out, frame, columns);
    return str_builder_finish(&out);
}

static String finish(Test *test, Live *live)
{
    StringBuilder out = str_builder_create(test->arena, 64);
    live_finish(live, &out);
    return str_builder_finish(&out);
}

static void test_redraws_changed_lines(Test *test)
{
    Live *live = live_create(test->arena, true);
    test_check_str(test, render_live(test, live, S("æøå\nb\n"), 80),
                   S("\x1b[?2026h\x1b[?25l\x1b[2Kæøå\r\n\x1b[2Kb\r\n\x1b[?2026l"), "first frame");
    test_check_str(test, render_live(test, live, S("æøå\nc"), 80),
                   S("\x1b[?2026h\x1b[2F\x1b[1E\x1b[2Kc\r\n\x1b[?2026l"), "skips the unchanged line");
    test_check_str(test, render_live(test, live, S("æøå\nc"), 80), S(""), "an unchanged frame writes nothing");
    test_check_str(test, render_live(test, live, S("æøå"), 80), S("\x1b[?2026h\x1b[2F\x1b[1E\x1b[J\x1b[?2026l"),
                   "erases what a shorter frame leaves");
    test_check_str(test, render_live(test, live, S("ÆØÅ"), 2), S("\x1b[?2026h\x1b[1F\x1b[2KÆØ\r\n\x1b[?2026l"),
                   "cuts lines to the width");
    test_check_str(test, finish(test, live), S("\x1b[?25h"), "finish shows the cursor");
}

static void test_clear(Test *test)
{
    Live *live = live_create(test->arena, true);
    render_live(test, live, S("æ\nø"), 80);
    StringBuilder out = str_builder_create(test->arena, 64);
    live_clear(live, &out);
    test_check_str(test, str_builder_finish(&out), S("\x1b[?2026h\x1b[2F\x1b[J\x1b[?2026l\x1b[?25h"),
                   "erases the frame and shows the cursor");
}

static void test_not_interactive(Test *test)
{
    Live *live = live_create(test->arena, false);
    test_check_str(test, render_live(test, live, S("a\nb"), 80), S(""), "draws nothing while running");
    render_live(test, live, S("æ\nø"), 80);
    test_check_str(test, finish(test, live), S("æ\nø\n"), "finish writes the last frame");
}

const TestCase TERM_LIVE_TESTS[] = {
    { "redraws_changed_lines", test_redraws_changed_lines },
    { "not_interactive", test_not_interactive },
    { "clear", test_clear },
    { nullptr, nullptr },
};
