#pragma once

#include "mc/text/str.h"

enum { SHA256_LEN = 32 };

typedef struct Sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t buffer[64];
    size_t buffered;
} Sha256;

void sha256_init(Sha256 *hash);
void sha256_update(Sha256 *hash, const void *data, size_t len);
void sha256_final(Sha256 *hash, uint8_t digest[SHA256_LEN]);
void sha256(String data, uint8_t digest[SHA256_LEN]);
void hmac_sha256(String key, String data, uint8_t digest[SHA256_LEN]);

// hex_encode writes bytes as lowercase hex digits.
String hex_encode(Arena *arena, const uint8_t *bytes, size_t len);
