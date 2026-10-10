#include "mc/text/utf8.h"

static bool is_continuation(unsigned char byte)
{
    return (byte & 0xC0) == 0x80;
}

bool utf8_decode(String s, size_t *position, uint32_t *codepoint)
{
    size_t i = *position;
    unsigned char lead = (unsigned char)s.data[i];
    size_t length;
    uint32_t value;
    uint32_t minimum;

    if (lead < 0x80) {
        *position = i + 1;
        *codepoint = lead;
        return true;
    } else if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        *position = i + 1;
        *codepoint = UTF8_REPLACEMENT;
        return false;
    }

    if (i + length > s.len) {
        *position = i + 1;
        *codepoint = UTF8_REPLACEMENT;
        return false;
    }
    for (size_t k = 1; k < length; k++) {
        unsigned char byte = (unsigned char)s.data[i + k];
        if (!is_continuation(byte)) {
            *position = i + 1;
            *codepoint = UTF8_REPLACEMENT;
            return false;
        }
        value = (value << 6) | (byte & 0x3F);
    }
    bool overlong = value < minimum;
    bool surrogate = value >= 0xD800 && value <= 0xDFFF;
    if (overlong || surrogate || value > 0x10FFFF) {
        *position = i + 1;
        *codepoint = UTF8_REPLACEMENT;
        return false;
    }
    *position = i + length;
    *codepoint = value;
    return true;
}

size_t utf8_encode(uint32_t codepoint, char out[UTF8_MAX_BYTES])
{
    if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        codepoint = UTF8_REPLACEMENT;
    }
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

bool utf8_is_valid(String s)
{
    size_t position = 0;
    uint32_t codepoint;
    while (position < s.len) {
        if (!utf8_decode(s, &position, &codepoint)) {
            return false;
        }
    }
    return true;
}

String utf8_from_latin1(Arena *arena, String latin1)
{
    size_t high = 0;
    for (size_t i = 0; i < latin1.len; i++) {
        high += (unsigned char)latin1.data[i] >= 0x80;
    }
    char *out = arena_push_aligned(arena, arena_size_add(arena_size_add(latin1.len, high), 1), 1);
    size_t len = 0;
    for (size_t i = 0; i < latin1.len; i++) {
        unsigned char byte = (unsigned char)latin1.data[i];
        if (byte < 0x80) {
            out[len++] = (char)byte;
        } else {
            len += utf8_encode(byte, out + len);
        }
    }
    return (String){ .data = out, .len = len };
}

size_t utf8_count(String s)
{
    size_t count = 0;
    for (size_t i = 0; i < s.len; i++) {
        count += !is_continuation((unsigned char)s.data[i]);
    }
    return count;
}
