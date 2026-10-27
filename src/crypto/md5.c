#include "mc/crypto/md5.h"

#include <string.h>

enum { BLOCK_LEN = 64 };

static const uint32_t ROUND_CONSTANTS[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t SHIFTS[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t rotate_left(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

static void transform(Md5 *hash, const uint8_t block[BLOCK_LEN])
{
    uint32_t m[16];
    for (size_t i = 0; i < 16; i++) {
        m[i] = (uint32_t)block[i * 4] | (uint32_t)block[i * 4 + 1] << 8 | (uint32_t)block[i * 4 + 2] << 16 |
               (uint32_t)block[i * 4 + 3] << 24;
    }
    uint32_t a = hash->state[0];
    uint32_t b = hash->state[1];
    uint32_t c = hash->state[2];
    uint32_t d = hash->state[3];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t f;
        unsigned g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t next = d;
        d = c;
        c = b;
        b = b + rotate_left(a + f + ROUND_CONSTANTS[i] + m[g], SHIFTS[i]);
        a = next;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
}

void md5_init(Md5 *hash)
{
    *hash = (Md5){ .state = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 } };
}

void md5_update(Md5 *hash, const void *data, size_t len)
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

void md5_final(Md5 *hash, uint8_t digest[MD5_LEN])
{
    uint64_t bits = hash->length * 8;
    uint8_t padding[BLOCK_LEN + 8] = { 0x80 };
    size_t pad_len = hash->buffered < 56 ? 56 - hash->buffered : BLOCK_LEN + 56 - hash->buffered;
    for (size_t i = 0; i < 8; i++) {
        padding[pad_len + i] = (uint8_t)(bits >> (i * 8));
    }
    md5_update(hash, padding, pad_len + 8);
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            digest[i * 4 + j] = (uint8_t)(hash->state[i] >> (8 * j));
        }
    }
}

void md5(String data, uint8_t digest[MD5_LEN])
{
    Md5 hash;
    md5_init(&hash);
    md5_update(&hash, data.data, data.len);
    md5_final(&hash, digest);
}
