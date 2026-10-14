#include "mc/encoding/json.h"

#include <stdlib.h>
#include <string.h>

#include "mc/text/utf8.h"
#include "yyjson.h"

// MAX_DEPTH bounds the recursion over the parsed tree, so hostile input
// cannot exhaust the stack here or in the code that walks the Node.
enum { MAX_DEPTH = 512 };

// yyjson allocates from a scratch arena, freed once the Node tree is built.
static void *arena_malloc(void *arena, size_t size)
{
    return arena_push(arena, size);
}

static void *arena_realloc(void *arena, void *old, size_t old_size, size_t size)
{
    void *grown = arena_push(arena, size);
    memcpy(grown, old, min_size(old_size, size));
    return grown;
}

static void arena_free(void *arena, void *ptr)
{
    unused(arena);
    unused(ptr);
}

static yyjson_alc arena_allocator(Arena *arena)
{
    return (yyjson_alc){ arena_malloc, arena_realloc, arena_free, arena };
}

typedef struct {
    Arena *arena;
    String text;
    // buffer is the arena copy of text that yyjson parses in place, so
    // strings and numbers in the tree point into it.
    const char *buffer;
    Err *err;
} Converter;

static void fail(const Converter *converter, size_t at, const char *what)
{
    size_t line = 0;
    size_t column = 0;
    size_t character;
    yyjson_locate_pos(converter->text.data, converter->text.len, at, &line, &column, &character);
    unused(err_set(converter->err, ERR_PARSE, "json: line %zu, column %zu: %s", line, column, what));
}

static Node *literal(Arena *arena, NodeKind kind, String text)
{
    Node *node = node_create(arena, kind);
    node->text = text;
    node->boolean = kind == NODE_BOOL && str_equal(text, S("true"));
    return node;
}

static Node *number(const Converter *converter, yyjson_val *raw)
{
    Node *node = literal(converter->arena, NODE_INT, (String){ yyjson_get_raw(raw), yyjson_get_len(raw) });
    if (!str_contains_any(node->text, S(".eE")) && str_parse_i64(node->text, &node->integer)) {
        node->number = (double)node->integer;
        return node;
    }
    node->kind = NODE_FLOAT;
    yyjson_val value;
    yyjson_read_err err;
    if (yyjson_read_number(node->text.data, &value, 0, nullptr, &err) == nullptr) {
        fail(converter, (size_t)(node->text.data - converter->buffer), err.msg);
        return nullptr;
    }
    node->number = yyjson_get_num(&value);
    return node;
}

static Node *convert(const Converter *converter, yyjson_val *value, int depth);

static Node *convert_array(const Converter *converter, yyjson_val *value, int depth)
{
    Node *array = node_create(converter->arena, NODE_SEQUENCE);
    yyjson_arr_iter iter = yyjson_arr_iter_with(value);
    for (yyjson_val *item; (item = yyjson_arr_iter_next(&iter)) != nullptr;) {
        Node *node = convert(converter, item, depth + 1);
        if (node == nullptr) {
            return nullptr;
        }
        node_append(converter->arena, array, node);
    }
    return array;
}

static Node *convert_object(const Converter *converter, yyjson_val *value, int depth)
{
    Node *object = node_create(converter->arena, NODE_MAPPING);
    yyjson_obj_iter iter = yyjson_obj_iter_with(value);
    for (yyjson_val *key; (key = yyjson_obj_iter_next(&iter)) != nullptr;) {
        Node *node = convert(converter, yyjson_obj_iter_get_val(key), depth + 1);
        if (node == nullptr) {
            return nullptr;
        }
        node_put(converter->arena, object, (String){ yyjson_get_str(key), yyjson_get_len(key) }, node);
    }
    return object;
}

static Node *convert(const Converter *converter, yyjson_val *value, int depth)
{
    if (depth == MAX_DEPTH) {
        unused(err_set(converter->err, ERR_PARSE, "json: nested more than %d levels deep", MAX_DEPTH));
        return nullptr;
    }
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: return literal(converter->arena, NODE_NULL, S("null"));
    case YYJSON_TYPE_BOOL:
        return literal(converter->arena, NODE_BOOL, yyjson_get_bool(value) ? S("true") : S("false"));
    case YYJSON_TYPE_RAW: return number(converter, value);
    case YYJSON_TYPE_STR:
        return literal(converter->arena, NODE_STRING, (String){ yyjson_get_str(value), yyjson_get_len(value) });
    case YYJSON_TYPE_ARR: return convert_array(converter, value, depth);
    case YYJSON_TYPE_OBJ: return convert_object(converter, value, depth);
    default: unreachable();
    }
}

Error json_parse(Arena *arena, String text, Node **root, Err *err)
{
    char *buffer = arena_push(arena, arena_size_add(text.len, YYJSON_PADDING_SIZE));
    memcpy(buffer, text.data, text.len);
    Converter converter = { .arena = arena, .text = text, .buffer = buffer, .err = err };
    Arena *scratch = arena_create(0);
    yyjson_alc alc = arena_allocator(scratch);
    yyjson_read_err read_err;
    yyjson_doc *doc = yyjson_read_opts(buffer, text.len, YYJSON_READ_INSITU | YYJSON_READ_NUMBER_AS_RAW, &alc,
                                       &read_err);
    Node *value = nullptr;
    if (doc == nullptr) {
        fail(&converter, read_err.pos, read_err.msg);
    } else {
        value = convert(&converter, yyjson_doc_get_root(doc), 0);
    }
    arena_destroy(scratch);
    if (value == nullptr) {
        return ERR_PARSE;
    }
    *root = value;
    return ERR_OK;
}

