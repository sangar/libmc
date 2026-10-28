#include "mc/core/arena.h"

#include <assert.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DEFAULT_BLOCK_SIZE = 256 * 1024 };

typedef struct Block Block;

// A block comes from calloc, so its data starts out zero. dirty is how far
// it has ever been handed out: only memory below it can hold old values and
// needs clearing when it is handed out again. Memory above it is still zero
// and, in a large block the system mapped fresh, not yet touched at all.
struct Block {
    Block *next;
    size_t used;
    size_t dirty;
    size_t capacity;
    alignas(max_align_t) unsigned char data[];
};

struct Arena {
    Block *first;
    Block *current;
    size_t block_size;
};

static void *allocate_or_abort(size_t size)
{
    void *memory = calloc(1, size);
    if (memory == nullptr) {
        fprintf(stderr, "arena: out of memory allocating %zu bytes\n", size);
        abort();
    }
    return memory;
}

size_t arena_size_add(size_t a, size_t b)
{
    size_t sum;
    if (ckd_add(&sum, a, b)) {
        fprintf(stderr, "arena: allocation size overflows (%zu + %zu)\n", a, b);
        abort();
    }
    return sum;
}

size_t arena_size_mul(size_t a, size_t b)
{
    size_t product;
    if (ckd_mul(&product, a, b)) {
        fprintf(stderr, "arena: allocation size overflows (%zu * %zu)\n", a, b);
        abort();
    }
    return product;
}

static Block *block_create(size_t capacity)
{
    Block *block = allocate_or_abort(arena_size_add(sizeof(Block), capacity));
    block->next = nullptr;
    block->used = 0;
    block->dirty = 0;
    block->capacity = capacity;
    return block;
}

Arena *arena_create(size_t block_size)
{
    Arena *arena = allocate_or_abort(sizeof(Arena));
    arena->block_size = block_size > 0 ? block_size : DEFAULT_BLOCK_SIZE;
    arena->first = block_create(arena->block_size);
    arena->current = arena->first;
    return arena;
}

static void free_from(Block *block)
{
    while (block != nullptr) {
        Block *next = block->next;
        free(block);
        block = next;
    }
}

void arena_destroy(Arena *arena)
{
    if (arena == nullptr) {
        return;
    }
    free_from(arena->first);
    free(arena);
}

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// hand_out gives out the block's bytes from start to end, clearing those
// that were handed out before.
static void *hand_out(Block *block, size_t start, size_t end)
{
    block->used = end;
    if (start < block->dirty) {
        memset(block->data + start, 0, min_size(end, block->dirty) - start);
    }
    block->dirty = max_size(block->dirty, end);
    return block->data + start;
}

// Blocks after the current one are empty: a later allocation moved past them
// only after a mark was released or the arena was reset.
void *arena_push_aligned(Arena *arena, size_t size, size_t alignment)
{
    assert(alignment > 0 && (alignment & (alignment - 1)) == 0);
    Block *block = arena->current;
    for (;;) {
        uintptr_t base = (uintptr_t)block->data;
        size_t start = align_up(base + block->used, alignment) - base;
        if (start <= block->capacity && size <= block->capacity - start) {
            arena->current = block;
            return hand_out(block, start, start + size);
        }
        if (block->next == nullptr) {
            block->next = block_create(max_size(arena_size_add(size, alignment), arena->block_size));
        }
        block = block->next;
    }
}

void *arena_push(Arena *arena, size_t size)
{
    return arena_push_aligned(arena, size, alignof(max_align_t));
}

// extend_in_place grows the arena's newest allocation, which ends at end, by
// extra bytes when its block has room.
static bool extend_in_place(Arena *arena, const unsigned char *end, size_t extra)
{
    Block *block = arena->current;
    if (end != block->data + block->used || extra > block->capacity - block->used) {
        return false;
    }
    hand_out(block, block->used, block->used + extra);
    return true;
}

void *arena_grow(Arena *arena, void *items, size_t *capacity, size_t count, size_t item_size)
{
    if (count < *capacity) {
        return items;
    }
    size_t grown = *capacity > 0 ? arena_size_mul(*capacity, 2) : 8;
    size_t size = *capacity * item_size;
    size_t grown_size = arena_size_mul(grown, item_size);
    if (items != nullptr && extend_in_place(arena, (unsigned char *)items + size, grown_size - size)) {
        *capacity = grown;
        return items;
    }
    void *fresh = arena_push(arena, grown_size);
    if (count > 0) {
        memcpy(fresh, items, count * item_size);
    }
    *capacity = grown;
    return fresh;
}

static void clear_from(Block *block)
{
    for (; block != nullptr; block = block->next) {
        block->used = 0;
    }
}

// A burst would otherwise stay allocated for as long as the arena lives.
void arena_reset(Arena *arena)
{
    free_from(arena->first->next);
    arena->first->next = nullptr;
    arena->first->used = 0;
    arena->current = arena->first;
}

ArenaMark arena_mark(Arena *arena)
{
    return (ArenaMark){ .arena = arena, .block = arena->current, .used = arena->current->used };
}

void arena_release(ArenaMark mark)
{
    Block *block = mark.block;
    block->used = mark.used;
    clear_from(block->next);
    mark.arena->current = block;
}

size_t arena_bytes_used(const Arena *arena)
{
    size_t total = 0;
    for (const Block *block = arena->first; block != nullptr; block = block->next) {
        total += block->used;
    }
    return total;
}
