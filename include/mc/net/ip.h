#pragma once

#include <stdint.h>

#include "mc/core/arena.h"
#include "mc/text/str.h"

// IP addresses read from and written as text, and matched against networks,
// as SPF's ip4 and ip6 mechanisms match a client (RFC 7208 5.6).

typedef enum IpFamily { IP_V4 = 4, IP_V6 = 6 } IpFamily;

// IpAddress is an address in network byte order: the first 4 bytes for IPv4,
// all 16 for IPv6.
typedef struct IpAddress {
    IpFamily family;
    uint8_t bytes[16];
} IpAddress;

// ip_parse reads text into *address and reports whether it is an address.
// IPv4 is four dotted decimal parts from 0 to 255 without leading zeros,
// which other parsers read as octal. IPv6 is written as RFC 4291 2.2 allows:
// up to eight groups of one to four hex digits in either case, at most one
// "::", and optionally a dotted IPv4 address last. Anything else, such as a
// zone, brackets or a prefix length, is refused, and *address then holds
// nothing meaningful. An IPv4-mapped address such as ::ffff:192.0.2.1 is
// IP_V6.
bool ip_parse(String text, IpAddress *address);

// ip_format writes address in the arena: IPv4 in dotted decimal, and IPv6 as
// RFC 5952 4 recommends, in lowercase without leading zeros, with the longest
// run of two or more zero groups, the first of equal runs, written as "::",
// and an IPv4-mapped address as ::ffff:192.0.2.1 (RFC 5952 5).
String ip_format(Arena *arena, IpAddress address);

// ip_in_network reports whether the first prefix_bits bits of address are
// those of network. Addresses of different families never match, and a
// prefix of 0 matches every address of network's family. prefix_bits must be
// at most 32 for an IPv4 network and 128 for an IPv6 one.
bool ip_in_network(IpAddress address, IpAddress network, unsigned prefix_bits);
