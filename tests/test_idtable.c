#include "test.h"

#include "mc/container/hash.h"
#include "mc/container/idtable.h"

// Keys are the strings of a list and ids their positions.
typedef struct {
    const String *keys;
    String wanted;
} Lookup;

static uint64_t hash_key(void *context, uint32_t id)
{
    const Lookup *lookup = context;
    return hash_string(lookup->keys[id]);
}

static bool key_equal(void *context, uint32_t id)
{
    const Lookup *lookup = context;
    return str_equal(lookup->keys[id], lookup->wanted);
}

static uint32_t find(const IdTable *table, Lookup *lookup, String key)
{
    lookup->wanted = key;
    return idtable_find(table, hash_string(key), key_equal, lookup);
}

static void ids_are_found_by_their_keys(Test *test)
{
    Arena *a = test->arena;
    enum { COUNT = 5000 };
    String *keys = arena_push(a, COUNT * sizeof *keys);
    for (uint32_t i = 0; i < COUNT; i++) {
        keys[i] = str_format(a, "key-%u", i);
    }
    Lookup lookup = { .keys = keys };
    IdTable *table = idtable_create(a, 4);
    for (uint32_t i = 0; i < COUNT; i++) {
        idtable_insert(table, hash_string(keys[i]), i, hash_key, &lookup);
    }
    test_check(test, idtable_count(table) == COUNT, "every id is in, through several grows");
    bool all_found = true;
    for (uint32_t i = 0; i < COUNT; i++) {
        all_found &= find(table, &lookup, keys[i]) == i;
    }
    test_check(test, all_found, "each key finds its id");
    test_check(test, find(table, &lookup, S("absent")) == IDTABLE_NONE, "a missing key");

    test_check(test, idtable_remove(table, hash_string(keys[7]), 7), "remove an id");
    test_check(test, !idtable_remove(table, hash_string(keys[7]), 7), "removing it again");
    test_check(test, find(table, &lookup, keys[7]) == IDTABLE_NONE, "a removed id is gone");
    test_check(test, find(table, &lookup, keys[8]) == 8, "its neighbours stay");
    idtable_insert(table, hash_string(keys[7]), 7, hash_key, &lookup);
    test_check(test, find(table, &lookup, keys[7]) == 7 && idtable_count(table) == COUNT, "insert it again");
}

const TestCase IDTABLE_TESTS[] = {
    { "ids_are_found_by_their_keys", ids_are_found_by_their_keys },
    { nullptr, nullptr },
};
