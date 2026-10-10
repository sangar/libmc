#include "test.h"

#include <string.h>

static void arena_grows_past_block_size(Test *test)
{
    Arena *arena = arena_create(64);
    char *small = arena_push(arena, 16);
    char *large = arena_push(arena, 1000);
    memset(large, 'x', 1000);
    small[0] = 'a';
    test_check(test, small[0] == 'a' && large[999] == 'x', "allocations beyond one block are usable");
    test_check(test, arena_bytes_used(arena) >= 1016, "bytes used counts every block");
    arena_destroy(arena);
}

static void arena_mark_release_reuses_memory(Test *test)
{
    Arena *arena = arena_create(128);
    arena_push(arena, 32);
    ArenaMark mark = arena_mark(arena);
    void *first = arena_push(arena, 16);
    arena_push(arena, 500);
    arena_release(mark);
    void *second = arena_push(arena, 16);
    test_check(test, first == second, "release returns to the marked position");
    test_check(test, arena_bytes_used(arena) == 48, "later blocks are emptied on release");
    arena_destroy(arena);
}

static bool is_zero(const unsigned char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

static void arena_clears_only_what_was_used(Test *test)
{
    Arena *arena = arena_create(4096);
    ArenaMark mark = arena_mark(arena);
    unsigned char *bytes = arena_push(arena, 100);
    memset(bytes, 0xff, 100);
    arena_release(mark);
    bytes = arena_push(arena, 300);
    test_check(test, is_zero(bytes, 300), "a push across what was used before is zero throughout");
    memset(bytes, 0xff, 300);
    arena_release(mark);
    bytes = arena_push(arena, 50);
    unsigned char *after = arena_push(arena, 200);
    test_check(test, is_zero(bytes, 50) && is_zero(after, 200), "pushes inside used memory are zero");
    unsigned char *large = arena_push(arena, 1 << 20);
    memset(large, 0xff, 1 << 20);
    arena_reset(arena);
    arena_push(arena, 4000);
    large = arena_push(arena, 1 << 20);
    test_check(test, is_zero(large, 1 << 20), "a reused large block is zero");
    arena_destroy(arena);
}

static void arena_memory_is_zeroed_and_aligned(Test *test)
{
    Arena *arena = arena_create(256);
    unsigned char *bytes = arena_push(arena, 40);
    memset(bytes, 0xff, 40);
    arena_reset(arena);
    bytes = arena_push(arena, 40);
    bool all_zero = true;
    for (size_t i = 0; i < 40; i++) {
        all_zero = all_zero && bytes[i] == 0;
    }
    test_check(test, all_zero, "reused memory comes back zeroed");
    arena_push_aligned(arena, 1, 1);
    void *aligned = arena_push_aligned(arena, 8, 64);
    test_check(test, (uintptr_t)aligned % 64 == 0, "push_aligned honours the alignment");
    test_check(test, (uintptr_t)arena_push(arena, 1) % alignof(max_align_t) == 0, "push aligns for any type");
    arena_destroy(arena);
}

static void arena_grow_doubles_and_keeps_items(Test *test)
{
    Arena *arena = arena_create(0);
    int *items = nullptr;
    size_t count = 0;
    size_t capacity = 0;
    for (int i = 0; i < 100; i++) {
        items = arena_grow(arena, items, &capacity, count, sizeof *items);
        items[count++] = i;
    }
    test_check(test, capacity == 128, "capacity doubles from 8");
    test_check(test, items[0] == 0 && items[99] == 99, "items survive growth");
    arena_destroy(arena);
}

static void repeat_past_size_max(void)
{
    Arena *arena = arena_create(0);
    str_repeat(arena, S("ab"), SIZE_MAX / 2 + 1);
}

static void concat_past_size_max(void)
{
    Arena *arena = arena_create(0);
    String huge = { .data = "x", .len = SIZE_MAX };
    str_concat(arena, huge, S("y"));
}

static void push_size_max(void)
{
    Arena *arena = arena_create(0);
    arena_push(arena, SIZE_MAX);
}

static void grow_past_size_max(void)
{
    Arena *arena = arena_create(0);
    size_t capacity = SIZE_MAX / 2 + 1;
    arena_grow(arena, nullptr, &capacity, capacity, 1);
}

static void overflowing_sizes_abort(Test *test)
{
    test_check_aborts(test, "repeat_past_size_max", "str_repeat whose length overflows");
    test_check_aborts(test, "concat_past_size_max", "str_concat whose length overflows");
    test_check_aborts(test, "push_size_max", "arena_push whose block size overflows");
    test_check_aborts(test, "grow_past_size_max", "arena_grow whose capacity overflows");
}

const AbortCase ABORT_CASES[] = {
    { "repeat_past_size_max", repeat_past_size_max },
    { "concat_past_size_max", concat_past_size_max },
    { "push_size_max", push_size_max },
    { "grow_past_size_max", grow_past_size_max },
    { nullptr, nullptr },
};

const TestCase ARENA_TESTS[] = {
    { "arena_grows_past_block_size", arena_grows_past_block_size },
    { "arena_mark_release_reuses_memory", arena_mark_release_reuses_memory },
    { "arena_memory_is_zeroed_and_aligned", arena_memory_is_zeroed_and_aligned },
    { "arena_clears_only_what_was_used", arena_clears_only_what_was_used },
    { "arena_grow_doubles_and_keeps_items", arena_grow_doubles_and_keeps_items },
    { "overflowing_sizes_abort", overflowing_sizes_abort },
    { nullptr, nullptr },
};
