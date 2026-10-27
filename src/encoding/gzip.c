#include "mc/encoding/gzip.h"

#include <limits.h>
#include <string.h>
#include <zlib.h>

enum { GZIP_WINDOW_BITS = 15 + 16, CHUNK_BYTES = 16384, HEADER_SLACK = 64 };

Error gzip_compress(Arena *arena, String data, const GzipOptions *options, String *compressed, Err *err)
{
    GzipOptions chosen = options != nullptr ? *options : (GzipOptions){ 0 };
    bool comment_has_nul = chosen.comment.len > 0 && memchr(chosen.comment.data, '\0', chosen.comment.len) != nullptr;
    if (chosen.level < 0 || chosen.level > 9 || comment_has_nul) {
        return err_set(err, ERR_INVALID_ARGUMENT, "gzip: a level outside 0 to 9 or a comment holding a NUL");
    }
    if (data.len > UINT_MAX) {
        return err_set(err, ERR_UNSUPPORTED, "gzip: %zu bytes is more than zlib takes at once", data.len);
    }
    z_stream stream = { 0 };
    if (deflateInit2(&stream, chosen.level == 0 ? Z_DEFAULT_COMPRESSION : chosen.level, Z_DEFLATED, GZIP_WINDOW_BITS, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return err_set(err, ERR_OUT_OF_MEMORY, "gzip: cannot start zlib");
    }
    gz_header header = { .comment = (Bytef *)str_copy(arena, chosen.comment).data };
    int status = chosen.comment.len > 0 ? deflateSetHeader(&stream, &header) : Z_OK;
    size_t capacity = deflateBound(&stream, (uLong)data.len) + chosen.comment.len + HEADER_SLACK;
    char *out = arena_push(arena, capacity);
    stream.next_in = (Bytef *)data.data;
    stream.avail_in = (uInt)data.len;
    stream.next_out = (Bytef *)out;
    stream.avail_out = (uInt)capacity;
    if (status == Z_OK) {
        status = deflate(&stream, Z_FINISH);
    }
    size_t written = stream.total_out;
    deflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return err_set(err, ERR_PLATFORM, "gzip: zlib could not compress (%d)", status);
    }
    *compressed = (String){ out, written };
    return ERR_OK;
}

Error gzip_decompress(Arena *arena, String data, size_t max_bytes, String *decompressed, Err *err)
{
    if (data.len > UINT_MAX) {
        return err_set(err, ERR_UNSUPPORTED, "gzip: %zu bytes is more than zlib takes at once", data.len);
    }
    z_stream stream = { 0 };
    if (inflateInit2(&stream, GZIP_WINDOW_BITS) != Z_OK) {
        return err_set(err, ERR_OUT_OF_MEMORY, "gzip: cannot start zlib");
    }
    stream.next_in = (Bytef *)data.data;
    stream.avail_in = (uInt)data.len;
    StringBuilder out = str_builder_create(arena, CHUNK_BYTES);
    unsigned char chunk[CHUNK_BYTES];
    int status = Z_OK;
    Error e = ERR_OK;
    while (e == ERR_OK && status != Z_STREAM_END) {
        stream.next_out = chunk;
        stream.avail_out = sizeof chunk;
        status = inflate(&stream, Z_NO_FLUSH);
        size_t produced = sizeof chunk - stream.avail_out;
        if (status != Z_OK && status != Z_STREAM_END) {
            e = err_set(err, ERR_PARSE, "gzip: not a whole gzip stream (%d)", status);
        } else if (out.len + produced > max_bytes) {
            e = err_set(err, ERR_UNSUPPORTED, "gzip: more than %zu bytes once decompressed", max_bytes);
        } else {
            str_builder_append(&out, (String){ (const char *)chunk, produced });
        }
    }
    if (e == ERR_OK && stream.avail_in != 0) {
        e = err_set(err, ERR_PARSE, "gzip: %u bytes after the gzip stream", stream.avail_in);
    }
    inflateEnd(&stream);
    if (e == ERR_OK) {
        *decompressed = str_builder_finish(&out);
    }
    return e;
}
