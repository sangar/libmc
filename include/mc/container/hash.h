#pragma once

#include "mc/text/str.h"

// FNV-1a: fast and well spread for hash table buckets and fingerprints, not
// for anything an attacker chooses the input of.
uint64_t hash_bytes(const void *data, size_t len, uint64_t seed);
uint64_t hash_string(String s);
