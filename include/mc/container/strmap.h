#pragma once

#include "mc/text/str.h"

// StrMap maps strings to pointers with open addressing, in arena memory.
// Keys are copied into the arena; values are the caller's. Growing rehashes
// into the same arena and abandons the old table, so a map that churns
// belongs in an arena that is reset or destroyed with it.
typedef struct StrMap StrMap;

StrMap *strmap_create(Arena *arena, size_t expected_entries);
size_t strmap_count(const StrMap *map);
bool strmap_has(const StrMap *map, String key);
// strmap_get returns nullptr when key is absent.
void *strmap_get(const StrMap *map, String key);
// strmap_put inserts or replaces, and returns the previous value or nullptr.
void *strmap_put(StrMap *map, String key, void *value);
// strmap_remove drops key and reports whether it was present; *value, when given, receives its value.
bool strmap_remove(StrMap *map, String key, void **value);

typedef struct StrMapIterator {
    const StrMap *map;
    size_t index;
} StrMapIterator;

// strmap_next walks the entries in no particular order. While iterating, a
// caller may replace the value of an existing key or remove entries; putting
// a new key can rehash the table and invalidates the iterator.
StrMapIterator strmap_iterate(const StrMap *map);
bool strmap_next(StrMapIterator *iterator, String *key, void **value);
