#include "mc/container/hash.h"

uint64_t hash_bytes(const void *data, size_t len, uint64_t seed)
{
    const unsigned char *bytes = data;
    uint64_t hash = 0xcbf29ce484222325ULL ^ seed;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t hash_string(String s)
{
    return hash_bytes(s.data, s.len, 0);
}
