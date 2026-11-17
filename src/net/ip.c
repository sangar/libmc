#include "mc/net/ip.h"

#include <assert.h>
#include <string.h>

enum { IPV4_LEN = 4, IPV6_LEN = 16, IPV6_GROUPS = 8, IPV4_MAX_DIGITS = 3, GROUP_MAX_DIGITS = 4 };

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool parse_ipv4(String text, uint8_t bytes[IPV4_LEN])
{
    size_t at = 0;
    for (size_t part = 0; part < IPV4_LEN; part++) {
        if (part > 0) {
            if (at == text.len || text.data[at] != '.') {
                return false;
            }
            at++;
        }
        size_t start = at;
        unsigned value = 0;
        while (at < text.len && at - start < IPV4_MAX_DIGITS && is_digit(text.data[at])) {
            value = value * 10 + (unsigned)(text.data[at] - '0');
            at++;
        }
        size_t digits = at - start;
        if (digits == 0 || value > UINT8_MAX || (digits > 1 && text.data[start] == '0')) {
            return false;
        }
        bytes[part] = (uint8_t)value;
    }
    return at == text.len;
}

static bool parse_group(String text, unsigned *value)
{
    if (text.len == 0 || text.len > GROUP_MAX_DIGITS) {
        return false;
    }
    *value = 0;
    for (size_t i = 0; i < text.len; i++) {
        char c = text.data[i];
        unsigned digit;
        if (is_digit(c)) {
            digit = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
        *value = *value * 16 + digit;
    }
    return true;
}

// parse_groups reads text, groups separated by ':', into bytes, *len of them.
// Only the address's last group, when ends_address, may be a dotted IPv4
// address, which fills two groups.
static bool parse_groups(String text, bool ends_address, uint8_t bytes[IPV6_LEN], size_t *len)
{
    *len = 0;
    if (text.len == 0) {
        return true;
    }
    bool more = true;
    while (more) {
        String group;
        more = str_cut(text, ':', &group, &text);
        if (!more && ends_address && str_contains(group, S("."))) {
            if (*len + IPV4_LEN > IPV6_LEN || !parse_ipv4(group, bytes + *len)) {
                return false;
            }
            *len += IPV4_LEN;
            return true;
        }
        unsigned value;
        if (*len + 2 > IPV6_LEN || !parse_group(group, &value)) {
            return false;
        }
        bytes[(*len)++] = (uint8_t)(value >> 8);
        bytes[(*len)++] = (uint8_t)value;
    }
    return true;
}

static bool parse_ipv6(String text, uint8_t bytes[IPV6_LEN])
{
    size_t gap;
    size_t len;
    if (!str_find(text, S("::"), &gap)) {
        return parse_groups(text, true, bytes, &len) && len == IPV6_LEN;
    }
    uint8_t head[IPV6_LEN];
    uint8_t tail[IPV6_LEN];
    size_t head_len;
    size_t tail_len;
    if (!parse_groups(str_slice(text, 0, gap), false, head, &head_len) ||
        !parse_groups(str_slice(text, gap + 2, text.len), true, tail, &tail_len) ||
        head_len + tail_len > IPV6_LEN - 2) {
        return false;
    }
    memset(bytes, 0, IPV6_LEN);
    memcpy(bytes, head, head_len);
    memcpy(bytes + IPV6_LEN - tail_len, tail, tail_len);
    return true;
}

bool ip_parse(String text, IpAddress *address)
{
    *address = (IpAddress){ 0 };
    if (str_contains(text, S(":"))) {
        address->family = IP_V6;
        return parse_ipv6(text, address->bytes);
    }
    address->family = IP_V4;
    return parse_ipv4(text, address->bytes);
}

static void append_groups(StringBuilder *text, const unsigned *groups, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            str_builder_append_char(text, ':');
        }
        str_builder_append_format(text, "%x", groups[i]);
    }
}

static String format_ipv6(Arena *arena, const uint8_t bytes[IPV6_LEN])
{
    static const uint8_t mapped_prefix[12] = { [10] = 0xff, [11] = 0xff };
    if (memcmp(bytes, mapped_prefix, sizeof mapped_prefix) == 0) {
        return str_format(arena, "::ffff:%d.%d.%d.%d", bytes[12], bytes[13], bytes[14], bytes[15]);
    }
    unsigned groups[IPV6_GROUPS];
    for (size_t i = 0; i < IPV6_GROUPS; i++) {
        groups[i] = (unsigned)bytes[i * 2] << 8 | bytes[i * 2 + 1];
    }
    size_t gap = 0;
    size_t gap_len = 0;
    for (size_t i = 0; i < IPV6_GROUPS; i++) {
        size_t run = 0;
        while (i + run < IPV6_GROUPS && groups[i + run] == 0) {
            run++;
        }
        if (run >= 2 && run > gap_len) {
            gap = i;
            gap_len = run;
        }
        i += run;
    }
    StringBuilder text = str_builder_create(arena, 40);
    if (gap_len == 0) {
        append_groups(&text, groups, IPV6_GROUPS);
        return str_builder_finish(&text);
    }
    append_groups(&text, groups, gap);
    str_builder_append(&text, S("::"));
    append_groups(&text, groups + gap + gap_len, IPV6_GROUPS - gap - gap_len);
    return str_builder_finish(&text);
}

String ip_format(Arena *arena, IpAddress address)
{
    if (address.family == IP_V4) {
        return str_format(arena, "%d.%d.%d.%d", address.bytes[0], address.bytes[1], address.bytes[2],
                          address.bytes[3]);
    }
    assert(address.family == IP_V6);
    return format_ipv6(arena, address.bytes);
}

bool ip_in_network(IpAddress address, IpAddress network, unsigned prefix_bits)
{
    assert(prefix_bits <= (network.family == IP_V4 ? IPV4_LEN : IPV6_LEN) * 8u);
    if (address.family != network.family) {
        return false;
    }
    size_t whole_bytes = prefix_bits / 8;
    unsigned rest_bits = prefix_bits % 8;
    if (memcmp(address.bytes, network.bytes, whole_bytes) != 0) {
        return false;
    }
    if (rest_bits == 0) {
        return true;
    }
    uint8_t mask = (uint8_t)(0xff << (8 - rest_bits));
    return ((address.bytes[whole_bytes] ^ network.bytes[whole_bytes]) & mask) == 0;
}
