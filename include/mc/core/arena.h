#pragma once

#include "mc/core/base.h"

// Arena hands out memory for objects that die together. Allocation never
// fails: running out of system memory aborts. Memory comes back zeroed.
typedef struct Arena Arena;

// ArenaMark is a position to return to: mark, allocate temporaries, release.
typedef struct ArenaMark {
    Arena *arena;
    void *block;
    size_t used;
} ArenaMark;

// arena_create grows in blocks of block_size bytes; 0 picks a default.
Arena *arena_create(size_t block_size);
void arena_destroy(Arena *arena);

// arena_push returns size bytes aligned for any type.
void *arena_push(Arena *arena, size_t size);
// arena_push_aligned takes a power-of-two alignment.
void *arena_push_aligned(Arena *arena, size_t size, size_t alignment);

// arena_size_add and arena_size_mul compute allocation sizes. A size that
// does not fit in size_t aborts, as running out of memory does.
size_t arena_size_add(size_t a, size_t b);
size_t arena_size_mul(size_t a, size_t b);

// arena_grow makes room for one more item in a dynamic array of count items:
// it returns items unchanged while count < *capacity, else a copy with twice
// the capacity, and updates *capacity.
void *arena_grow(Arena *arena, void *items, size_t *capacity, size_t count, size_t item_size);

// arena_reset discards every allocation and keeps the blocks for reuse.
void arena_reset(Arena *arena);
ArenaMark arena_mark(Arena *arena);
void arena_release(ArenaMark mark);

size_t arena_bytes_used(const Arena *arena);
