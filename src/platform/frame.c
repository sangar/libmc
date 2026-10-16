#include "mc/platform/frame.h"

#include <string.h>

#include "mc/core/arena.h"
#include "mc/platform/platform.h"

static size_t get_u32(const char *at)
{
    size_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= (size_t)(unsigned char)at[i] << (8 * i);
    }
    return value;
}

static void put_u32(char *at, size_t value)
{
    for (int i = 0; i < 4; i++) {
        at[i] = (char)((value >> (8 * i)) & 0xff);
    }
}

void frame_prefix(char prefix[FRAME_PREFIX_LEN], size_t header_len, size_t body_len)
{
    put_u32(prefix, header_len);
    put_u32(prefix + 4, body_len);
}

String frame_encode(Arena *arena, String header, String body)
{
    size_t len = arena_size_add(arena_size_add(FRAME_PREFIX_LEN, header.len), body.len);
    char *data = arena_push_aligned(arena, len, 1);
    frame_prefix(data, header.len, body.len);
    if (header.len > 0) {
        memcpy(data + FRAME_PREFIX_LEN, header.data, header.len);
    }
    if (body.len > 0) {
        memcpy(data + FRAME_PREFIX_LEN + header.len, body.data, body.len);
    }
    return (String){ data, len };
}

// receive_exactly fills buffer; *closed says the peer closed before the first byte.
[[nodiscard]] static Error receive_exactly(int fd, char *buffer, size_t len, bool *closed, Err *err)
{
    *closed = false;
    size_t have = 0;
    while (have < len) {
        size_t got;
        Error e = net_receive(fd, buffer + have, len - have, &got, err);
        if (e != ERR_OK) {
            return e;
        }
        if (got == 0) {
            *closed = have == 0;
            return err_set(err, ERR_NETWORK, "the connection closed inside a frame");
        }
        have += got;
    }
    return ERR_OK;
}

Error frame_receive(int fd, Arena *arena, size_t limit, Frame *frame, Err *err)
{
    char prefix[FRAME_PREFIX_LEN];
    bool closed;
    Error e = receive_exactly(fd, prefix, FRAME_PREFIX_LEN, &closed, err);
    if (closed) {
        return err_set(err, ERR_CANCELLED, "the connection closed");
    }
    if (e != ERR_OK) {
        return e;
    }
    size_t header_len = get_u32(prefix);
    size_t body_len = get_u32(prefix + 4);
    if (header_len > limit || body_len > limit - header_len) {
        return err_set(err, ERR_PARSE, "a frame of %zu and %zu bytes is over the limit of %zu", header_len, body_len,
                       limit);
    }
    char *data = arena_push_aligned(arena, header_len + body_len + 1, 1);
    e = receive_exactly(fd, data, header_len + body_len, &closed, err);
    if (e != ERR_OK) {
        return e;
    }
    *frame = (Frame){ { data, header_len }, { data + header_len, body_len } };
    return ERR_OK;
}

Error frame_send(int fd, Arena *scratch, String header, String body, Err *err)
{
    ArenaMark mark = arena_mark(scratch);
    Error e = net_send(fd, frame_encode(scratch, header, body), err);
    arena_release(mark);
    return e;
}
