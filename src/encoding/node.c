#include "mc/encoding/node.h"

Node *node_create(Arena *arena, NodeKind kind)
{
    Node *node = arena_push(arena, sizeof *node);
    node->kind = kind;
    node->text = S("");
    return node;
}

Node *node_scalar(Arena *arena, NodeKind kind, String text)
{
    Node *node = node_create(arena, kind);
    node->text = str_copy(arena, text);
    return node;
}

void node_append(Arena *arena, Node *sequence, Node *item)
{
    sequence->items = arena_grow(arena, sequence->items, &sequence->capacity, sequence->count, sizeof *sequence->items);
    sequence->items[sequence->count++] = item;
}

void node_put(Arena *arena, Node *mapping, String key, Node *value)
{
    for (size_t i = 0; i < mapping->entry_count; i++) {
        if (str_equal(mapping->entries[i].key, key)) {
            mapping->entries[i].value = value;
            return;
        }
    }
    mapping->entries = arena_grow(arena, mapping->entries, &mapping->entry_capacity, mapping->entry_count,
                                  sizeof *mapping->entries);
    mapping->entries[mapping->entry_count++] = (NodeEntry){ str_copy(arena, key), value };
}

const Node *node_get(const Node *mapping, String key)
{
    if (mapping == nullptr || mapping->kind != NODE_MAPPING) {
        return nullptr;
    }
    for (size_t i = 0; i < mapping->entry_count; i++) {
        if (str_equal(mapping->entries[i].key, key)) {
            return mapping->entries[i].value;
        }
    }
    return nullptr;
}

const char *node_kind_name(NodeKind kind)
{
    switch (kind) {
    case NODE_NULL: return "null";
    case NODE_BOOL: return "boolean";
    case NODE_INT: return "integer";
    case NODE_FLOAT: return "number";
    case NODE_STRING: return "string";
    case NODE_SEQUENCE: return "sequence";
    case NODE_MAPPING: return "mapping";
    }
    return "unknown";
}

static const Node *get_kind(const Node *mapping, String key, NodeKind kind)
{
    const Node *value = node_get(mapping, key);
    return value != nullptr && value->kind == kind ? value : nullptr;
}

String node_get_string(const Node *mapping, String key, String fallback)
{
    const Node *value = get_kind(mapping, key, NODE_STRING);
    return value != nullptr ? value->text : fallback;
}

int64_t node_get_int(const Node *mapping, String key, int64_t fallback)
{
    const Node *value = get_kind(mapping, key, NODE_INT);
    return value != nullptr ? value->integer : fallback;
}

bool node_get_bool(const Node *mapping, String key, bool fallback)
{
    const Node *value = get_kind(mapping, key, NODE_BOOL);
    return value != nullptr ? value->boolean : fallback;
}
