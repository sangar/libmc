#include "mc/encoding/json.h"

#include <math.h>

#include "mc/platform/platform.h"
#include "mc/text/utf8.h"

// MAX_DEPTH bounds the recursion, so hostile input cannot exhaust the stack.
enum { MAX_DEPTH = 512 };

typedef struct {
    Arena *arena;
    String text;
    size_t at;
    int depth;
    Err *err;
    bool failed;
} Parser;

static void fail(Parser *parser, size_t at, const char *what)
{
    if (parser->failed) {
        return;
    }
    parser->failed = true;
    size_t line = 1;
    size_t column = 1;
    for (size_t i = 0; i < at && i < parser->text.len; i++) {
        if (parser->text.data[i] == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }
    unused(err_set(parser->err, ERR_PARSE, "json: line %zu, column %zu: %s", line, column, what));
}

static bool at_end(const Parser *parser)
{
    return parser->at >= parser->text.len;
}

static char peek(const Parser *parser)
{
    return at_end(parser) ? '\0' : parser->text.data[parser->at];
}

static void skip_space(Parser *parser)
{
    for (char c = peek(parser); c == ' ' || c == '\t' || c == '\n' || c == '\r'; c = peek(parser)) {
        parser->at++;
    }
}

static bool consume(Parser *parser, String word)
{
    if (!str_starts_with(str_slice(parser->text, parser->at, parser->text.len), word)) {
        return false;
    }
    parser->at += word.len;
    return true;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static size_t skip_digits(Parser *parser)
{
    size_t start = parser->at;
    while (is_digit(peek(parser))) {
        parser->at++;
    }
    return parser->at - start;
}

static Node *parse_number(Parser *parser)
{
    size_t start = parser->at;
    consume(parser, S("-"));
    if (!consume(parser, S("0")) && skip_digits(parser) == 0) {
        fail(parser, parser->at, "digits expected");
        return nullptr;
    }
    bool integral = true;
    if (consume(parser, S("."))) {
        integral = false;
        if (skip_digits(parser) == 0) {
            fail(parser, parser->at, "digits expected after '.'");
            return nullptr;
        }
    }
    if (consume(parser, S("e")) || consume(parser, S("E"))) {
        integral = false;
        if (!consume(parser, S("+"))) {
            consume(parser, S("-"));
        }
        if (skip_digits(parser) == 0) {
            fail(parser, parser->at, "digits expected in the exponent");
            return nullptr;
        }
    }
    Node *node = node_scalar(parser->arena, NODE_INT, str_slice(parser->text, start, parser->at));
    if (integral && str_parse_i64(node->text, &node->integer)) {
        node->number = (double)node->integer;
        return node;
    }
    node->kind = NODE_FLOAT;
    if (!float_parse(node->text, &node->number)) {
        fail(parser, start, "number out of range");
        return nullptr;
    }
    return node;
}

static bool read_hex4(Parser *parser, uint32_t *value)
{
    if (parser->text.len - parser->at < 4) {
        return false;
    }
    uint32_t result = 0;
    for (size_t i = 0; i < 4; i++) {
        char c = parser->text.data[parser->at + i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
        result = result << 4 | digit;
    }
    parser->at += 4;
    *value = result;
    return true;
}

// parse_unicode_escape reads the hex digits after "\u", joining a surrogate
// pair; a lone surrogate becomes U+FFFD.
static bool parse_unicode_escape(Parser *parser, uint32_t *codepoint)
{
    if (!read_hex4(parser, codepoint)) {
        return false;
    }
    bool high_surrogate = *codepoint >= 0xD800 && *codepoint < 0xDC00;
    size_t after = parser->at;
    uint32_t low;
    if (high_surrogate && consume(parser, S("\\u")) && read_hex4(parser, &low) && low >= 0xDC00 && low < 0xE000) {
        *codepoint = 0x10000 + ((*codepoint - 0xD800) << 10) + (low - 0xDC00);
    } else {
        parser->at = after;
    }
    return true;
}

static bool parse_escape(Parser *parser, StringBuilder *builder)
{
    size_t start = parser->at++;
    char escape = peek(parser);
    parser->at++;
    switch (escape) {
    case '"':
    case '\\':
    case '/': str_builder_append_char(builder, escape); return true;
    case 'b': str_builder_append_char(builder, '\b'); return true;
    case 'f': str_builder_append_char(builder, '\f'); return true;
    case 'n': str_builder_append_char(builder, '\n'); return true;
    case 'r': str_builder_append_char(builder, '\r'); return true;
    case 't': str_builder_append_char(builder, '\t'); return true;
    case 'u': {
        uint32_t codepoint;
        if (!parse_unicode_escape(parser, &codepoint)) {
            fail(parser, start, "invalid \\u escape");
            return false;
        }
        char encoded[UTF8_MAX_BYTES];
        str_builder_append(builder, (String){ encoded, utf8_encode(codepoint, encoded) });
        return true;
    }
    default: fail(parser, start, "invalid escape"); return false;
    }
}

// parse_string reads the string literal at the opening quote into the arena.
static bool parse_string(Parser *parser, String *value)
{
    size_t open = parser->at++;
    StringBuilder builder = str_builder_create(parser->arena, 16);
    size_t run = parser->at;
    for (;;) {
        if (at_end(parser)) {
            fail(parser, open, "unterminated string");
            return false;
        }
        unsigned char c = (unsigned char)peek(parser);
        if (c == '"' || c == '\\') {
            str_builder_append(&builder, str_slice(parser->text, run, parser->at));
            if (c == '"') {
                parser->at++;
                *value = str_builder_finish(&builder);
                return true;
            }
            if (!parse_escape(parser, &builder)) {
                return false;
            }
            run = parser->at;
        } else if (c < 0x20) {
            fail(parser, parser->at, "control character in string");
            return false;
        } else if (c >= 0x80) {
            size_t start = parser->at;
            uint32_t codepoint;
            if (!utf8_decode(parser->text, &parser->at, &codepoint)) {
                fail(parser, start, "invalid UTF-8 in string");
                return false;
            }
        } else {
            parser->at++;
        }
    }
}

static Node *parse_value(Parser *parser);

static Node *parse_array(Parser *parser)
{
    Node *array = node_create(parser->arena, NODE_SEQUENCE);
    parser->at++;
    skip_space(parser);
    if (consume(parser, S("]"))) {
        return array;
    }
    for (;;) {
        Node *item = parse_value(parser);
        if (item == nullptr) {
            return nullptr;
        }
        node_append(parser->arena, array, item);
        skip_space(parser);
        if (consume(parser, S("]"))) {
            return array;
        }
        if (!consume(parser, S(","))) {
            fail(parser, parser->at, "expected ',' or ']'");
            return nullptr;
        }
    }
}

static Node *parse_object(Parser *parser)
{
    Node *object = node_create(parser->arena, NODE_MAPPING);
    parser->at++;
    skip_space(parser);
    if (consume(parser, S("}"))) {
        return object;
    }
    for (;;) {
        skip_space(parser);
        String key;
        if (peek(parser) != '"') {
            fail(parser, parser->at, "expected a string key");
            return nullptr;
        }
        if (!parse_string(parser, &key)) {
            return nullptr;
        }
        skip_space(parser);
        if (!consume(parser, S(":"))) {
            fail(parser, parser->at, "expected ':' after the key");
            return nullptr;
        }
        Node *value = parse_value(parser);
        if (value == nullptr) {
            return nullptr;
        }
        node_put(parser->arena, object, key, value);
        skip_space(parser);
        if (consume(parser, S("}"))) {
            return object;
        }
        if (!consume(parser, S(","))) {
            fail(parser, parser->at, "expected ',' or '}'");
            return nullptr;
        }
    }
}

static Node *literal(Parser *parser, NodeKind kind, String text)
{
    Node *node = node_create(parser->arena, kind);
    node->text = text;
    node->boolean = kind == NODE_BOOL && str_equal(text, S("true"));
    return node;
}

static Node *parse_nested(Parser *parser)
{
    char c = peek(parser);
    if (c == '{') {
        return parse_object(parser);
    }
    if (c == '[') {
        return parse_array(parser);
    }
    if (c == '"') {
        String text;
        return parse_string(parser, &text) ? literal(parser, NODE_STRING, text) : nullptr;
    }
    if (c == '-' || is_digit(c)) {
        return parse_number(parser);
    }
    if (consume(parser, S("true"))) {
        return literal(parser, NODE_BOOL, S("true"));
    }
    if (consume(parser, S("false"))) {
        return literal(parser, NODE_BOOL, S("false"));
    }
    if (consume(parser, S("null"))) {
        return literal(parser, NODE_NULL, S("null"));
    }
    fail(parser, parser->at, at_end(parser) ? "unexpected end of input" : "expected a value");
    return nullptr;
}

static Node *parse_value(Parser *parser)
{
    skip_space(parser);
    if (parser->depth == MAX_DEPTH) {
        fail(parser, parser->at, "nested too deeply");
        return nullptr;
    }
    parser->depth++;
    Node *node = parse_nested(parser);
    parser->depth--;
    return node;
}

Error json_parse(Arena *arena, String text, Node **root, Err *err)
{
    Parser parser = { .arena = arena, .text = text, .err = err };
    Node *value = parse_value(&parser);
    skip_space(&parser);
    if (value != nullptr && !at_end(&parser)) {
        fail(&parser, parser.at, "unexpected text after the value");
    }
    if (parser.failed) {
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

static void encode(StringBuilder *builder, const Node *node)
{
    switch (node->kind) {
    case NODE_NULL: str_builder_append(builder, S("null")); return;
    case NODE_BOOL: str_builder_append(builder, node->boolean ? S("true") : S("false")); return;
    case NODE_INT: str_builder_append_format(builder, "%lld", (long long)node->integer); return;
    case NODE_FLOAT:
        str_builder_append(builder, isfinite(node->number) ? float_format(builder->arena, node->number) : S("null"));
        return;
    case NODE_STRING: json_append_quoted(builder, node->text); return;
    case NODE_SEQUENCE:
        str_builder_append_char(builder, '[');
        for (size_t i = 0; i < node->count; i++) {
            if (i > 0) {
                str_builder_append_char(builder, ',');
            }
            encode(builder, node->items[i]);
        }
        str_builder_append_char(builder, ']');
        return;
    case NODE_MAPPING:
        str_builder_append_char(builder, '{');
        for (size_t i = 0; i < node->entry_count; i++) {
            if (i > 0) {
                str_builder_append_char(builder, ',');
            }
            json_append_quoted(builder, node->entries[i].key);
            str_builder_append_char(builder, ':');
            encode(builder, node->entries[i].value);
        }
        str_builder_append_char(builder, '}');
        return;
    }
}

String json_encode(Arena *arena, const Node *node)
{
    StringBuilder builder = str_builder_create(arena, 64);
    encode(&builder, node);
    return str_builder_finish(&builder);
}
