#include "mc/container/strmap.h"

#include "mc/container/hash.h"

typedef enum { SLOT_EMPTY, SLOT_USED, SLOT_REMOVED } SlotState;

typedef struct {
    String key;
    void *value;
    uint64_t hash;
    SlotState state;
} Slot;

struct StrMap {
    Arena *arena;
    Slot *slots;
    size_t capacity; // a power of two
    size_t live;
    size_t removed;
};

static size_t round_up_power_of_two(size_t value)
{
    size_t result = 16;
    while (result < value) {
        result = arena_size_mul(result, 2);
    }
    return result;
}

StrMap *strmap_create(Arena *arena, size_t expected_entries)
{
    StrMap *map = arena_push(arena, sizeof(StrMap));
    map->arena = arena;
    map->capacity = round_up_power_of_two(arena_size_mul(expected_entries, 2));
    map->slots = arena_push(arena, arena_size_mul(map->capacity, sizeof(Slot)));
    return map;
}

size_t strmap_count(const StrMap *map)
{
    return map->live;
}

static Slot *find_used(const StrMap *map, String key, uint64_t hash)
{
    size_t mask = map->capacity - 1;
    for (size_t i = (size_t)hash & mask;; i = (i + 1) & mask) {
        Slot *slot = &map->slots[i];
        if (slot->state == SLOT_EMPTY) {
            return nullptr;
        }
        if (slot->state == SLOT_USED && slot->hash == hash && str_equal(slot->key, key)) {
            return slot;
        }
    }
}

// find_free returns the first slot a new key may take: a removed one on the way, else the empty one ending the probe.
static Slot *find_free(Slot *slots, size_t capacity, uint64_t hash)
{
    size_t mask = capacity - 1;
    for (size_t i = (size_t)hash & mask;; i = (i + 1) & mask) {
        if (slots[i].state != SLOT_USED) {
            return &slots[i];
        }
    }
}

bool strmap_has(const StrMap *map, String key)
{
    return find_used(map, key, hash_string(key)) != nullptr;
}

void *strmap_get(const StrMap *map, String key)
{
    Slot *slot = find_used(map, key, hash_string(key));
    return slot != nullptr ? slot->value : nullptr;
}

// rehash also clears removed slots, so a map with many removals keeps its capacity.
static void rehash(StrMap *map)
{
    size_t capacity = (map->live + 1) * 4 > map->capacity ? arena_size_mul(map->capacity, 2) : map->capacity;
    Slot *slots = arena_push(map->arena, arena_size_mul(capacity, sizeof(Slot)));
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->slots[i].state == SLOT_USED) {
            *find_free(slots, capacity, map->slots[i].hash) = map->slots[i];
        }
    }
    map->slots = slots;
    map->capacity = capacity;
    map->removed = 0;
}

void *strmap_put(StrMap *map, String key, void *value)
{
    uint64_t hash = hash_string(key);
    Slot *existing = find_used(map, key, hash);
    if (existing != nullptr) {
        void *previous = existing->value;
        existing->value = value;
        return previous;
    }
    if ((map->live + map->removed + 1) * 2 > map->capacity) {
        rehash(map);
    }
    Slot *slot = find_free(map->slots, map->capacity, hash);
    if (slot->state == SLOT_REMOVED) {
        map->removed--;
    }
    *slot = (Slot){ .key = str_copy(map->arena, key), .value = value, .hash = hash, .state = SLOT_USED };
    map->live++;
    return nullptr;
}

bool strmap_remove(StrMap *map, String key, void **value)
{
    Slot *slot = find_used(map, key, hash_string(key));
    if (slot == nullptr) {
        return false;
    }
    if (value != nullptr) {
        *value = slot->value;
    }
    *slot = (Slot){ .state = SLOT_REMOVED };
    map->live--;
    map->removed++;
    return true;
}

StrMapIterator strmap_iterate(const StrMap *map)
{
    return (StrMapIterator){ .map = map, .index = 0 };
}

bool strmap_next(StrMapIterator *iterator, String *key, void **value)
{
    while (iterator->index < iterator->map->capacity) {
        const Slot *slot = &iterator->map->slots[iterator->index++];
        if (slot->state == SLOT_USED) {
            *key = slot->key;
            *value = slot->value;
            return true;
        }
    }
    return false;
}
