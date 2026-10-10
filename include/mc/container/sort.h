#pragma once

#include "mc/core/arena.h"

// Comparators receive a context, so sorting stays reentrant across threads.
typedef int (*CompareFn)(const void *context, const void *a, const void *b);

// sort_stable is a merge sort; its buffer lives in scratch and is released before it returns.
void sort_stable(Arena *scratch, void *base, size_t count, size_t size, CompareFn compare, const void *context);

// sort_top moves the k smallest elements, in order, to the front of base and
// returns how many that is. The other elements stay in base, in no particular
// order: base remains a permutation of its input. Selecting a few hundred out of a million is far
// cheaper than sorting the million.
size_t sort_top(Arena *scratch, void *base, size_t count, size_t size, size_t k, CompareFn compare,
                const void *context);
