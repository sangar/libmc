#include "mc/container/idtable.h"

#include <assert.h>
#include <string.h>

static const uint32_t SLOT_EMPTY = UINT32_MAX;
static const uint32_t SLOT_REMOVED = UINT32_MAX - 1;

struct IdTable {
    Arena *arena;
    uint32_t *slots;
    size_t capacity; // a power of two
    size_t used;     // live ids plus removed markers
    size_t live;
};

// spread mixes every bit of a hash into the low bits that pick a slot, so a
// weak hash such as FNV-1a over similar keys still spreads out.
static uint64_t spread(uint64_t hash)
{
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    return hash;
}

static void allocate_slots(IdTable *table, size_t expected)
{
    size_t capacity = 16;
    while (capacity < arena_size_mul(expected, 2)) {
        capacity *= 2;
    }
    table->slots = arena_push_aligned(table->arena, arena_size_mul(capacity, sizeof *table->slots),
                                      alignof(uint32_t));
    memset(table->slots, 0xff, capacity * sizeof *table->slots);
    table->capacity = capacity;
    table->used = 0;
    table->live = 0;
}

IdTable *idtable_create(Arena *arena, size_t expected)
{
    IdTable *table = arena_push(arena, sizeof *table);
    table->arena = arena;
    allocate_slots(table, expected);
    return table;
}

size_t idtable_count(const IdTable *table)
{
    return table->live;
}

uint32_t idtable_find(const IdTable *table, uint64_t hash, IdEqualFn equal, void *context)
{
    size_t mask = table->capacity - 1;
    for (size_t i = spread(hash) & mask;; i = (i + 1) & mask) {
        uint32_t id = table->slots[i];
        if (id == SLOT_EMPTY) {
            return IDTABLE_NONE;
        }
        if (id != SLOT_REMOVED && equal(context, id)) {
            return id;
        }
    }
}

static void place(IdTable *table, uint64_t hash, uint32_t id)
{
    size_t mask = table->capacity - 1;
    size_t i = spread(hash) & mask;
    while (table->slots[i] != SLOT_EMPTY && table->slots[i] != SLOT_REMOVED) {
        i = (i + 1) & mask;
    }
    if (table->slots[i] == SLOT_EMPTY) {
        table->used++;
    }
    table->slots[i] = id;
    table->live++;
}

static void grow(IdTable *table, IdHashFn rehash, void *context)
{
    uint32_t *old = table->slots;
    size_t old_capacity = table->capacity;
    allocate_slots(table, table->live + 1);
    for (size_t i = 0; i < old_capacity; i++) {
        if (old[i] != SLOT_EMPTY && old[i] != SLOT_REMOVED) {
            place(table, rehash(context, old[i]), old[i]);
        }
    }
}

void idtable_insert(IdTable *table, uint64_t hash, uint32_t id, IdHashFn rehash, void *context)
{
    assert(id < SLOT_REMOVED);
    if ((table->used + 1) * 4 > table->capacity * 3) {
        grow(table, rehash, context);
    }
    place(table, hash, id);
}

bool idtable_remove(IdTable *table, uint64_t hash, uint32_t id)
{
    size_t mask = table->capacity - 1;
    for (size_t i = spread(hash) & mask;; i = (i + 1) & mask) {
        if (table->slots[i] == SLOT_EMPTY) {
            return false;
        }
        if (table->slots[i] == id) {
            table->slots[i] = SLOT_REMOVED;
            table->live--;
            return true;
        }
    }
}
