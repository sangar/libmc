#include "mc/crypto/sha256.h"

#include <string.h>

enum { BLOCK_LEN = 64 };

static const uint32_t ROUND_CONSTANTS[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void transform(Sha256 *hash, const uint8_t block[BLOCK_LEN])
{
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 |
               block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = hash->state[0];
    uint32_t b = hash->state[1];
    uint32_t c = hash->state[2];
    uint32_t d = hash->state[3];
    uint32_t e = hash->state[4];
    uint32_t f = hash->state[5];
    uint32_t g = hash->state[6];
    uint32_t h = hash->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choice + ROUND_CONSTANTS[i] + w[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
    hash->state[4] += e;
    hash->state[5] += f;
    hash->state[6] += g;
    hash->state[7] += h;
}

void sha256_init(Sha256 *hash)
{
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(hash->state, initial, sizeof initial);
    hash->length = 0;
    hash->buffered = 0;
}

void sha256_update(Sha256 *hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    hash->length += len;
    if (hash->buffered > 0) {
        size_t take = min_size(BLOCK_LEN - hash->buffered, len);
        memcpy(hash->buffer + hash->buffered, bytes, take);
        hash->buffered += take;
        bytes += take;
        len -= take;
        if (hash->buffered < BLOCK_LEN) {
            return;
        }
        transform(hash, hash->buffer);
        hash->buffered = 0;
    }
    for (; len >= BLOCK_LEN; bytes += BLOCK_LEN, len -= BLOCK_LEN) {
        transform(hash, bytes);
    }
    if (len > 0) {
        memcpy(hash->buffer, bytes, len);
    }
    hash->buffered = len;
}

void sha256_final(Sha256 *hash, uint8_t digest[SHA256_LEN])
{
    uint64_t bits = hash->length * 8;
    uint8_t padding[BLOCK_LEN + 8] = { 0x80 };
    size_t pad_len = hash->buffered < 56 ? 56 - hash->buffered : BLOCK_LEN + 56 - hash->buffered;
    for (size_t i = 0; i < 8; i++) {
        padding[pad_len + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_update(hash, padding, pad_len + 8);
    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(hash->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(hash->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(hash->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)hash->state[i];
    }
}

void sha256(String data, uint8_t digest[SHA256_LEN])
{
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, data.data, data.len);
    sha256_final(&hash, digest);
}

void hmac_sha256(String key, String data, uint8_t digest[SHA256_LEN])
{
    uint8_t block_key[BLOCK_LEN] = { 0 };
    if (key.len > BLOCK_LEN) {
        sha256(key, block_key);
    } else if (key.len > 0) {
        memcpy(block_key, key.data, key.len);
    }
    uint8_t inner_pad[BLOCK_LEN];
    uint8_t outer_pad[BLOCK_LEN];
    for (size_t i = 0; i < BLOCK_LEN; i++) {
        inner_pad[i] = block_key[i] ^ 0x36;
        outer_pad[i] = block_key[i] ^ 0x5c;
    }
    uint8_t inner[SHA256_LEN];
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, inner_pad, BLOCK_LEN);
    sha256_update(&hash, data.data, data.len);
    sha256_final(&hash, inner);
    sha256_init(&hash);
    sha256_update(&hash, outer_pad, BLOCK_LEN);
    sha256_update(&hash, inner, SHA256_LEN);
    sha256_final(&hash, digest);
}

String hex_encode(Arena *arena, const uint8_t *bytes, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    char *text = arena_push_aligned(arena, arena_size_add(arena_size_mul(len, 2), 1), 1);
    for (size_t i = 0; i < len; i++) {
        text[i * 2] = digits[bytes[i] >> 4];
        text[i * 2 + 1] = digits[bytes[i] & 15];
    }
    return (String){ .data = text, .len = len * 2 };
}