// needs_no_escape is true for the ASCII bytes a JSON string holds as they are.
static bool needs_no_escape(unsigned char c)
{
    return c >= 0x20 && c < 0x80 && c != '"' && c != '\\';
}

void json_append_quoted(StringBuilder *builder, String s)
{
    static const char hex[] = "0123456789abcdef";
    str_builder_append_char(builder, '"');
    size_t at = 0;
    while (at < s.len) {
        size_t plain = at;
        while (plain < s.len && needs_no_escape((unsigned char)s.data[plain])) {
            plain++;
        }
        if (plain > at) {
            str_builder_append(builder, str_slice(s, at, plain));
            at = plain;
            continue;
        }
        unsigned char c = (unsigned char)s.data[at];
        if (c >= 0x80) {
            size_t start = at;
            uint32_t codepoint;
            bool valid = utf8_decode(s, &at, &codepoint);
            str_builder_append(builder, valid ? str_slice(s, start, at) : S("\xEF\xBF\xBD"));
            continue;
        }
        at++;
        switch (c) {
        case '"': str_builder_append(builder, S("\\\"")); break;
        case '\\': str_builder_append(builder, S("\\\\")); break;
        case '\n': str_builder_append(builder, S("\\n")); break;
        case '\r': str_builder_append(builder, S("\\r")); break;
        case '\t': str_builder_append(builder, S("\\t")); break;
        default:
            if (c < 0x20) {
                const char escape[] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
                str_builder_append(builder, (String){ escape, sizeof escape });
            } else {
                str_builder_append_char(builder, (char)c);
            }
        }
    }
    str_builder_append_char(builder, '"');
}

String json_quote(Arena *arena, String s)
{
    StringBuilder builder = str_builder_create(arena, s.len + 2);
    json_append_quoted(&builder, s);
    return str_builder_finish(&builder);
}

static String text_for_writer(Arena *arena, String text, bool replace_invalid_utf8)
{
    if (!replace_invalid_utf8 || utf8_is_valid(text)) {
        return text;
    }
    StringBuilder builder = str_builder_create(arena, text.len);
    size_t at = 0;
    while (at < text.len) {
        uint32_t codepoint;
        utf8_decode(text, &at, &codepoint);
        char encoded[UTF8_MAX_BYTES];
        str_builder_append(&builder, (String){ encoded, utf8_encode(codepoint, encoded) });
    }
    return str_builder_finish(&builder);
}

static yyjson_mut_val *string_value(yyjson_mut_doc *doc, Arena *arena, String text, bool replace_invalid_utf8)
{
    String written = text_for_writer(arena, text, replace_invalid_utf8);
    return yyjson_mut_strn(doc, written.data, written.len);
}

static yyjson_mut_val *to_writer(yyjson_mut_doc *doc, Arena *arena, const Node *node, bool replace_invalid_utf8)
{
    switch (node->kind) {
    case NODE_NULL: return yyjson_mut_null(doc);
    case NODE_BOOL: return yyjson_mut_bool(doc, node->boolean);
    case NODE_INT: return yyjson_mut_sint(doc, node->integer);
    case NODE_FLOAT: return yyjson_mut_real(doc, node->number);
    case NODE_STRING: return string_value(doc, arena, node->text, replace_invalid_utf8);
    case NODE_SEQUENCE: {
        yyjson_mut_val *array = yyjson_mut_arr(doc);
        for (size_t i = 0; i < node->count; i++) {
            yyjson_mut_arr_append(array, to_writer(doc, arena, node->items[i], replace_invalid_utf8));
        }
        return array;
    }
    case NODE_MAPPING: {
        yyjson_mut_val *object = yyjson_mut_obj(doc);
        for (size_t i = 0; i < node->entry_count; i++) {
            const NodeEntry *entry = &node->entries[i];
            yyjson_mut_obj_add(object, string_value(doc, arena, entry->key, replace_invalid_utf8),
                               to_writer(doc, arena, entry->value, replace_invalid_utf8));
        }
        return object;
    }
    }
    unreachable();
}

static bool write_json(Arena *arena, const Node *node, bool replace_invalid_utf8, String *json)
{
    yyjson_alc alc = arena_allocator(arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    yyjson_mut_doc_set_root(doc, to_writer(doc, arena, node, replace_invalid_utf8));
    size_t len;
    char *written = yyjson_mut_write_opts(doc, YYJSON_WRITE_INF_AND_NAN_AS_NULL, &alc, &len, nullptr);
    if (written == nullptr) {
        return false;
    }
    *json = (String){ written, len };
    return true;
}

// json_encode writes strings as they are and only on a writer error, which
// is invalid UTF-8, does it write again with every string checked.
String json_encode(Arena *arena, const Node *node)
{
    String json;
    if (write_json(arena, node, false, &json) || write_json(arena, node, true, &json)) {
        return json;
    }
    abort();
}
