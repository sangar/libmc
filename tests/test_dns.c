#include "test.h"

#include <stdatomic.h>

#include "mc/net/dns.h"
#include "mc/platform/platform.h"

enum { DNS_POLL_MS = 20, DNS_HEADER_BYTES = 12, DNS_UDP_LIMIT = 512, DNS_RCODE_SERVFAIL = 2, DNS_RCODE_NXDOMAIN = 3 };

// DNS_SERVFAIL fails on every query. DNS_LOWERCASED comes back with its
// question in lowercase, as an off-path forger who cannot guess the random
// case of DNS 0x20 would send it; it has enough letters that the random case
// is never all lowercase.
static const char DNS_SERVFAIL[] = "servfail.example";
static const char DNS_LOWERCASED[] = "the-question-of-this-name.comes-back-lowercased.so-it-never-matches.example";

typedef struct DnsZoneRecord {
    const char *name;
    DnsType type;
    String rdata;
} DnsZoneRecord;

// DnsServer is an authoritative server for records, on UDP and on TCP at the
// same port. Over UDP it truncates replies longer than 512 bytes, so those
// are asked for again over TCP.
typedef struct DnsServer {
    const DnsZoneRecord *records;
    size_t record_count;
    int udp;
    int tcp;
    String address;
    atomic_bool stopping;
    atomic_size_t tcp_queries;
    Thread udp_thread;
    Thread tcp_thread;
} DnsServer;

static String dns_wire_name(Arena *arena, const char *name)
{
    StringBuilder wire = str_builder_create(arena, 64);
    StringList labels = str_split(arena, S(name), '.');
    for (size_t i = 0; i < labels.count; i++) {
        str_builder_append_char(&wire, (char)labels.items[i].len);
        str_builder_append(&wire, labels.items[i]);
    }
    str_builder_append_char(&wire, '\0');
    return str_builder_finish(&wire);
}

static String dns_mx_rdata(Arena *arena, uint16_t preference, const char *exchange)
{
    char bytes[] = { (char)(preference >> 8), (char)(preference & 0xFF) };
    return str_concat(arena, (String){ bytes, 2 }, dns_wire_name(arena, exchange));
}

// dns_txt_rdata is text in strings of at most 255 bytes, each after its length.
static String dns_txt_rdata(Arena *arena, String text)
{
    StringBuilder rdata = str_builder_create(arena, text.len + text.len / 255 + 1);
    for (size_t at = 0; at < text.len; at += 255) {
        String piece = str_slice(text, at, at + 255 < text.len ? at + 255 : text.len);
        str_builder_append_char(&rdata, (char)piece.len);
        str_builder_append(&rdata, piece);
    }
    return str_builder_finish(&rdata);
}

static void dns_append_u16(StringBuilder *builder, size_t value)
{
    str_builder_append_char(builder, (char)((value >> 8) & 0xFF));
    str_builder_append_char(builder, (char)(value & 0xFF));
}

// dns_question_name reads the question's name in lowercase, and where the
// question's type follows it.
static bool dns_question_name(Arena *arena, String query, String *name, size_t *end)
{
    StringBuilder text = str_builder_create(arena, 64);
    size_t at = DNS_HEADER_BYTES;
    while (at < query.len && query.data[at] != '\0') {
        size_t len = (unsigned char)query.data[at];
        if (len > 63 || at + 1 + len >= query.len) {
            return false;
        }
        if (text.len > 0) {
            str_builder_append_char(&text, '.');
        }
        // DNS names match ignoring the case of ASCII letters alone, RFC 4343.
        str_builder_append(&text, str_lower_ascii(arena, str_slice(query, at + 1, at + 1 + len))); // modern-c: allow ascii-text
        at += 1 + len;
    }
    *name = str_builder_finish(&text);
    *end = at + 1;
    return at + 5 <= query.len;
}

