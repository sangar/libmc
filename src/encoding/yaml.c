// Wrapper around cyaml, the only file that includes it. cyaml allocates with
// malloc, so its tree is copied into the arena and freed before returning.
#include "mc/encoding/yaml.h"

#include <math.h>
#include <stdlib.h>

#include "cyaml.h"
#include "mc/container/strmap.h"
#include "mc/platform/platform.h"

// Anchored remembers the node made for an anchored cyaml node, so that
// aliases share it; done is false while its children are being converted.
typedef struct Anchored {
    Node *node;
    bool done;
} Anchored;

typedef struct Converter {
    Arena *arena;
    const cyaml_doc_t *doc;
    StrMap *anchored; // cyaml node address -> Anchored
    Err *err;
} Converter;

[[nodiscard]] static Error fail_at(Err *err, cyaml_span_t span, const char *what)
{
    return err_set(err, ERR_PARSE, "yaml: line %u, column %u: %s", span.start_line, span.start_col, what);
}

static String address_key(const cyaml_node_t *const *source)
{
    return (String){ (const char *)source, sizeof *source };
}

static String span_text(const Converter *c, cyaml_span_t span)
{
    return span.len == 0 ? S("") : (String){ cyaml_span_ptr(c->doc, span), span.len };
}

static String scalar_text(Converter *c, const cyaml_node_t *source)
{
    char *text = cyaml_scalar_str(c->doc, source);
    String copy = str_copy(c->arena, text != nullptr ? S(text) : S(""));
    free(text); // modern-c: allow malloc
    return copy;
}

static bool is_string_tag(String tag)
{
    return str_equal(tag, S("!")) || str_equal(tag, S("!!str")) || str_equal(tag, S("!<tag:yaml.org,2002:str>"));
}

static bool parse_digits(String digits, unsigned base, int64_t *value)
{
    if (digits.len == 0) {
        return false;
    }
    uint64_t total = 0;
    for (size_t i = 0; i < digits.len; i++) {
        char c = digits.data[i];
        unsigned digit = c >= '0' && c <= '9'   ? (unsigned)(c - '0')
                         : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
                         : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10)
                                                : base;
        if (digit >= base || total > ((uint64_t)INT64_MAX - digit) / base) {
            return false;
        }
        total = total * base + digit;
    }
    *value = (int64_t)total;
    return true;
}

// parse_integer reads the core schema's integer forms; false when the value does not fit int64_t.
static bool parse_integer(String text, int64_t *value)
{
    if (str_starts_with(text, S("0x"))) {
        return parse_digits(str_slice(text, 2, text.len), 16, value);
    }
    if (str_starts_with(text, S("0o"))) {
        return parse_digits(str_slice(text, 2, text.len), 8, value);
    }
    return str_parse_i64(text, value);
}

static bool parse_float(String text, double *value)
{
    String magnitude = str_trim_prefix(str_trim_prefix(text, S("+")), S("-"));
    if (str_equal_ignore_case(magnitude, S(".inf"))) {
        *value = str_starts_with(text, S("-")) ? -INFINITY : INFINITY;
        return true;
    }
    if (str_equal_ignore_case(text, S(".nan"))) {
        *value = NAN;
        return true;
    }
    return float_parse(text, value);
}

static Node *convert_scalar(Converter *c, const cyaml_node_t *source)
{
    String text = scalar_text(c, source);
    if (source->style != CYAML_PLAIN || is_string_tag(span_text(c, source->tag))) {
        return node_scalar(c->arena, NODE_STRING, text);
    }
    cyaml_scalar_kind_t kind = cyaml_scalar_kind(c->doc, source);
    if (kind == CYAML_KIND_NULL) {
        return node_scalar(c->arena, NODE_NULL, text);
    }
    if (kind == CYAML_KIND_BOOL) {
        Node *node = node_scalar(c->arena, NODE_BOOL, text);
        node->boolean = text.data[0] == 't' || text.data[0] == 'T';
        return node;
    }
    int64_t integer;
    if (kind == CYAML_KIND_INT && parse_integer(text, &integer)) {
        Node *node = node_scalar(c->arena, NODE_INT, text);
        node->integer = integer;
        node->number = (double)integer;
        return node;
    }
    double number;
    if ((kind == CYAML_KIND_INT || kind == CYAML_KIND_FLOAT) && parse_float(text, &number)) {
        Node *node = node_scalar(c->arena, NODE_FLOAT, text);
        node->number = number;
        return node;
    }
    return node_scalar(c->arena, NODE_STRING, text);
}

