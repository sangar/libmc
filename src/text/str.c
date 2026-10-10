#include "mc/text/str.h"

#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_bytes(char *to, String from)
{
    if (from.len > 0) {
        memcpy(to, from.data, from.len);
    }
}

String S(const char *cstr)
{
    return (String){ .data = cstr == nullptr ? "" : cstr, .len = cstr == nullptr ? 0 : strlen(cstr) };
}

bool str_equal(String a, String b)
{
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

static char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static char upper_ascii(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - ('a' - 'A')) : c;
}

static bool is_ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool str_equal_ignore_case(String a, String b)
{
    if (a.len != b.len) {
        return false;
    }
    for (size_t i = 0; i < a.len; i++) {
        if (lower_ascii(a.data[i]) != lower_ascii(b.data[i])) {
            return false;
        }
    }
    return true;
}

int str_compare(String a, String b)
{
    size_t common = min_size(a.len, b.len);
    int order = common > 0 ? memcmp(a.data, b.data, common) : 0;
    if (order != 0) {
        return order;
    }
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

int str_compare_ignore_case(String a, String b)
{
    size_t common = min_size(a.len, b.len);
    for (size_t i = 0; i < common; i++) {
        unsigned char x = (unsigned char)lower_ascii(a.data[i]);
        unsigned char y = (unsigned char)lower_ascii(b.data[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

bool str_starts_with(String s, String prefix)
{
    return s.len >= prefix.len && str_equal(str_slice(s, 0, prefix.len), prefix);
}

bool str_ends_with(String s, String suffix)
{
    return s.len >= suffix.len && str_equal(str_slice(s, s.len - suffix.len, s.len), suffix);
}

// memchr skips to each occurrence of the needle's first byte, so the full
// comparison runs only where a match can start.
bool str_find(String s, String needle, size_t *index)
{
    if (needle.len == 0) {
        *index = 0;
        return true;
    }
    size_t start = 0;
    while (start + needle.len <= s.len) {
        const char *first = memchr(s.data + start, needle.data[0], s.len - needle.len + 1 - start);
        if (first == nullptr) {
            return false;
        }
        start = (size_t)(first - s.data);
        if (memcmp(first, needle.data, needle.len) == 0) {
            *index = start;
            return true;
        }
        start++;
    }
    return false;
}

bool str_find_char(String s, char c, size_t *index)
{
    const char *found = s.len > 0 ? memchr(s.data, c, s.len) : nullptr;
    if (found == nullptr) {
        return false;
    }
    *index = (size_t)(found - s.data);
    return true;
}

bool str_find_last_char(String s, char c, size_t *index)
{
    for (size_t i = s.len; i > 0; i--) {
        if (s.data[i - 1] == c) {
            *index = i - 1;
            return true;
        }
    }
    return false;
}

bool str_contains(String s, String needle)
{
    size_t index;
    return str_find(s, needle, &index);
}

bool str_contains_any(String s, String chars)
{
    size_t index;
    for (size_t i = 0; i < s.len; i++) {
        if (str_find_char(chars, s.data[i], &index)) {
            return true;
        }
    }
    return false;
}

size_t str_count_char(String s, char c)
{
    size_t count = 0;
    for (size_t i = 0; i < s.len; i++) {
        count += s.data[i] == c;
    }
    return count;
}

String str_slice(String s, size_t start, size_t end)
{
    end = min_size(end, s.len);
    start = min_size(start, end);
    return (String){ .data = s.data + start, .len = end - start };
}

String str_trim(String s)
{
    size_t start = 0;
    size_t end = s.len;
    while (start < end && is_ascii_space(s.data[start])) {
        start++;
    }
    while (end > start && is_ascii_space(s.data[end - 1])) {
        end--;
    }
    return str_slice(s, start, end);
}

String str_trim_prefix(String s, String prefix)
{
    return str_starts_with(s, prefix) ? str_slice(s, prefix.len, s.len) : s;
}

String str_trim_suffix(String s, String suffix)
{
    return str_ends_with(s, suffix) ? str_slice(s, 0, s.len - suffix.len) : s;
}

String str_first_line(String s)
{
    size_t index;
    return str_find_char(s, '\n', &index) ? str_slice(s, 0, index) : s;
}

bool str_cut(String s, char separator, String *head, String *tail)
{
    size_t index;
    if (!str_find_char(s, separator, &index)) {
        *head = s;
        *tail = str_slice(s, s.len, s.len);
        return false;
    }
    *head = str_slice(s, 0, index);
    *tail = str_slice(s, index + 1, s.len);
    return true;
}

bool str_parse_u64(String s, uint64_t *value)
{
    if (s.len == 0) {
        return false;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < s.len; i++) {
        char c = s.data[i];
        if (c < '0' || c > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(c - '0');
        if (result > (UINT64_MAX - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

bool str_parse_i64(String s, int64_t *value)
{
    bool negative = s.len > 0 && s.data[0] == '-';
    if (s.len > 0 && (s.data[0] == '-' || s.data[0] == '+')) {
        s = str_slice(s, 1, s.len);
    }
    uint64_t magnitude;
    if (!str_parse_u64(s, &magnitude)) {
        return false;
    }
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    if (magnitude > limit) {
        return false;
    }
    *value = negative ? (int64_t)(0 - magnitude) : (int64_t)magnitude;
    return true;
}

bool str_is_digits(String s)
{
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] < '0' || s.data[i] > '9') {
            return false;
        }
    }
    return s.len > 0;
}

static char *push_text(Arena *arena, size_t len)
{
    return arena_push_aligned(arena, arena_size_add(len, 1), 1);
}

String str_copy(Arena *arena, String s)
{
    char *data = push_text(arena, s.len);
    copy_bytes(data, s);
    return (String){ .data = data, .len = s.len };
}

const char *str_cstr(Arena *arena, String s)
{
    return str_copy(arena, s).data;
}

String str_format_va(Arena *arena, const char *format, va_list args)
{
    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (needed < 0) {
        return S("");
    }
    size_t len = (size_t)needed;
    char *data = push_text(arena, len);
    vsnprintf(data, len + 1, format, args);
    return (String){ .data = data, .len = len };
}

String str_format(Arena *arena, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    String s = str_format_va(arena, format, args);
    va_end(args);
    return s;
}

String str_concat(Arena *arena, String a, String b)
{
    size_t len = arena_size_add(a.len, b.len);
    char *data = push_text(arena, len);
    copy_bytes(data, a);
    copy_bytes(data + a.len, b);
    return (String){ .data = data, .len = len };
}

String str_repeat(Arena *arena, String s, size_t times)
{
    size_t len = arena_size_mul(s.len, times);
    char *data = push_text(arena, len);
    for (size_t at = 0; at < len; at += s.len) {
        copy_bytes(data + at, s);
    }
    return (String){ .data = data, .len = len };
}

String str_replace_all(Arena *arena, String s, String old, String replacement)
{
    if (old.len == 0) {
        return str_copy(arena, s);
    }
    StringBuilder builder = str_builder_create(arena, s.len);
    String rest = s;
    size_t at;
    while (str_find(rest, old, &at)) {
        str_builder_append(&builder, str_slice(rest, 0, at));
        str_builder_append(&builder, replacement);
        rest = str_slice(rest, at + old.len, rest.len);
    }
    str_builder_append(&builder, rest);
    return str_builder_finish(&builder);
}

static String map_ascii(Arena *arena, String s, char (*map)(char))
{
    char *data = push_text(arena, s.len);
    for (size_t i = 0; i < s.len; i++) {
        data[i] = map(s.data[i]);
    }
    return (String){ .data = data, .len = s.len };
}

String str_lower_ascii(Arena *arena, String s)
{
    return map_ascii(arena, s, lower_ascii);
}

String str_upper_ascii(Arena *arena, String s)
{
    return map_ascii(arena, s, upper_ascii);
}

String str_quote(Arena *arena, String s)
{
    StringBuilder builder = str_builder_create(arena, s.len + 2);
    str_builder_append_char(&builder, '"');
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        switch (c) {
        case '"': str_builder_append(&builder, S("\\\"")); break;
        case '\\': str_builder_append(&builder, S("\\\\")); break;
        case '\n': str_builder_append(&builder, S("\\n")); break;
        case '\r': str_builder_append(&builder, S("\\r")); break;
        case '\t': str_builder_append(&builder, S("\\t")); break;
        default:
            if (c < 0x20 || c == 0x7f) {
                str_builder_append_format(&builder, "\\x%02x", c);
            } else {
                str_builder_append_char(&builder, (char)c);
            }
        }
    }
    str_builder_append_char(&builder, '"');
    return str_builder_finish(&builder);
}

static bool is_unreserved(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

String str_percent_encode(Arena *arena, String s, bool keep_slash)
{
    static const char hex[] = "0123456789ABCDEF";
    StringBuilder builder = str_builder_create(arena, s.len);
    for (size_t i = 0; i < s.len; i++) {
        char c = s.data[i];
        if (is_unreserved(c) || (keep_slash && c == '/')) {
            str_builder_append_char(&builder, c);
            continue;
        }
        unsigned char byte = (unsigned char)c;
        str_builder_append_char(&builder, '%');
        str_builder_append_char(&builder, hex[byte >> 4]);
        str_builder_append_char(&builder, hex[byte & 15]);
    }
    return str_builder_finish(&builder);
}

StringList str_split(Arena *arena, String s, char separator)
{
    StringList list = { 0 };
    size_t start = 0;
    for (size_t i = 0; i <= s.len; i++) {
        if (i == s.len || s.data[i] == separator) {
            strlist_push(arena, &list, str_slice(s, start, i));
            start = i + 1;
        }
    }
    return list;
}

StringList str_fields(Arena *arena, String s)
{
    StringList list = { 0 };
    size_t i = 0;
    while (i < s.len) {
        while (i < s.len && is_ascii_space(s.data[i])) {
            i++;
        }
        size_t start = i;
        while (i < s.len && !is_ascii_space(s.data[i])) {
            i++;
        }
        if (i > start) {
            strlist_push(arena, &list, str_slice(s, start, i));
        }
    }
    return list;
}

String str_join(Arena *arena, StringList list, String separator)
{
    size_t total = 0;
    for (size_t i = 0; i < list.count; i++) {
        total = arena_size_add(total, list.items[i].len);
        if (i > 0) {
            total = arena_size_add(total, separator.len);
        }
    }
    char *data = push_text(arena, total);
    size_t at = 0;
    for (size_t i = 0; i < list.count; i++) {
        if (i > 0) {
            copy_bytes(data + at, separator);
            at += separator.len;
        }
        copy_bytes(data + at, list.items[i]);
        at += list.items[i].len;
    }
    return (String){ .data = data, .len = total };
}

void strlist_push(Arena *arena, StringList *list, String s)
{
    list->items = arena_grow(arena, list->items, &list->capacity, list->count, sizeof *list->items);
    list->items[list->count++] = s;
}

void strlist_push_all(Arena *arena, StringList *list, StringList more)
{
    for (size_t i = 0; i < more.count; i++) {
        strlist_push(arena, list, more.items[i]);
    }
}

StringList strlist_copy(Arena *arena, StringList list)
{
    StringList copy = { 0 };
    strlist_push_all(arena, &copy, list);
    return copy;
}

StringList strlist_of(Arena *arena, size_t count, const char *const *items)
{
    StringList list = { 0 };
    for (size_t i = 0; i < count; i++) {
        strlist_push(arena, &list, S(items[i]));
    }
    return list;
}

StringList strlist_from_lines(Arena *arena, String s)
{
    if (s.len == 0) {
        return (StringList){ 0 };
    }
    return str_split(arena, s, '\n');
}

bool strlist_contains(StringList list, String s)
{
    for (size_t i = 0; i < list.count; i++) {
        if (str_equal(list.items[i], s)) {
            return true;
        }
    }
    return false;
}

static int compare_strings(const void *a, const void *b)
{
    return str_compare(*(const String *)a, *(const String *)b);
}

void strlist_sort(StringList *list)
{
    if (list->count > 1) {
        qsort(list->items, list->count, sizeof *list->items, compare_strings);
    }
}

StringBuilder str_builder_create(Arena *arena, size_t capacity)
{
    capacity = max_size(capacity, 16);
    return (StringBuilder){ .arena = arena, .data = push_text(arena, capacity), .len = 0, .capacity = capacity };
}

// reserve keeps room for extra bytes and the terminator. Appends call it for
// every piece, so the common case, room to spare, is checked inline.
static void reserve(StringBuilder *builder, size_t extra)
{
    size_t needed;
    if (!ckd_add(&needed, builder->len, extra) && needed < builder->capacity) {
        return;
    }
    needed = arena_size_add(arena_size_add(builder->len, extra), 1);
    size_t capacity = max_size(needed, arena_size_mul(builder->capacity, 2));
    char *data = push_text(builder->arena, capacity);
    copy_bytes(data, (String){ .data = builder->data, .len = builder->len });
    builder->data = data;
    builder->capacity = capacity;
}

void str_builder_append(StringBuilder *builder, String s)
{
    reserve(builder, s.len);
    copy_bytes(builder->data + builder->len, s);
    builder->len += s.len;
}

void str_builder_append_char(StringBuilder *builder, char c)
{
    reserve(builder, 1);
    builder->data[builder->len++] = c;
}

void str_builder_append_format(StringBuilder *builder, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    int needed = vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (needed > 0) {
        size_t len = (size_t)needed;
        reserve(builder, len);
        vsnprintf(builder->data + builder->len, len + 1, format, args);
        builder->len += len;
    }
    va_end(args);
}

String str_builder_finish(StringBuilder *builder)
{
    reserve(builder, 0);
    builder->data[builder->len] = '\0';
    return (String){ .data = builder->data, .len = builder->len };
}
