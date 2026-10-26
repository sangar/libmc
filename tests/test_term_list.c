#include "mc/term/list.h"
#include "test.h"

static Event key(Key k)
{
    return (Event){ .key = k };
}

static Event ctrl(uint32_t codepoint)
{
    return (Event){ .key = KEY_CHAR, .codepoint = codepoint, .ctrl = true };
}

static List three_rows_of_five(Test *test)
{
    const char *const items[] = { "æble", "øl", "ål", "fisk", "brød" };
    return (List){ .items = strlist_of(test->arena, countof(items), items), .height = 3 };
}

static void test_scrolls_with_cursor(Test *test)
{
    List list = three_rows_of_five(test);
    for (int i = 0; i < 3; i++) {
        list_handle(&list, key(KEY_DOWN));
    }
    test_check(test, list.cursor == 3 && list.offset == 1, "scrolls down to keep the cursor in view");
    list_handle(&list, key(KEY_END));
    list_handle(&list, key(KEY_DOWN));
    test_check(test, list.cursor == 4 && list.offset == 2, "stops at the last item");
    StringBuilder out = str_builder_create(test->arena, 64);
    list_append(&out, COLOR_DEPTH_NONE, &list, (Rgb){ 0 });
    test_check_str(test, str_builder_finish(&out), S("  ål\n  fisk\n> brød"), "draws the rows in view");
    list_handle(&list, key(KEY_HOME));
    test_check(test, list.cursor == 0 && list.offset == 0, "home scrolls back to the top");
}

static void test_select_and_cancel(Test *test)
{
    List list = three_rows_of_five(test);
    test_check(test, list_handle(&list, key(KEY_ENTER)) == LIST_SELECTED, "enter selects");
    test_check(test, list_handle(&list, ctrl('c')) == LIST_CANCELLED, "ctrl-c cancels");
    test_check(test, list_handle(&list, key(KEY_ESCAPE)) == LIST_CANCELLED, "escape cancels");
    List empty = { .height = 3 };
    test_check(test, list_handle(&empty, key(KEY_ENTER)) == LIST_CONTINUE, "an empty list selects nothing");
}

const TestCase TERM_LIST_TESTS[] = {
    { "scrolls_with_cursor", test_scrolls_with_cursor },
    { "select_and_cancel", test_select_and_cancel },
    { nullptr, nullptr },
};
