#include "mc/encoding/base64.h"

static const char STANDARD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

String base64_encode(Arena *arena, String data, Base64Alphabet alphabet)
{
    const char *digits = alphabet == BASE64_URL ? URL : STANDARD;
    StringBuilder text = str_builder_create(arena, (data.len + 2) / 3 * 4 + 1);
    const unsigned char *bytes = (const unsigned char *)data.data;
    for (size_t i = 0; i < data.len; i += 3) {
        size_t count = data.len - i < 3 ? data.len - i : 3;
        uint32_t group = (uint32_t)bytes[i] << 16;
        group |= count > 1 ? (uint32_t)bytes[i + 1] << 8 : 0;
        group |= count > 2 ? (uint32_t)bytes[i + 2] : 0;
        for (size_t j = 0; j < 4; j++) {
            if (j <= count) {
                str_builder_append_char(&text, digits[(group >> (18 - 6 * j)) & 0x3f]);
            } else if (alphabet == BASE64_STANDARD) {
                str_builder_append_char(&text, '=');
            }
        }
    }
    return str_builder_finish(&text);
}

// digit_value is c's value in digits, or -1 when it is not one of them.
static int digit_value(const char *digits, char c)
{
    for (int i = 0; i < 64; i++) {
        if (digits[i] == c) {
            return i;
        }
    }
    return -1;
}

bool base64_decode(Arena *arena, String text, Base64Alphabet alphabet, String *data)
{
    const char *digits = alphabet == BASE64_URL ? URL : STANDARD;
    size_t len = text.len;
    if (alphabet == BASE64_STANDARD) {
        if (len % 4 != 0) {
            return false;
        }
        size_t padding = 0;
        while (padding < 2 && padding < len && text.data[len - 1 - padding] == '=') {
            padding++;
        }
        len -= padding;
    }
    if (len % 4 == 1) {
        return false;
    }
    unsigned char *bytes = arena_push(arena, len / 4 * 3 + 2);
    size_t count = 0;
    uint32_t bits = 0;
    unsigned bit_count = 0;
    for (size_t i = 0; i < len; i++) {
        int value = digit_value(digits, text.data[i]);
        if (value < 0) {
            return false;
        }
        bits = (bits << 6) | (uint32_t)value;
        bit_count += 6;
        if (bit_count >= 8) {
            bit_count -= 8;
            bytes[count++] = (unsigned char)(bits >> bit_count);
            bits &= (1u << bit_count) - 1;
        }
    }
    if (bits != 0) {
        return false;
    }
    *data = (String){ (const char *)bytes, count };
    return true;
}
