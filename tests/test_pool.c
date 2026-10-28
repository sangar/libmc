#include "test.h"

#include "mc/container/pool.h"

typedef struct {
    int value;
    char name[12];
} Item;

static void pool_reuses_freed_slots_under_new_handles(Test *test)
{
    Pool *pool = pool_create(test->arena, sizeof(Item));
    PoolHandle first = pool_alloc(pool);
    PoolHandle second = pool_alloc(pool);
    ((Item *)pool_get(pool, first))->value = 1;
    ((Item *)pool_get(pool, second))->value = 2;

    test_check(test, pool_free(pool, first), "free a live item");
    test_check(test, !pool_free(pool, first), "freeing it twice does nothing");
    test_check(test, pool_get(pool, first) == nullptr, "a freed item is gone");

    PoolHandle reused = pool_alloc(pool);
    Item *item = pool_get(pool, reused);
    test_check(test, reused.index == first.index && item->value == 0, "the freed slot comes back zeroed");
    test_check(test, pool_get(pool, first) == nullptr, "the stale handle does not find the new item");
    test_check(test, ((Item *)pool_get(pool, second))->value == 2, "other items stay");
    test_check(test, pool_get(pool, (PoolHandle){ 0 }) == nullptr, "the zero handle names no item");
}

static void pool_items_survive_growth(Test *test)
{
    enum { COUNT = 1000 };
    Pool *pool = pool_create(test->arena, sizeof(Item));
    PoolHandle handles[COUNT];
    for (int i = 0; i < COUNT; i++) {
        handles[i] = pool_alloc(pool);
        ((Item *)pool_get(pool, handles[i]))->value = i;
    }
    bool all_kept = true;
    for (int i = 0; i < COUNT; i++) {
        all_kept &= ((Item *)pool_get(pool, handles[i]))->value == i;
    }
    test_check(test, all_kept, "every item keeps its value");
}

const TestCase POOL_TESTS[] = {
    { "pool_reuses_freed_slots_under_new_handles", pool_reuses_freed_slots_under_new_handles },
    { "pool_items_survive_growth", pool_items_survive_growth },
    { nullptr, nullptr },
};
