#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// A frame is one message on a stream socket: [u32 header length][u32 body
// length][header][body], the lengths little-endian. Both parts are bytes; a
// header is usually a small JSON object that says what the body is.

enum { FRAME_PREFIX_LEN = 8 };

typedef struct Frame {
    String header;
    String body;
} Frame;

// frame_prefix writes the lengths that start a frame of a header and a body.
void frame_prefix(char prefix[FRAME_PREFIX_LEN], size_t header_len, size_t body_len);
String frame_encode(Arena *arena, String header, String body);
// frame_receive reads one whole frame from fd into the arena. A peer that
// closed between frames is ERR_CANCELLED; one that closed inside a frame is
// ERR_NETWORK, and a frame of more than limit bytes is ERR_PARSE.
[[nodiscard]] Error frame_receive(int fd, Arena *arena, size_t limit, Frame *frame, Err *err);
// frame_send writes one frame to fd, building it in scratch.
[[nodiscard]] Error frame_send(int fd, Arena *scratch, String header, String body, Err *err);
