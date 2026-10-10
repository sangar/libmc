#include "test.h"

#include "mc/container/strmap.h"

static void strmap_put_get_replace(Test *test)
{
    StrMap *map = strmap_create(test->arena, 0);
    int one = 1;
    int two = 2;
    test_check(test, strmap_put(map, S("a"), &one) == nullptr, "first put has no previous value");
    test_check(test, strmap_put(map, S("a"), &two) == &one, "replacing returns the previous value");
    test_check(test, strmap_get(map, S("a")) == &two && strmap_count(map) == 1, "get sees the replacement");
    test_check(test, strmap_get(map, S("b")) == nullptr && !strmap_has(map, S("b")), "absent key");
}

static void strmap_grows_and_removes(Test *test)
{
    StrMap *map = strmap_create(test->arena, 4);
    int *values = arena_push(test->arena, 1000 * sizeof *values);
    for (int i = 0; i < 1000; i++) {
        values[i] = i;
        strmap_put(map, str_format(test->arena, "key%d", i), &values[i]);
    }
    bool all_found = true;
    for (int i = 0; i < 1000; i++) {
        int *found = strmap_get(map, str_format(test->arena, "key%d", i));
        all_found = all_found && found != nullptr && *found == i;
    }
    test_check(test, all_found && strmap_count(map) == 1000, "every key survives growth");
    for (int i = 0; i < 1000; i += 2) {
        strmap_remove(map, str_format(test->arena, "key%d", i), nullptr);
    }
    void *removed = nullptr;
    test_check(test, strmap_remove(map, S("key1"), &removed) && removed == &values[1], "remove returns the value");
    test_check(test, !strmap_remove(map, S("key1"), nullptr), "a second remove finds nothing");
    test_check(test, strmap_count(map) == 499 && strmap_has(map, S("key3")) && !strmap_has(map, S("key4")),
               "removal leaves the other keys");
    for (int round = 0; round < 50; round++) {
        strmap_put(map, S("churn"), &values[0]);
        strmap_remove(map, S("churn"), nullptr);
    }
    size_t walked = 0;
    StrMapIterator iterator = strmap_iterate(map);
    String key;
    void *value;
    while (strmap_next(&iterator, &key, &value)) {
        walked++;
    }
    test_check(test, walked == 499, "iteration visits each live entry once");
}

const TestCase STRMAP_TESTS[] = {
    { "strmap_put_get_replace", strmap_put_get_replace },
    { "strmap_grows_and_removes", strmap_grows_and_removes },
    { nullptr, nullptr },
};