// dns_answer is the server's reply to query, empty for one it cannot read.
static String dns_answer(DnsServer *server, Arena *arena, String query, bool udp)
{
    String name = { 0 };
    size_t question_end = 0;
    if (query.len < DNS_HEADER_BYTES || query.data[4] != 0 || query.data[5] != 1 ||
        !dns_question_name(arena, query, &name, &question_end)) {
        return (String){ 0 };
    }
    size_t type = ((size_t)(unsigned char)query.data[question_end] << 8) | (unsigned char)query.data[question_end + 1];
    String question = str_slice(query, DNS_HEADER_BYTES, question_end + 4);
    if (str_equal(name, S(DNS_LOWERCASED))) {
        question = str_lower_ascii(arena, question); // modern-c: allow ascii-text
    }
    bool exists = false;
    StringBuilder answers = str_builder_create(arena, 512);
    size_t answer_count = 0;
    for (size_t i = 0; i < server->record_count; i++) {
        const DnsZoneRecord *record = &server->records[i];
        if (!str_equal(name, S(record->name))) {
            continue;
        }
        exists = true;
        if ((size_t)record->type == type) {
            // A pointer to the question's name, class IN, a TTL of 300 s.
            str_builder_append(&answers, (String){ "\xC0\x0C", 2 });
            dns_append_u16(&answers, type);
            str_builder_append(&answers, (String){ "\x00\x01\x00\x00\x01\x2C", 6 });
            dns_append_u16(&answers, record->rdata.len);
            str_builder_append(&answers, record->rdata);
            answer_count++;
        }
    }
    int rcode = str_equal(name, S(DNS_SERVFAIL)) ? DNS_RCODE_SERVFAIL : exists ? 0 : DNS_RCODE_NXDOMAIN;
    bool truncated = udp && DNS_HEADER_BYTES + question.len + answers.len > DNS_UDP_LIMIT;
    StringBuilder reply = str_builder_create(arena, DNS_HEADER_BYTES + question.len + answers.len);
    str_builder_append(&reply, str_slice(query, 0, 2));
    // QR and AA, with RD as asked; RA and the rcode.
    str_builder_append_char(&reply, (char)(0x84 | (truncated ? 0x02 : 0) | (query.data[2] & 0x01)));
    str_builder_append_char(&reply, (char)(0x80 | rcode));
    dns_append_u16(&reply, 1);
    dns_append_u16(&reply, truncated ? 0 : answer_count);
    dns_append_u16(&reply, 0);
    dns_append_u16(&reply, 0);
    str_builder_append(&reply, question);
    if (!truncated) {
        str_builder_append(&reply, str_builder_finish(&answers));
    }
    return str_builder_finish(&reply);
}

static void *dns_serve_udp(void *argument)
{
    DnsServer *server = argument;
    char buffer[1500];
    while (!atomic_load(&server->stopping)) {
        size_t got = 0;
        NetPeer peer = { 0 };
        if (net_receive_from(server->udp, DNS_POLL_MS, buffer, sizeof buffer, &got, &peer, nullptr) != ERR_OK) {
            continue;
        }
        Arena *arena = arena_create(0);
        String reply = dns_answer(server, arena, (String){ buffer, got }, true);
        if (reply.len > 0) {
            unused(net_send_to(server->udp, reply, &peer, nullptr));
        }
        arena_destroy(arena);
    }
    return nullptr;
}

static bool dns_receive_all(int fd, char *buffer, size_t len)
{
    for (size_t at = 0, got = 0; at < len; at += got) {
        if (net_receive(fd, buffer + at, len - at, &got, nullptr) != ERR_OK || got == 0) {
            return false;
        }
    }
    return true;
}

// dns_serve_connection answers each query on a TCP connection, which comes
// and goes after its two-byte length, until the client closes it.
static void dns_serve_connection(DnsServer *server, int client)
{
    char length[2];
    char query[UINT16_MAX];
    while (dns_receive_all(client, length, 2)) {
        size_t len = ((size_t)(unsigned char)length[0] << 8) | (unsigned char)length[1];
        if (!dns_receive_all(client, query, len)) {
            return;
        }
        atomic_fetch_add(&server->tcp_queries, 1);
        Arena *arena = arena_create(0);
        String reply = dns_answer(server, arena, (String){ query, len }, false);
        StringBuilder framed = str_builder_create(arena, reply.len + 2);
        dns_append_u16(&framed, reply.len);
        str_builder_append(&framed, reply);
        bool sent = reply.len > 0 && net_send(client, str_builder_finish(&framed), nullptr) == ERR_OK;
        arena_destroy(arena);
        if (!sent) {
            return;
        }
    }
}

