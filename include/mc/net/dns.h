#pragma once

#include <stdint.h>

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

// DNS lookups of the records that checks on a mail's sender read, such as
// SPF, DKIM and DMARC: c-ares, pinned in deps.lock. Replies that do not match
// the query, in id, server, or the random case of the name (DNS 0x20), are
// dropped, and a reply too long for UDP is asked for again over TCP.

typedef enum DnsType {
    DNS_A = 1,
    DNS_PTR = 12,
    DNS_MX = 15,
    DNS_TXT = 16,
    DNS_AAAA = 28,
} DnsType;

// DnsRecord is one answer of the type asked for.
typedef struct DnsRecord {
    // data is an A or AAAA record's address as text, the name a PTR or MX
    // record points to, or a TXT record's strings joined, as SPF and DKIM
    // read them.
    String data;
    // preference orders MX records, lowest first; 0 for other types.
    uint16_t preference;
    uint32_t ttl;
} DnsRecord;

typedef struct DnsRecords {
    DnsRecord *items;
    size_t count;
} DnsRecords;

// DnsResolverConfig says where to ask. Zero values take the default named
// beside each field.
typedef struct DnsResolverConfig {
    // servers is a comma-separated list of host:port, such as
    // 127.0.0.1:5353; empty asks the system's servers.
    String servers;
    // timeout_ms bounds each of the 3 tries a server gets: 2 s.
    int64_t timeout_ms;
} DnsResolverConfig;

// DnsResolver runs one thread that sends every query and reads every reply,
// and keeps answers for their TTL, up to an hour. Many threads may look up
// through one resolver at once.
typedef struct DnsResolver DnsResolver;

[[nodiscard]] Error dns_resolver_create(const DnsResolverConfig *config, DnsResolver **resolver, Err *err);
// dns_resolver_destroy stops the resolver's thread; no lookup may be in flight.
void dns_resolver_destroy(DnsResolver *resolver);

// dns_lookup asks for name's records of type, copied into arena. A name that
// exists without records of that type is ERR_OK with none. A name that does
// not exist is ERR_NOT_FOUND, no answer from any server ERR_TIMEOUT, and a
// server's failure, such as SERVFAIL, ERR_NETWORK.
[[nodiscard]] Error dns_lookup(DnsResolver *resolver, Arena *arena, String name, DnsType type, DnsRecords *records,
                               Err *err);
