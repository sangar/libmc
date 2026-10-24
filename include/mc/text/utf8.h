#pragma once

#include "mc/text/str.h"

enum { UTF8_REPLACEMENT = 0xFFFD, UTF8_MAX_BYTES = 4 };

// utf8_decode reads the code point at *position and advances past it. On
// malformed input it advances one byte, sets U+FFFD and returns false.
bool utf8_decode(String s, size_t *position, uint32_t *codepoint);
// utf8_encode writes codepoint and returns its length in bytes. A value that
// is not a Unicode scalar value, a surrogate or one past U+10FFFF, is written
// as U+FFFD.
size_t utf8_encode(uint32_t codepoint, char out[UTF8_MAX_BYTES]);
bool utf8_is_valid(String s);
// utf8_count counts the code points of valid UTF-8 by its lead bytes.
size_t utf8_count(String s);

// utf8_codepoint_width is how many terminal columns codepoint takes: 2 for
// East Asian wide and fullwidth characters and emoji, 0 for combining marks,
// zero-width characters and controls, 1 otherwise. The tables follow
// Unicode 15.1; terminals that lag behind it may differ on recent emoji.
int utf8_codepoint_width(uint32_t codepoint);

// utf8_width is the number of terminal columns s takes, with malformed bytes
// counted as U+FFFD.
size_t utf8_width(String s);
// utf8_from_latin1 converts ISO-8859-1 text, in which every byte is the code
// point of the same value, to UTF-8 in arena. Any input is valid. Windows-1252
// text differs only in bytes 0x80 to 0x9F, which become C1 control codes.
String utf8_from_latin1(Arena *arena, String latin1);
