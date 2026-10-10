#pragma once

#include "mc/text/str.h"

// Node is a parsed document: scalars, sequences and mappings in document
// order, allocated in an arena. JSON and YAML both parse into it.
typedef enum NodeKind {
    NODE_NULL,
    NODE_BOOL,
    NODE_INT,
    NODE_FLOAT,
    NODE_STRING,
    NODE_SEQUENCE,
    NODE_MAPPING,
} NodeKind;

typedef struct Node Node;

typedef struct NodeEntry {
    String key;
    Node *value;
} NodeEntry;

struct Node {
    NodeKind kind;
    // text is a scalar as written, so a number keeps its digits; for a
    // string it is the decoded value.
    String text;
    bool boolean;
    int64_t integer;
    double number;
    Node **items;
    size_t count;
    size_t capacity;
    NodeEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
};

Node *node_create(Arena *arena, NodeKind kind);
// node_scalar copies text into the arena.
Node *node_scalar(Arena *arena, NodeKind kind, String text);
void node_append(Arena *arena, Node *sequence, Node *item);
// node_put replaces the value of a key already present, keeping its place.
void node_put(Arena *arena, Node *mapping, String key, Node *value);
// node_get returns nullptr when mapping is not a mapping or lacks key.
const Node *node_get(const Node *mapping, String key);
const char *node_kind_name(NodeKind kind);

// The typed getters return fallback when the key is missing or holds another kind.
String node_get_string(const Node *mapping, String key, String fallback);
int64_t node_get_int(const Node *mapping, String key, int64_t fallback);
bool node_get_bool(const Node *mapping, String key, bool fallback);
