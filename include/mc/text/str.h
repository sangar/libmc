#pragma once

#include <stdarg.h>

#include "mc/core/arena.h"
#include "mc/core/base.h"

// String is a view of bytes: a pointer and a length. It owns nothing and is
// not NUL-terminated, except where a function that allocates it in an arena
// says so. Every function here that takes an Arena returns a NUL-terminated
// String in it.
typedef struct String {
    const char *data;
    size_t len;
} String;

// StringList is an arena-grown array of String views; it owns the array, not the bytes.
typedef struct StringList {
    String *items;
    size_t count;
    size_t capacity;
} StringList;

// S views a NUL-terminated C string; nullptr views the empty string.
String S(const char *cstr);

bool str_equal(String a, String b);
// str_equal_ignore_case folds ASCII letters only.
bool str_equal_ignore_case(String a, String b);
// str_compare orders bytewise, a shorter prefix first, like strcmp.
int str_compare(String a, String b);
// str_compare_ignore_case orders as str_compare would after folding ASCII
// letters to lower case, so "Readme" sorts between "rat" and "rest".
int str_compare_ignore_case(String a, String b);
bool str_starts_with(String s, String prefix);
bool str_ends_with(String s, String suffix);
bool str_contains(String s, String needle);
bool str_contains_any(String s, String chars);
size_t str_count_char(String s, char c);

// The find functions store the byte offset of the first (or last) match.
bool str_find(String s, String needle, size_t *index);
bool str_find_char(String s, char c, size_t *index);
bool str_find_last_char(String s, char c, size_t *index);

// str_slice clamps start and end to the string.
String str_slice(String s, size_t start, size_t end);
String str_trim(String s);
String str_trim_prefix(String s, String prefix);
String str_trim_suffix(String s, String suffix);
String str_first_line(String s);
// str_cut splits around the first separator; without one, head is s and tail is empty.
bool str_cut(String s, char separator, String *head, String *tail);

bool str_parse_u64(String s, uint64_t *value);
// str_parse_i64 accepts an optional leading sign.
bool str_parse_i64(String s, int64_t *value);
bool str_is_digits(String s);

String str_copy(Arena *arena, String s);
const char *str_cstr(Arena *arena, String s);
[[gnu::format(printf, 2, 3)]] String str_format(Arena *arena, const char *format, ...);
String str_format_va(Arena *arena, const char *format, va_list args);
String str_concat(Arena *arena, String a, String b);
String str_repeat(Arena *arena, String s, size_t times);
String str_replace_all(Arena *arena, String s, String old, String replacement);
String str_lower_ascii(Arena *arena, String s);
String str_upper_ascii(Arena *arena, String s);
// str_quote writes s as a double-quoted literal with C escapes.
String str_quote(Arena *arena, String s);
// str_percent_encode encodes everything but RFC 3986 unreserved characters,
// and '/' too unless keep_slash.
String str_percent_encode(Arena *arena, String s, bool keep_slash);

// The parts of str_split and str_fields are views into s.
StringList str_split(Arena *arena, String s, char separator);
// str_fields splits around runs of ASCII whitespace.
StringList str_fields(Arena *arena, String s);
String str_join(Arena *arena, StringList list, String separator);

void strlist_push(Arena *arena, StringList *list, String s);
void strlist_push_all(Arena *arena, StringList *list, StringList more);
StringList strlist_copy(Arena *arena, StringList list);
StringList strlist_of(Arena *arena, size_t count, const char *const *items);
// strlist_from_lines splits on '\n'; an empty string gives an empty list.
StringList strlist_from_lines(Arena *arena, String s);
bool strlist_contains(StringList list, String s);
void strlist_sort(StringList *list);

// StringBuilder appends into arena memory, growing by doubling.
typedef struct StringBuilder {
    Arena *arena;
    char *data;
    size_t len;
    size_t capacity;
} StringBuilder;

StringBuilder str_builder_create(Arena *arena, size_t capacity);
void str_builder_append(StringBuilder *builder, String s);
void str_builder_append_char(StringBuilder *builder, char c);
[[gnu::format(printf, 2, 3)]] void str_builder_append_format(StringBuilder *builder, const char *format, ...);
// str_builder_finish returns the built text, NUL-terminated. The builder may
// keep appending: an earlier result keeps its bytes and length, but a later
// append can overwrite its terminator.
String str_builder_finish(StringBuilder *builder);