[[nodiscard]] static Error convert(Converter *c, const cyaml_node_t *source, Node **out);

[[nodiscard]] static Error convert_alias(Converter *c, const cyaml_node_t *source, Node **out)
{
    const cyaml_node_t *target = source->alias.target;
    if (target == nullptr) {
        return fail_at(c->err, source->anchor, "alias names no anchor");
    }
    Anchored *anchored = strmap_get(c->anchored, address_key(&target));
    if (anchored == nullptr) {
        return convert(c, target, out);
    }
    if (!anchored->done) {
        return fail_at(c->err, source->anchor, "alias inside the node it names");
    }
    *out = anchored->node;
    return ERR_OK;
}

[[nodiscard]] static Error convert_key(Converter *c, const cyaml_node_t *source, String *key)
{
    if (source != nullptr && source->type == CYAML_ALIAS && source->alias.target != nullptr) {
        source = source->alias.target;
    }
    if (source == nullptr || source->type == CYAML_NULL || source->type == CYAML_NONE) {
        *key = S("");
        return ERR_OK;
    }
    if (source->type != CYAML_SCALAR) {
        return fail_at(c->err, source->span, "a mapping key must be a scalar");
    }
    *key = scalar_text(c, source);
    return ERR_OK;
}

[[nodiscard]] static Error convert_children(Converter *c, const cyaml_node_t *source, Node *node)
{
    Error e = ERR_OK;
    if (source->type == CYAML_SEQ) {
        for (uint32_t i = 0; e == ERR_OK && i < source->seq.count; i++) {
            Node *item;
            e = convert(c, source->seq.items[i], &item);
            if (e == ERR_OK) {
                node_append(c->arena, node, item);
            }
        }
        return e;
    }
    for (uint32_t i = 0; e == ERR_OK && i < source->map.count; i++) {
        String key;
        Node *value;
        e = convert_key(c, source->map.pairs[i].key, &key);
        if (e == ERR_OK) {
            e = convert(c, source->map.pairs[i].val, &value);
        }
        if (e == ERR_OK) {
            node_put(c->arena, node, key, value);
        }
    }
    return e;
}

[[nodiscard]] static Error convert(Converter *c, const cyaml_node_t *source, Node **out)
{
    if (source == nullptr || source->type == CYAML_NULL || source->type == CYAML_NONE) {
        *out = node_create(c->arena, NODE_NULL);
        return ERR_OK;
    }
    if (source->type == CYAML_ALIAS) {
        return convert_alias(c, source, out);
    }
    if (source->type == CYAML_SCALAR) {
        *out = convert_scalar(c, source);
    } else {
        *out = node_create(c->arena, source->type == CYAML_SEQ ? NODE_SEQUENCE : NODE_MAPPING);
    }
    Anchored *anchored = nullptr;
    if (source->anchor.len > 0) {
        anchored = arena_push(c->arena, sizeof *anchored);
        anchored->node = *out;
        strmap_put(c->anchored, address_key(&source), anchored);
    }
    Error e = source->type == CYAML_SCALAR ? ERR_OK : convert_children(c, source, *out);
    if (anchored != nullptr) {
        anchored->done = true;
    }
    return e;
}

Error yaml_parse(Arena *arena, String text, Node **root, Err *err)
{
    *root = nullptr;
    if (text.len > UINT32_MAX) {
        return err_set(err, ERR_INVALID_ARGUMENT, "yaml: %zu bytes is more than the parser takes", text.len);
    }
    cyaml_error_t error = { 0 };
    cyaml_stream_t *stream = cyaml_parse_stream(text.data, text.len, nullptr, &error);
    if (stream == nullptr) {
        if (error.code == CYAML_ERR_NOMEM) {
            return err_set(err, ERR_OUT_OF_MEMORY, "yaml: out of memory");
        }
        return fail_at(err, error.span, error.msg);
    }
    Error e = ERR_OK;
    if (stream->count > 1) {
        const cyaml_node_t *second = stream->docs[1]->root;
        e = fail_at(err, second != nullptr ? second->span : (cyaml_span_t){ 0 }, "more than one document");
    } else {
        Converter c = {
            .arena = arena,
            .doc = stream->count == 1 ? stream->docs[0] : nullptr,
            .anchored = strmap_create(arena, 0),
            .err = err,
        };
        e = convert(&c, c.doc != nullptr ? c.doc->root : nullptr, root);
    }
    cyaml_stream_free(stream);
    if (e != ERR_OK) {
        *root = nullptr;
    }
    return e;
}
