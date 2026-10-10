#pragma once

#include "mc/core/arena.h"

// IdTable is an open-addressing hash set of uint32_t ids whose keys live
// elsewhere, such as in the records the ids number. The caller hashes keys
// and decides equality, so a slot holds nothing but an id: four bytes, where
// a map from keys would copy every key. Its slots live in an arena; growing
// abandons the old slots there.
typedef struct IdTable IdTable;

// IDTABLE_NONE is what idtable_find returns for a missing key. It and the id
// below it are reserved and cannot be inserted.
static const uint32_t IDTABLE_NONE = UINT32_MAX;

typedef bool (*IdEqualFn)(void *context, uint32_t id);
typedef uint64_t (*IdHashFn)(void *context, uint32_t id);

// idtable_create sizes the table for expected ids without growing.
IdTable *idtable_create(Arena *arena, size_t expected);
size_t idtable_count(const IdTable *table);
// idtable_find returns the id whose key has hash and satisfies equal, or IDTABLE_NONE.
uint32_t idtable_find(const IdTable *table, uint64_t hash, IdEqualFn equal, void *context);
// idtable_insert adds id under hash. When the table grows, rehash recomputes
// the hash of every id already in it.
void idtable_insert(IdTable *table, uint64_t hash, uint32_t id, IdHashFn rehash, void *context);
// idtable_remove drops id, inserted under hash, and reports whether it was there.
bool idtable_remove(IdTable *table, uint64_t hash, uint32_t id);
