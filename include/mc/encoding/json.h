#pragma once

#include "mc/core/error.h"
#include "mc/encoding/node.h"

// json_parse reads one JSON value (RFC 8259) into a Node tree in the arena.
// A number without a fraction or exponent that fits int64_t is NODE_INT,
// any other number NODE_FLOAT. A key repeated in an object keeps its first
// place and its last value. Malformed input is ERR_PARSE with the line and
// column in err.
[[nodiscard]] Error json_parse(Arena *arena, String text, Node **root, Err *err);

// json_append_quoted writes s as a JSON string literal. Bytes that are not
// valid UTF-8 are written as U+FFFD, so the output is always valid JSON.
void json_append_quoted(StringBuilder *builder, String s);
String json_quote(Arena *arena, String s);

// json_encode writes node as compact JSON. A float is written in the
// shortest form that reads back as the same double, or as null when it is
// infinite or not a number.
String json_encode(Arena *arena, const Node *node);
