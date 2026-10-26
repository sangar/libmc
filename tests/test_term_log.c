#include "test.h"

#include "mc/term/ansi.h"
#include "mc/term/logbuf.h"

static void logbuf_splits_terminal_output_into_lines(Test *test)
{
    LogBuffer *log = logbuf_create(100, 1 << 20);
    logbuf_write(log, S("one\r\ntw"));
    logbuf_write(log, S("o\r"));
    logbuf_write(log, S("\nthree"));
    test_check(test, logbuf_end(log) == 2, "two finished lines");
    test_check_str(test, logbuf_line(log, 0), S("one"), "CR LF ends a line");
    test_check_str(test, logbuf_line(log, 1), S("two"), "even when split between writes");
    test_check_str(test, logbuf_partial(log), S("three"), "an unfinished line waits");
    logbuf_destroy(log);
}

static void logbuf_keeps_colour_and_drops_other_control(Test *test)
{
    LogBuffer *log = logbuf_create(100, 1 << 20);
    logbuf_write(log, S("\x1b[31mred\x1b[0m\x1b[2K\x1b]0;title\x07 ok\n"));
    logbuf_write(log, S("downloading 10%\rdownloading 99%\rdone\n"));
    logbuf_write(log, S("\x1b[1G\x1b[0K⠋ spin\x1b[1G\x1b[0K⠙ spun\n"));
    test_check_str(test, logbuf_line(log, 0), S("\x1b[31mred\x1b[0m ok"), "SGR stays, erase and titles go");
    test_check_str(test, logbuf_line(log, 1), S("done"), "a carriage return rewrites the line");
    test_check_str(test, logbuf_line(log, 2), S("⠙ spun"), "so does moving to the first column");
    test_check_str(test, ansi_strip(test->arena, logbuf_line(log, 0)), S("red ok"), "plain text has no escapes");
    logbuf_destroy(log);
}

static void logbuf_forgets_the_oldest_lines(Test *test)
{
    LogBuffer *log = logbuf_create(3, 1 << 20);
    logbuf_write(log, S("a\nb\nc\nd\n"));
    test_check(test, logbuf_first(log) == 1 && logbuf_end(log) == 4, "only max_lines are kept");
    test_check_str(test, logbuf_line(log, 1), S("b"), "lines keep their numbers");
    logbuf_destroy(log);

    log = logbuf_create(1000, 64);
    for (int i = 0; i < 100; i++) {
        logbuf_write(log, S("0123456789\n"));
    }
    logbuf_note(log, S("newest"));
    test_check(test, logbuf_end(log) - logbuf_first(log) < 10, "the byte budget drops old lines too");
    test_check_str(test, logbuf_line(log, logbuf_end(log) - 1), S("newest"), "and keeps the newest");
    logbuf_destroy(log);
}

const TestCase TERM_LOG_TESTS[] = {
    { "logbuf_splits_terminal_output_into_lines", logbuf_splits_terminal_output_into_lines },
    { "logbuf_keeps_colour_and_drops_other_control", logbuf_keeps_colour_and_drops_other_control },
    { "logbuf_forgets_the_oldest_lines", logbuf_forgets_the_oldest_lines },
    { nullptr, nullptr },
};
