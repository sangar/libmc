#pragma once

#include "mc/core/error.h"
#include "mc/encoding/node.h"

// yaml_parse reads one YAML 1.2 document into a Node tree in the arena.
// Plain scalars are typed by the core schema: null, ~ and the empty value
// are NODE_NULL, true and false NODE_BOOL, decimal, 0o octal and 0x hex
// integers that fit int64_t NODE_INT, other numbers, .inf and .nan
// NODE_FLOAT. Quoted, block and !!str scalars are strings. An alias shares
// the node of its anchor. Keys must be scalars; a repeated key keeps its
// first place and its last value. An empty document is NODE_NULL.
// Malformed input, an alias inside the node it names and a stream of more
// than one document are ERR_PARSE with the line and column in err.
[[nodiscard]] Error yaml_parse(Arena *arena, String text, Node **root, Err *err);
