#pragma once

#include "mc/text/str.h"

// MD5 is broken as a cryptographic hash. It is here for the protocols that
// still name it as a checksum, such as S3's ETag and Content-MD5.

enum { MD5_LEN = 16 };

typedef struct Md5 {
    uint32_t state[4];
    uint64_t length;
    uint8_t buffer[64];
    size_t buffered;
} Md5;

void md5_init(Md5 *hash);
void md5_update(Md5 *hash, const void *data, size_t len);
void md5_final(Md5 *hash, uint8_t digest[MD5_LEN]);
void md5(String data, uint8_t digest[MD5_LEN]);
