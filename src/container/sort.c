#include "mc/container/sort.h"

#include <string.h>

enum { INSERTION_RUN = 16 };

static void merge_pass(const char *from, char *to, size_t count, size_t size, size_t width, CompareFn compare,
                       const void *context)
{
    for (size_t low = 0; low < count; low += 2 * width) {
        size_t middle = min_size(low + width, count);
        size_t high = min_size(low + 2 * width, count);
        size_t i = low;
        size_t j = middle;
        size_t k = low;
        while (i < middle && j < high) {
            if (compare(context, from + j * size, from + i * size) < 0) {
                memcpy(to + k++ * size, from + j++ * size, size);
            } else {
                memcpy(to + k++ * size, from + i++ * size, size);
            }
        }
        memcpy(to + k * size, from + i * size, (middle - i) * size);
        k += middle - i;
        memcpy(to + k * size, from + j * size, (high - j) * size);
    }
}

static void insertion_sort(char *base, size_t count, size_t size, CompareFn compare, const void *context, char *held)
{
    for (size_t i = 1; i < count; i++) {
        size_t j = i;
        memcpy(held, base + i * size, size);
        while (j > 0 && compare(context, held, base + (j - 1) * size) < 0) {
            memcpy(base + j * size, base + (j - 1) * size, size);
            j--;
        }
        memcpy(base + j * size, held, size);
    }
}

void sort_stable(Arena *scratch, void *base, size_t count, size_t size, CompareFn compare, const void *context)
{
    if (count < 2) {
        return;
    }
    ArenaMark mark = arena_mark(scratch);
    char *items = base;
    char *held = arena_push(scratch, size);
    for (size_t low = 0; low < count; low += INSERTION_RUN) {
        insertion_sort(items + low * size, min_size(INSERTION_RUN, count - low), size, compare, context, held);
    }
    if (count > INSERTION_RUN) {
        char *from = items;
        char *to = arena_push(scratch, arena_size_mul(count, size));
        for (size_t width = INSERTION_RUN; width < count; width *= 2) {
            merge_pass(from, to, count, size, width, compare, context);
            char *swap = from;
            from = to;
            to = swap;
        }
        if (from != items) {
            memcpy(items, from, count * size);
        }
    }
    arena_release(mark);
}

// The heap keeps the worst of the best k at its root.
static void sift_down(char *heap, size_t count, size_t i, size_t size, CompareFn compare, const void *context,
                      char *held)
{
    for (;;) {
        size_t worst = i;
        size_t left = 2 * i + 1;
        size_t right = left + 1;
        if (left < count && compare(context, heap + left * size, heap + worst * size) > 0) {
            worst = left;
        }
        if (right < count && compare(context, heap + right * size, heap + worst * size) > 0) {
            worst = right;
        }
        if (worst == i) {
            return;
        }
        memcpy(held, heap + i * size, size);
        memcpy(heap + i * size, heap + worst * size, size);
        memcpy(heap + worst * size, held, size);
        i = worst;
    }
}

size_t sort_top(Arena *scratch, void *base, size_t count, size_t size, size_t k, CompareFn compare,
                const void *context)
{
    if (k >= count || k * 4 > count) {
        sort_stable(scratch, base, count, size, compare, context);
        return min_size(k, count);
    }
    if (k == 0) {
        return 0;
    }
    ArenaMark mark = arena_mark(scratch);
    char *items = base;
    char *held = arena_push(scratch, size);
    for (size_t i = k / 2 + 1; i-- > 0;) {
        sift_down(items, k, i, size, compare, context, held);
    }
    for (size_t i = k; i < count; i++) {
        if (compare(context, items + i * size, items) < 0) {
            memcpy(held, items, size);
            memcpy(items, items + i * size, size);
            memcpy(items + i * size, held, size);
            sift_down(items, k, 0, size, compare, context, held);
        }
    }
    arena_release(mark);
    sort_stable(scratch, items, k, size, compare, context);
    return k;
}