static void *dns_serve_tcp(void *argument)
{
    DnsServer *server = argument;
    while (!atomic_load(&server->stopping)) {
        int client = -1;
        if (net_accept(server->tcp, DNS_POLL_MS, &client)) {
            unused(net_set_timeouts(client, 2000, nullptr));
            dns_serve_connection(server, client);
            net_close(client);
        }
    }
    return nullptr;
}

static bool dns_server_start(Test *test, DnsServer *server, const DnsZoneRecord *records, size_t record_count)
{
    *server = (DnsServer){ .records = records, .record_count = record_count, .udp = -1, .tcp = -1 };
    atomic_init(&server->stopping, false);
    atomic_init(&server->tcp_queries, 0);
    bool started = net_listen_udp(S("127.0.0.1:0"), &server->udp, nullptr) == ERR_OK &&
                   net_local_address(test->arena, server->udp, &server->address, nullptr) == ERR_OK &&
                   net_listen(server->address, &server->tcp, nullptr) == ERR_OK &&
                   thread_start(&server->udp_thread, dns_serve_udp, server, nullptr) == ERR_OK &&
                   thread_start(&server->tcp_thread, dns_serve_tcp, server, nullptr) == ERR_OK;
    test_check(test, started, "the DNS server starts");
    return started;
}

static void dns_server_stop(DnsServer *server)
{
    atomic_store(&server->stopping, true);
    thread_join(&server->udp_thread);
    thread_join(&server->tcp_thread);
    net_close(server->udp);
    net_close(server->tcp);
}

static DnsResolver *dns_test_resolver(Test *test, const DnsServer *server)
{
    DnsResolver *resolver = nullptr;
    Err err = { 0 };
    DnsResolverConfig config = { .servers = server->address, .timeout_ms = 100 };
    test_check(test, dns_resolver_create(&config, &resolver, &err) == ERR_OK, err.msg);
    return resolver;
}

static DnsRecords dns_test_lookup(Test *test, DnsResolver *resolver, const char *name, DnsType type)
{
    DnsRecords records = { 0 };
    Err err = { 0 };
    test_check(test, dns_lookup(resolver, test->arena, S(name), type, &records, &err) == ERR_OK, err.msg);
    return records;
}

static String dns_first(DnsRecords records)
{
    return records.count > 0 ? records.items[0].data : S("(none)");
}

