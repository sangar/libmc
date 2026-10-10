#include "test.h"

static void str_slicing_and_search(Test *test)
{
    String s = S("documents/report.txt");
    size_t index = 0;
    test_check(test, str_find_char(s, '.', &index) && index == 16, "find_char locates the dot");
    test_check_str(test, str_slice(s, index + 1, s.len), S("txt"), "slice after the dot");
    test_check(test, str_find(s, S("report"), &index) && index == 10, "find locates a substring");
    test_check(test, !str_find(s, S("missing"), &index), "find reports absence");
    test_check(test, str_find_last_char(S("a/b/c"), '/', &index) && index == 3, "find_last_char");
    test_check(test, str_starts_with(s, S("doc")) && str_ends_with(s, S(".txt")), "prefix and suffix");
    test_check(test, str_slice(s, 50, 60).len == 0, "slice clamps out-of-range");
    test_check(test, str_compare(S("ab"), S("abc")) < 0 && str_compare(S("b"), S("abc")) > 0, "compare orders");
    test_check(test, str_compare_ignore_case(S("Readme"), S("rat")) > 0 && str_compare_ignore_case(S("Readme"), S("rest")) < 0,
               "compare ignoring case folds letters");
    test_check(test, str_compare_ignore_case(S("ABC"), S("abc")) == 0 && str_compare_ignore_case(S("ab"), S("ABC")) < 0,
               "a folded prefix sorts first");
}

static void str_find_edges(Test *test)
{
    size_t index = 0;
    test_check(test, str_find(S("aaab"), S("aab"), &index) && index == 1, "find past a partial match");
    test_check(test, str_find(S("abcabd"), S("abd"), &index) && index == 3, "find after a failed first byte");
    test_check(test, str_find(S("xyz"), S("xyz"), &index) && index == 0, "find the whole string");
    test_check(test, str_find(S("a.txt"), S("txt"), &index) && index == 2, "find at the very end");
    test_check(test, !str_find(S("ab"), S("abc"), &index), "find a needle longer than the string");
    test_check(test, !str_find(S(""), S("a"), &index), "find in an empty string");
    test_check(test, !str_find(S("abab"), S("ba "), &index), "find stops before running off the end");
    test_check(test, str_find(S("abc"), S(""), &index) && index == 0, "find an empty needle");
}

static void str_cut_trim_case(Test *test)
{
    String head;
    String tail;
    test_check(test, str_cut(S("Content-Type: text/html"), ':', &head, &tail), "cut finds the separator");
    test_check_str(test, head, S("Content-Type"), "cut head");
    test_check_str(test, str_trim(tail), S("text/html"), "trim removes surrounding space");
    test_check(test, !str_cut(S("nosep"), ':', &head, &tail) && tail.len == 0, "cut without a separator");
    test_check(test, str_equal_ignore_case(S("TEXT/html"), S("text/HTML")), "ascii case-insensitive compare");
    test_check_str(test, str_lower_ascii(test->arena, S("Blå ÆØÅ")), S("blå ÆØÅ"),
                   "lower_ascii leaves non-ascii bytes alone");
}

static void str_parse_numbers(Test *test)
{
    uint64_t value = 0;
    test_check(test, str_parse_u64(S("12345"), &value) && value == 12345, "parses digits");
    test_check(test, !str_parse_u64(S("12a"), &value), "rejects non-digits");
    test_check(test, !str_parse_u64(S(""), &value), "rejects empty");
    test_check(test, !str_parse_u64(S("99999999999999999999"), &value), "rejects overflow");
    int64_t signed_value = 0;
    test_check(test, str_parse_i64(S("-42"), &signed_value) && signed_value == -42, "parses a negative number");
    test_check(test, str_parse_i64(S("-9223372036854775808"), &signed_value) && signed_value == INT64_MIN,
               "parses the smallest int64");
    test_check(test, !str_parse_i64(S("9223372036854775808"), &signed_value), "rejects int64 overflow");
    test_check(test, !str_parse_i64(S("4x"), &signed_value), "rejects trailing junk");
}

static void str_builder_and_format(Test *test)
{
    StringBuilder builder = str_builder_create(test->arena, 4);
    str_builder_append(&builder, S("https://"));
    str_builder_append(&builder, S("www.nrk.no"));
    str_builder_append_char(&builder, '/');
    str_builder_append_format(&builder, "page/%d", 42);
    String built = str_builder_finish(&builder);
    test_check_str(test, built, S("https://www.nrk.no/page/42"), "builder grows and concatenates");
    test_check(test, built.data[built.len] == '\0', "builder result is NUL-terminated");
    test_check_str(test, str_format(test->arena, "%s=%zu", "pages", (size_t)100000), S("pages=100000"),
                   "format into the arena");
    test_check(test, str_copy(test->arena, S("abc")).data[3] == '\0', "copy is NUL-terminated");
}

static void str_split_join_replace(Test *test)
{
    StringList parts = str_split(test->arena, S("a,b,,c"), ',');
    test_check(test, parts.count == 4, "split keeps empty parts");
    test_check_str(test, str_join(test->arena, parts, S("|")), S("a|b||c"), "join");
    test_check_str(test, str_replace_all(test->arena, S("it's"), S("'"), S("'\\''")), S("it'\\''s"),
                   "replace_all");
    StringList fields = str_fields(test->arena, S("  one two\tthree  "));
    test_check(test, fields.count == 3, "fields splits on whitespace runs");
    test_check_str(test, fields.items[2], S("three"), "last field");
    test_check_str(test, str_quote(test->arena, S("a\"b\n")), S("\"a\\\"b\\n\""), "quote escapes");
    test_check_str(test, str_percent_encode(test->arena, S("a b/c~"), true), S("a%20b/c~"),
                   "percent_encode keeps unreserved and slash");
    test_check_str(test, str_percent_encode(test->arena, S("a/b"), false), S("a%2Fb"), "percent_encode slash");
}

static void strlist_sorts_and_contains(Test *test)
{
    const char *const names[] = { "pear", "apple", "fig" };
    StringList list = strlist_of(test->arena, countof(names), names);
    strlist_sort(&list);
    test_check_str(test, str_join(test->arena, list, S(",")), S("apple,fig,pear"), "sort orders names");
    test_check(test, strlist_contains(list, S("fig")) && !strlist_contains(list, S("kiwi")), "contains");
    test_check(test, strlist_from_lines(test->arena, S("")).count == 0, "no lines in an empty string");
}

const TestCase STR_TESTS[] = {
    { "str_slicing_and_search", str_slicing_and_search },
    { "str_find_edges", str_find_edges },
    { "str_cut_trim_case", str_cut_trim_case },
    { "str_parse_numbers", str_parse_numbers },
    { "str_builder_and_format", str_builder_and_format },
    { "str_split_join_replace", str_split_join_replace },
    { "strlist_sorts_and_contains", strlist_sorts_and_contains },
    { nullptr, nullptr },
};
