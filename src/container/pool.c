#include "mc/container/pool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t NO_SLOT = UINT32_MAX;

// A slot's generation starts at 1, so the zero handle matches no slot, and
// changes each time its item is freed.
typedef struct {
    uint32_t generation;
    uint32_t next_free;
} Slot;

struct Pool {
    Arena *arena;
    size_t item_size;
    unsigned char *items;
    Slot *slots;
    size_t count; // slots handed out at least once
    size_t capacity;
    uint32_t first_free;
};

Pool *pool_create(Arena *arena, size_t item_size)
{
    assert(item_size > 0);
    Pool *pool = arena_push(arena, sizeof *pool);
    pool->arena = arena;
    pool->item_size = item_size;
    pool->first_free = NO_SLOT;
    return pool;
}

static void *item_at(const Pool *pool, uint32_t index)
{
    return pool->items + index * pool->item_size;
}

static uint32_t new_slot(Pool *pool)
{
    if (pool->count == NO_SLOT) {
        fprintf(stderr, "pool: more than %u items\n", NO_SLOT);
        abort();
    }
    size_t capacity = pool->capacity;
    pool->slots = arena_grow(pool->arena, pool->slots, &capacity, pool->count, sizeof *pool->slots);
    pool->items = arena_grow(pool->arena, pool->items, &pool->capacity, pool->count, pool->item_size);
    uint32_t index = (uint32_t)pool->count++;
    pool->slots[index].generation = 1;
    return index;
}

PoolHandle pool_alloc(Pool *pool)
{
    uint32_t index = pool->first_free;
    if (index == NO_SLOT) {
        index = new_slot(pool);
    } else {
        pool->first_free = pool->slots[index].next_free;
        memset(item_at(pool, index), 0, pool->item_size);
    }
    return (PoolHandle){ .index = index, .generation = pool->slots[index].generation };
}

static bool is_live(const Pool *pool, PoolHandle handle)
{
    return handle.index < pool->count && pool->slots[handle.index].generation == handle.generation;
}

void *pool_get(const Pool *pool, PoolHandle handle)
{
    return is_live(pool, handle) ? item_at(pool, handle.index) : nullptr;
}

bool pool_free(Pool *pool, PoolHandle handle)
{
    if (!is_live(pool, handle)) {
        return false;
    }
    Slot *slot = &pool->slots[handle.index];
    slot->generation = slot->generation == UINT32_MAX ? 1 : slot->generation + 1;
    slot->next_free = pool->first_free;
    pool->first_free = handle.index;
    return true;
}