static void dns_looks_up_each_type_of_record(Test *test)
{
    Arena *arena = test->arena;
    // A DKIM key of 4096 bits is too long for one TXT string, and for a reply over UDP.
    String key = str_concat(arena, S("v=DKIM1; k=rsa; p="), str_repeat(arena, S("MIICIjANBgkqhkiG9w0B"), 37));
    const DnsZoneRecord records[] = {
        { "a.example", DNS_A, { "\xC0\x00\x02\x01", 4 } },
        { "mx.example", DNS_AAAA, { "\x20\x01\x0D\xB8\0\0\0\0\0\0\0\0\0\0\0\x01", 16 } },
        { "example", DNS_MX, dns_mx_rdata(arena, 20, "backup.example") },
        { "example", DNS_MX, dns_mx_rdata(arena, 10, "mx.example") },
        { "example", DNS_TXT, { "\x08v=spf1 a\x05 -all", 15 } },
        { "s._domainkey.example", DNS_TXT, dns_txt_rdata(arena, key) },
        { "1.2.0.192.in-addr.arpa", DNS_PTR, dns_wire_name(arena, "mx.example") },
    };
    DnsServer server;
    if (!dns_server_start(test, &server, records, countof(records))) {
        return;
    }
    DnsResolver *resolver = dns_test_resolver(test, &server);
    if (resolver != nullptr) {
        DnsRecords a = dns_test_lookup(test, resolver, "a.example", DNS_A);
        test_check_str(test, dns_first(a), S("192.0.2.1"), "an A record's address");
        test_check(test, a.count == 1 && a.items[0].ttl == 300, "with its TTL");
        test_check_str(test, dns_first(dns_test_lookup(test, resolver, "mx.example", DNS_AAAA)), S("2001:db8::1"),
                       "an AAAA record's address, shortened");
        DnsRecords mx = dns_test_lookup(test, resolver, "example", DNS_MX);
        test_check(test,
                   mx.count == 2 && str_equal(mx.items[0].data, S("backup.example")) && mx.items[0].preference == 20 &&
                       str_equal(mx.items[1].data, S("mx.example")) && mx.items[1].preference == 10,
                   "MX records, each with its preference");
        test_check_str(test, dns_first(dns_test_lookup(test, resolver, "example", DNS_TXT)), S("v=spf1 a -all"),
                       "a TXT record's strings joined");
        test_check_str(test, dns_first(dns_test_lookup(test, resolver, "s._domainkey.example", DNS_TXT)), key,
                       "a TXT record too long for UDP");
        test_check(test, atomic_load(&server.tcp_queries) == 1, "came over TCP");
        test_check_str(test, dns_first(dns_test_lookup(test, resolver, "1.2.0.192.in-addr.arpa", DNS_PTR)),
                       S("mx.example"), "a PTR record's name");
        dns_resolver_destroy(resolver);
    }
    dns_server_stop(&server);
}

static void dns_tells_a_missing_name_from_missing_records(Test *test)
{
    const DnsZoneRecord records[] = { { "example", DNS_A, { "\xC0\x00\x02\x01", 4 } } };
    DnsServer server;
    if (!dns_server_start(test, &server, records, countof(records))) {
        return;
    }
    DnsResolver *resolver = dns_test_resolver(test, &server);
    if (resolver != nullptr) {
        test_check(test, dns_test_lookup(test, resolver, "example", DNS_TXT).count == 0,
                   "a name without records of the type has none");
        DnsRecords records_found = { 0 };
        Err err = { 0 };
        test_check(test, dns_lookup(resolver, test->arena, S("nowhere.example"), DNS_A, &records_found, &err) ==
                             ERR_NOT_FOUND,
                   "a name that does not exist is not found");
        dns_resolver_destroy(resolver);
    }
    dns_server_stop(&server);
}

static void dns_reports_failures_and_forged_replies(Test *test)
{
    DnsServer server;
    if (!dns_server_start(test, &server, nullptr, 0)) {
        return;
    }
    DnsResolver *resolver = dns_test_resolver(test, &server);
    if (resolver != nullptr) {
        DnsRecords records = { 0 };
        Err err = { 0 };
        test_check(test, dns_lookup(resolver, test->arena, S(DNS_SERVFAIL), DNS_A, &records, &err) == ERR_NETWORK,
                   "a server's failure");
        int64_t started = clock_monotonic_ns();
        test_check(test, dns_lookup(resolver, test->arena, S(DNS_LOWERCASED), DNS_A, &records, &err) == ERR_TIMEOUT,
                   "a reply whose question lost its random case is dropped, and the lookup times out");
        test_check(test, clock_monotonic_ns() - started < INT64_C(2000000000), "after the tries' timeouts");
        test_check(test, dns_lookup(resolver, test->arena, (String){ "a\0b.example", 11 }, DNS_A, &records, &err) ==
                             ERR_INVALID_ARGUMENT,
                   "a name with a NUL in it is refused, not cut short");
        dns_resolver_destroy(resolver);
    }
    dns_server_stop(&server);
}

const TestCase DNS_TESTS[] = {
    { "dns_looks_up_each_type_of_record", dns_looks_up_each_type_of_record },
    { "dns_tells_a_missing_name_from_missing_records", dns_tells_a_missing_name_from_missing_records },
    { "dns_reports_failures_and_forged_replies", dns_reports_failures_and_forged_replies },
    { nullptr, nullptr },
};
