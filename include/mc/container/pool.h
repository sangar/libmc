#pragma once

#include "mc/core/arena.h"

// Pool holds items of one size that are freed one at a time, such as nodes
// or connections, and reuses the slots of freed items. Keep a PoolHandle,
// not a pointer: freeing an item changes its slot's generation, so a stale
// handle finds nothing instead of the item that reused the slot. Items live
// in an arena; growing abandons the old items there.
typedef struct Pool Pool;

// PoolHandle names one item. The zero handle names none. Wrap it in a struct
// per item type, so the compiler rejects a handle into the wrong pool.
typedef struct PoolHandle {
    uint32_t index;
    uint32_t generation;
} PoolHandle;

Pool *pool_create(Arena *arena, size_t item_size);
// pool_alloc returns the handle of a new, zeroed item.
PoolHandle pool_alloc(Pool *pool);
// pool_get returns the item, or nullptr once it is freed. The pointer is
// valid until the next pool_alloc, which can move the items.
void *pool_get(const Pool *pool, PoolHandle handle);
// pool_free frees the item and reports whether it was live, so freeing it
// twice does nothing.
bool pool_free(Pool *pool, PoolHandle handle);
