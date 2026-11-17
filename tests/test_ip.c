#include "test.h"

#include <string.h>

#include "mc/net/ip.h"

typedef struct IpFormatCase {
    const char *text;
    IpFamily family;
    const char *formatted;
} IpFormatCase;

static void ip_parses_and_formats_addresses(Test *test)
{
    static const IpFormatCase cases[] = {
        { "192.0.2.1", IP_V4, "192.0.2.1" },
        { "0.0.0.0", IP_V4, "0.0.0.0" },
        { "255.255.255.255", IP_V4, "255.255.255.255" },
        { "::", IP_V6, "::" },
        { "::1", IP_V6, "::1" },
        { "1::", IP_V6, "1::" },
        // RFC 5952 4.1, 4.2.1, 4.2.2, 4.2.3 and 4.3.
        { "2001:0db8::0001", IP_V6, "2001:db8::1" },
        { "2001:db8:0:0:0:0:2:1", IP_V6, "2001:db8::2:1" },
        { "2001:db8:0:1:1:1:1:1", IP_V6, "2001:db8:0:1:1:1:1:1" },
        { "2001:0:0:1:0:0:0:1", IP_V6, "2001:0:0:1::1" },
        { "2001:db8:0:0:1:0:0:1", IP_V6, "2001:db8::1:0:0:1" },
        { "2001:DB8::AbCd", IP_V6, "2001:db8::abcd" },
        { "1:2:3:4:5:6:7::", IP_V6, "1:2:3:4:5:6:7:0" },
        { "::2:3:4:5:6:7:8", IP_V6, "0:2:3:4:5:6:7:8" },
        { "1:2:3:4:5:6:1.2.3.4", IP_V6, "1:2:3:4:5:6:102:304" },
        { "64:ff9b::192.0.2.33", IP_V6, "64:ff9b::c000:221" },
        // IPv4-mapped addresses, RFC 5952 5.
        { "::ffff:192.0.2.1", IP_V6, "::ffff:192.0.2.1" },
        { "::FFFF:c000:0201", IP_V6, "::ffff:192.0.2.1" },
        { "0:0:0:0:0:ffff:192.0.2.1", IP_V6, "::ffff:192.0.2.1" },
    };
    for (size_t i = 0; i < countof(cases); i++) {
        IpAddress address;
        bool parsed = ip_parse(S(cases[i].text), &address);
        test_check(test, parsed && address.family == cases[i].family, cases[i].text);
        if (parsed) {
            test_check_str(test, ip_format(test->arena, address), S(cases[i].formatted), cases[i].text);
        }
    }
    IpAddress mapped;
    static const uint8_t mapped_bytes[16] = { [10] = 0xff, [11] = 0xff, [12] = 192, [13] = 0, [14] = 2, [15] = 1 };
    test_check(test, ip_parse(S("::ffff:192.0.2.1"), &mapped) && memcmp(mapped.bytes, mapped_bytes, 16) == 0,
               "a mapped address's bytes");
    IpAddress v4;
    test_check(test, ip_parse(S("192.0.2.1"), &v4) && v4.bytes[0] == 192 && v4.bytes[3] == 1,
               "an IPv4 address's bytes in network order");
}

static void ip_refuses_other_forms(Test *test)
{
    static const char *const refused[] = {
        "",
        "01.2.3.4",
        "00.1.2.3",
        "1.2.3",
        "1.2.3.4.",
        "1.2.3.4.5",
        ".1.2.3.4",
        "1..2.3",
        "256.1.2.3",
        "1234.1.1.1",
        "0x1.2.3.4",
        "1.2.3.0x4",
        "1.2.3.-4",
        " 1.2.3.4",
        "1.2.3.4 ",
        ":",
        ":::",
        "1:::2",
        "1::2::3",
        ":1::2",
        "1::2:",
        "1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:9",
        "1:2:3:4:5:6:7:8::",
        "::1:2:3:4:5:6:7:8",
        "1:2:3:4:5:6:7:1.2.3.4",
        "12345::",
        "g::",
        "fe80::1%eth0",
        "[::1]",
        "::1/128",
        "2001:db8::/32",
        "::ffff:1.2.3",
        "::ffff:01.2.3.4",
        "::ffff:256.1.2.3",
        "::1.2.3.4:5",
        "1.2.3.4::",
    };
    for (size_t i = 0; i < countof(refused); i++) {
        IpAddress address;
        test_check(test, !ip_parse(S(refused[i]), &address), refused[i]);
    }
    IpAddress address;
    test_check(test, !ip_parse((String){ "1.2.3.4\0", 8 }, &address), "a NUL after an address");
}

static bool ip_test_in_network(Test *test, const char *address, const char *network, unsigned prefix_bits)
{
    IpAddress parsed_address;
    IpAddress parsed_network;
    bool parsed = ip_parse(S(address), &parsed_address) && ip_parse(S(network), &parsed_network);
    test_check(test, parsed, address);
    return parsed && ip_in_network(parsed_address, parsed_network, prefix_bits);
}

static void ip_matches_networks_by_prefix(Test *test)
{
    test_check(test, ip_test_in_network(test, "203.0.113.9", "10.0.0.0", 0), "/0 matches every IPv4 address");
    test_check(test, ip_test_in_network(test, "2001:db8::1", "::", 0), "/0 matches every IPv6 address");
    test_check(test, ip_test_in_network(test, "192.0.2.77", "192.0.2.0", 24), "inside a /24");
    test_check(test, !ip_test_in_network(test, "192.0.3.77", "192.0.2.0", 24), "outside a /24");
    test_check(test, ip_test_in_network(test, "192.0.2.4", "192.0.2.4", 31), "the even half of a /31");
    test_check(test, ip_test_in_network(test, "192.0.2.5", "192.0.2.4", 31), "the odd half of a /31");
    test_check(test, !ip_test_in_network(test, "192.0.2.6", "192.0.2.4", 31), "past a /31");
    test_check(test, !ip_test_in_network(test, "192.0.2.3", "192.0.2.4", 31), "before a /31");
    test_check(test, ip_test_in_network(test, "192.0.2.4", "192.0.2.4", 32), "a /32 is one address");
    test_check(test, !ip_test_in_network(test, "192.0.2.5", "192.0.2.4", 32), "a /32 is only that address");
    test_check(test, ip_test_in_network(test, "2001:db8:1::1", "2001:db8::", 32), "inside an IPv6 /32");
    test_check(test, !ip_test_in_network(test, "2001:db9::1", "2001:db8::", 32), "outside an IPv6 /32");
    test_check(test, ip_test_in_network(test, "2001:db8::1", "2001:db8::1", 128), "a /128 is one address");
    test_check(test, !ip_test_in_network(test, "2001:db8::2", "2001:db8::1", 128), "a /128 is only that address");
    test_check(test, !ip_test_in_network(test, "192.0.2.1", "::", 0), "IPv4 is never in an IPv6 network");
    test_check(test, !ip_test_in_network(test, "::ffff:192.0.2.1", "192.0.2.0", 24),
               "a mapped address is not in an IPv4 network");
}

const TestCase IP_TESTS[] = {
    { "ip_parses_and_formats_addresses", ip_parses_and_formats_addresses },
    { "ip_refuses_other_forms", ip_refuses_other_forms },
    { "ip_matches_networks_by_prefix", ip_matches_networks_by_prefix },
    { nullptr, nullptr },
};
