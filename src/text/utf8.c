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

typedef struct {
    uint32_t first;
    uint32_t last;
} CodepointRange;

// Wide (W) and fullwidth (F) code points of EastAsianWidth.txt, Unicode 15.1.
static const CodepointRange WIDE[] = {
    { 0x1100, 0x115F },   { 0x231A, 0x231B },   { 0x2329, 0x232A },   { 0x23E9, 0x23EC },   { 0x23F0, 0x23F0 },
    { 0x23F3, 0x23F3 },   { 0x25FD, 0x25FE },   { 0x2614, 0x2615 },   { 0x2648, 0x2653 },   { 0x267F, 0x267F },
    { 0x2693, 0x2693 },   { 0x26A1, 0x26A1 },   { 0x26AA, 0x26AB },   { 0x26BD, 0x26BE },   { 0x26C4, 0x26C5 },
    { 0x26CE, 0x26CE },   { 0x26D4, 0x26D4 },   { 0x26EA, 0x26EA },   { 0x26F2, 0x26F3 },   { 0x26F5, 0x26F5 },
    { 0x26FA, 0x26FA },   { 0x26FD, 0x26FD },   { 0x2705, 0x2705 },   { 0x270A, 0x270B },   { 0x2728, 0x2728 },
    { 0x274C, 0x274C },   { 0x274E, 0x274E },   { 0x2753, 0x2755 },   { 0x2757, 0x2757 },   { 0x2795, 0x2797 },
    { 0x27B0, 0x27B0 },   { 0x27BF, 0x27BF },   { 0x2B1B, 0x2B1C },   { 0x2B50, 0x2B50 },   { 0x2B55, 0x2B55 },
    { 0x2E80, 0x2E99 },   { 0x2E9B, 0x2EF3 },   { 0x2F00, 0x2FD5 },   { 0x2FF0, 0x2FFF },   { 0x3000, 0x303E },
    { 0x3041, 0x3096 },   { 0x3099, 0x30FF },   { 0x3105, 0x312F },   { 0x3131, 0x318E },   { 0x3190, 0x31E3 },
    { 0x31EF, 0x321E },   { 0x3220, 0x3247 },   { 0x3250, 0x4DBF },   { 0x4E00, 0xA48C },   { 0xA490, 0xA4C6 },
    { 0xA960, 0xA97C },   { 0xAC00, 0xD7A3 },   { 0xF900, 0xFAFF },   { 0xFE10, 0xFE19 },   { 0xFE30, 0xFE52 },
    { 0xFE54, 0xFE66 },   { 0xFE68, 0xFE6B },   { 0xFF01, 0xFF60 },   { 0xFFE0, 0xFFE6 },   { 0x16FE0, 0x16FE4 },
    { 0x16FF0, 0x16FF1 }, { 0x17000, 0x187F7 }, { 0x18800, 0x18CD5 }, { 0x18D00, 0x18D08 }, { 0x1AFF0, 0x1AFFE },
    { 0x1B000, 0x1B122 }, { 0x1B132, 0x1B132 }, { 0x1B150, 0x1B152 }, { 0x1B155, 0x1B155 }, { 0x1B164, 0x1B167 },
    { 0x1B170, 0x1B2FB }, { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF }, { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A },
    { 0x1F200, 0x1F202 }, { 0x1F210, 0x1F23B }, { 0x1F240, 0x1F248 }, { 0x1F250, 0x1F251 }, { 0x1F260, 0x1F265 },
    { 0x1F300, 0x1F320 }, { 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 }, { 0x1F3A0, 0x1F3CA },
    { 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 }, { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 },
    { 0x1F442, 0x1F4FC }, { 0x1F4FF, 0x1F53D }, { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A },
    { 0x1F595, 0x1F596 }, { 0x1F5A4, 0x1F5A4 }, { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC },
    { 0x1F6D0, 0x1F6D2 }, { 0x1F6D5, 0x1F6D7 }, { 0x1F6DC, 0x1F6DF }, { 0x1F6EB, 0x1F6EC }, { 0x1F6F4, 0x1F6FC },
    { 0x1F7E0, 0x1F7EB }, { 0x1F7F0, 0x1F7F0 }, { 0x1F90C, 0x1F93A }, { 0x1F93C, 0x1F945 }, { 0x1F947, 0x1F9FF },
    { 0x1FA70, 0x1FA7C }, { 0x1FA80, 0x1FA88 }, { 0x1FA90, 0x1FABD }, { 0x1FABF, 0x1FAC5 }, { 0x1FACE, 0x1FADB },
    { 0x1FAE0, 0x1FAE8 }, { 0x1FAF0, 0x1FAF8 }, { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
};

// Combining marks, format characters and variation selectors in the blocks
// that file names and user text commonly use. Decomposed text, which macOS
// file names often are, puts accents in the first range and Japanese voicing
// marks at U+3099 and U+309A.
static const CodepointRange ZERO_WIDTH[] = {
    { 0x0300, 0x036F },   { 0x0483, 0x0489 },   { 0x0591, 0x05BD },   { 0x05BF, 0x05BF },   { 0x05C1, 0x05C2 },
    { 0x05C4, 0x05C5 },   { 0x05C7, 0x05C7 },   { 0x0610, 0x061A },   { 0x061C, 0x061C },   { 0x064B, 0x065F },
    { 0x0670, 0x0670 },   { 0x06D6, 0x06DC },   { 0x06DF, 0x06E4 },   { 0x06E7, 0x06E8 },   { 0x06EA, 0x06ED },
    { 0x0900, 0x0902 },   { 0x093A, 0x093A },   { 0x093C, 0x093C },   { 0x0941, 0x0948 },   { 0x094D, 0x094D },
    { 0x0951, 0x0957 },   { 0x0962, 0x0963 },   { 0x0E31, 0x0E31 },   { 0x0E34, 0x0E3A },   { 0x0E47, 0x0E4E },
    { 0x1160, 0x11FF },   { 0x1AB0, 0x1AFF },   { 0x1DC0, 0x1DFF },   { 0x200B, 0x200F },   { 0x202A, 0x202E },
    { 0x2060, 0x2064 },   { 0x20D0, 0x20F0 },   { 0x302A, 0x302D },   { 0x3099, 0x309A },   { 0xFE00, 0xFE0F },
    { 0xFE20, 0xFE2F },   { 0xFEFF, 0xFEFF },   { 0xE0001, 0xE0001 }, { 0xE0020, 0xE007F }, { 0xE0100, 0xE01EF },
};

static bool in_ranges(const CodepointRange *ranges, size_t count, uint32_t codepoint)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (codepoint < ranges[middle].first) {
            high = middle;
        } else if (codepoint > ranges[middle].last) {
            low = middle + 1;
        } else {
            return true;
        }
    }
    return false;
}

int utf8_codepoint_width(uint32_t codepoint)
{
    if (codepoint >= 0x20 && codepoint < 0x7F) {
        return 1;
    }
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0)) {
        return 0;
    }
    if (in_ranges(ZERO_WIDTH, countof(ZERO_WIDTH), codepoint)) {
        return 0;
    }
    return in_ranges(WIDE, countof(WIDE), codepoint) ? 2 : 1;
}

size_t utf8_width(String s)
{
    size_t width = 0;
    size_t position = 0;
    uint32_t codepoint;
    while (position < s.len) {
        utf8_decode(s, &position, &codepoint);
        width += (size_t)utf8_codepoint_width(codepoint);
    }
    return width;
}
