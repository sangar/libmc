#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// gzip (RFC 1952) on zlib, as HTTP's Content-Encoding: gzip carries it.

typedef struct GzipOptions {
    int level; // 1, fastest, to 9, smallest; 0 is zlib's default, 6
    // comment goes in the header's comment field, which decoders skip; a
    // server can pad responses with it against compression side channels
    // such as BREACH. It may not hold a NUL.
    String comment;
} GzipOptions;

// gzip_compress writes data as one gzip member in arena; options may be
// nullptr. A comment holding a NUL, or a level outside 0 to 9, is
// ERR_INVALID_ARGUMENT.
[[nodiscard]] Error gzip_compress(Arena *arena, String data, const GzipOptions *options, String *compressed, Err *err);

// gzip_decompress reads one gzip member, with nothing after it, into arena.
// Data that is not gzip, is truncated or fails its check is ERR_PARSE; more
// than max_bytes once decompressed is ERR_UNSUPPORTED, which bounds what a
// small malicious input can make it hold.
[[nodiscard]] Error gzip_decompress(Arena *arena, String data, size_t max_bytes, String *decompressed, Err *err);
