#include "mc/term/ansi.h"
#include "test.h"

static void test_width(Test *test)
{
    test_check(test, ansi_width(S("\x1b[1mæøå\x1b[0m")) == 3, "colours take no columns");
    test_check(test, ansi_width(S("a\xcc\x8a")) == 1, "decomposed å is one column");
    test_check(test, ansi_width(S("日本")) == 4, "wide characters take two");
    test_check(test, ansi_width(S("\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\")) == 4,
               "hyperlinks take no columns");
}

static void test_truncate(Test *test)
{
    test_check_str(test, ansi_truncate(S("\x1b[1mÆØÅ\x1b[0m"), 2), S("\x1b[1mÆØ"), "cuts after two columns");
    test_check_str(test, ansi_truncate(S("日本"), 3), S("日"), "never halves a wide character");
    test_check_str(test, ansi_truncate(S("øa\xcc\x8a!"), 2), S("øa\xcc\x8a"), "keeps the combining ring");
    test_check_str(test, ansi_truncate(S("æøå"), 5), S("æøå"), "short text is whole");
}

const TestCase TERM_ANSI_TESTS[] = {
    { "width", test_width },
    { "truncate", test_truncate },
    { nullptr, nullptr },
};
