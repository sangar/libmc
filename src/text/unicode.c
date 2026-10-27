#include "mc/text/unicode.h"

#include <stdlib.h>

#include "mc/text/utf8.h"
#include "utf8proc.h"

uint32_t unicode_lower_codepoint(uint32_t codepoint)
{
    return (uint32_t)utf8proc_tolower((utf8proc_int32_t)codepoint);
}

static uint32_t upper(uint32_t codepoint)
{
    return (uint32_t)utf8proc_toupper((utf8proc_int32_t)codepoint);
}

static uint32_t identity(uint32_t codepoint)
{
    return codepoint;
}

// fold maps through upper case first so that letters with several lower case
// forms, such as σ and ς, fold together.
static uint32_t fold(uint32_t codepoint)
{
    return unicode_lower_codepoint(upper(codepoint));
}

static String map_code_points(Arena *arena, String s, uint32_t (*map)(uint32_t codepoint))
{
    StringBuilder builder = str_builder_create(arena, s.len);
    char encoded[UTF8_MAX_BYTES];
    for (size_t position = 0; position < s.len;) {
        uint32_t codepoint;
        utf8_decode(s, &position, &codepoint);
        str_builder_append(&builder, (String){ encoded, utf8_encode(map(codepoint), encoded) });
    }
    return str_builder_finish(&builder);
}

String unicode_nfc(Arena *arena, String s)
{
    if (!utf8_is_valid(s)) {
        s = map_code_points(arena, s, identity);
    }
    const utf8proc_uint8_t *bytes = (const utf8proc_uint8_t *)s.data;
    utf8proc_ssize_t len = (utf8proc_ssize_t)s.len;
    unsigned options = UTF8PROC_STABLE | UTF8PROC_COMPOSE;
    utf8proc_ssize_t count = utf8proc_decompose(bytes, len, nullptr, 0, options);
    if (count < 0) {
        abort();
    }
    // utf8proc_reencode writes the UTF-8 over the code points, at most four
    // bytes for each four-byte slot, so one more slot holds the terminator.
    size_t slots = (size_t)count + 1;
    utf8proc_int32_t *buffer = arena_push(arena, arena_size_mul(slots, sizeof *buffer));
    if (utf8proc_decompose(bytes, len, buffer, count, options) != count) {
        abort();
    }
    utf8proc_ssize_t written = utf8proc_reencode(buffer, count, options);
    if (written < 0) {
        abort();
    }
    return (String){ .data = (const char *)buffer, .len = (size_t)written };
}

String unicode_lower(Arena *arena, String s)
{
    return map_code_points(arena, s, unicode_lower_codepoint);
}

String unicode_upper(Arena *arena, String s)
{
    return map_code_points(arena, s, upper);
}

bool unicode_equal_ignore_case(String a, String b)
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.len && j < b.len) {
        uint32_t x;
        uint32_t y;
        utf8_decode(a, &i, &x);
        utf8_decode(b, &j, &y);
        if (fold(x) != fold(y)) {
            return false;
        }
    }
    return i == a.len && j == b.len;
}

// norwegian_key spreads code points four apart to make room for Æ, Ø and Å
// right after Z, and for æ, ø and å right after z.
static uint32_t norwegian_key(uint32_t codepoint)
{
    switch (codepoint) {
    case 0xC6: return 'Z' * 4 + 1;
    case 0xD8: return 'Z' * 4 + 2;
    case 0xC5: return 'Z' * 4 + 3;
    case 0xE6: return 'z' * 4 + 1;
    case 0xF8: return 'z' * 4 + 2;
    case 0xE5: return 'z' * 4 + 3;
    default: return codepoint * 4;
    }
}

static int compare_norwegian(String a, String b, uint32_t (*map)(uint32_t codepoint))
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.len && j < b.len) {
        uint32_t x;
        uint32_t y;
        utf8_decode(a, &i, &x);
        utf8_decode(b, &j, &y);
        uint32_t key_x = norwegian_key(map(x));
        uint32_t key_y = norwegian_key(map(y));
        if (key_x != key_y) {
            return key_x < key_y ? -1 : 1;
        }
    }
    bool a_done = i == a.len;
    bool b_done = j == b.len;
    return a_done && b_done ? 0 : a_done ? -1 : 1;
}

int unicode_compare_norwegian(String a, String b)
{
    return compare_norwegian(a, b, identity);
}

int unicode_compare_norwegian_ignore_case(String a, String b)
{
    return compare_norwegian(a, b, fold);
}

static int compare_norwegian_items(const void *a, const void *b)
{
    String x = *(const String *)a;
    String y = *(const String *)b;
    int order = unicode_compare_norwegian_ignore_case(x, y);
    return order != 0 ? order : unicode_compare_norwegian(x, y);
}

void unicode_sort_norwegian(StringList *list)
{
    if (list->count > 1) {
        qsort(list->items, list->count, sizeof *list->items, compare_norwegian_items);
    }
}
