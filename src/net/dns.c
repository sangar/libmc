#include "mc/net/dns.h"

#include <ares.h>
#include <string.h>

#include "mc/platform/platform.h"

enum { DEFAULT_TIMEOUT_MS = 2000, TRIES = 3 };

struct DnsResolver {
    Arena *arena;
    ares_channel_t *channel;
};

// Lookup is one dns_lookup waiting for c-ares's callback, which runs on the
// resolver's thread and copies the answers into the caller's arena.
typedef struct Lookup {
    Mutex mutex;
    Cond answered;
    bool done;
    DnsType type;
    Arena *arena;
    DnsRecords *records;
    ares_status_t status;
} Lookup;

Error dns_resolver_create(const DnsResolverConfig *config, DnsResolver **resolver, Err *err)
{
    *resolver = nullptr;
    // Without threads c-ares cannot run its own event thread, which the
    // blocking lookups wait on.
    if (!ares_threadsafety()) {
        return err_set(err, ERR_UNSUPPORTED, "dns: c-ares was built without threads");
    }
    int64_t timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : DEFAULT_TIMEOUT_MS;
    struct ares_options options = {
        .flags = ARES_FLAG_EDNS | ARES_FLAG_DNS0x20,
        .timeout = (int)timeout_ms,
        .maxtimeout = (int)timeout_ms,
        .tries = TRIES,
        .evsys = ARES_EVSYS_DEFAULT,
    };
    int mask = ARES_OPT_FLAGS | ARES_OPT_TIMEOUTMS | ARES_OPT_MAXTIMEOUTMS | ARES_OPT_TRIES | ARES_OPT_EVENT_THREAD;
    ares_channel_t *channel = nullptr;
    int status = ares_init_options(&channel, &options, mask);
    if (status != ARES_SUCCESS) {
        return err_set(err, ERR_PLATFORM, "dns: %s", ares_strerror(status));
    }
    Arena *arena = arena_create(0);
    if (config->servers.len > 0) {
        status = ares_set_servers_csv(channel, str_cstr(arena, config->servers));
    }
    if (status != ARES_SUCCESS) {
        ares_destroy(channel);
        arena_destroy(arena);
        return err_set(err, ERR_INVALID_ARGUMENT, "dns: servers %.*s: %s", (int)config->servers.len,
                       config->servers.data, ares_strerror(status));
    }
    DnsResolver *created = arena_push(arena, sizeof *created);
    *created = (DnsResolver){ .arena = arena, .channel = channel };
    *resolver = created;
    return ERR_OK;
}

void dns_resolver_destroy(DnsResolver *resolver)
{
    ares_destroy(resolver->channel);
    arena_destroy(resolver->arena);
}

static String address_text(Arena *arena, int family, const void *address)
{
    char text[64];
    return ares_inet_ntop(family, address, text, sizeof text) != nullptr ? str_copy(arena, S(text)) : (String){ 0 };
}

// txt_text joins a TXT record's strings with nothing between them, as RFC 7208 3.3 and RFC 6376 3.6.2.2 read them.
static String txt_text(Arena *arena, const ares_dns_rr_t *rr)
{
    StringBuilder text = str_builder_create(arena, 256);
    size_t count = ares_dns_rr_get_abin_cnt(rr, ARES_RR_TXT_DATA);
    for (size_t i = 0; i < count; i++) {
        size_t len = 0;
        const unsigned char *bytes = ares_dns_rr_get_abin(rr, ARES_RR_TXT_DATA, i, &len);
        str_builder_append(&text, (String){ .data = (const char *)bytes, .len = len });
    }
    return str_builder_finish(&text);
}

static DnsRecord record_of(Arena *arena, const ares_dns_rr_t *rr)
{
    DnsRecord record = { .ttl = ares_dns_rr_get_ttl(rr) };
    switch (ares_dns_rr_get_type(rr)) {
    case ARES_REC_TYPE_A:
        record.data = address_text(arena, AF_INET, ares_dns_rr_get_addr(rr, ARES_RR_A_ADDR));
        break;
    case ARES_REC_TYPE_AAAA:
        record.data = address_text(arena, AF_INET6, ares_dns_rr_get_addr6(rr, ARES_RR_AAAA_ADDR));
        break;
    case ARES_REC_TYPE_PTR:
        record.data = str_copy(arena, S(ares_dns_rr_get_str(rr, ARES_RR_PTR_DNAME)));
        break;
    case ARES_REC_TYPE_MX:
        record.data = str_copy(arena, S(ares_dns_rr_get_str(rr, ARES_RR_MX_EXCHANGE)));
        record.preference = ares_dns_rr_get_u16(rr, ARES_RR_MX_PREFERENCE);
        break;
    case ARES_REC_TYPE_TXT:
        record.data = txt_text(arena, rr);
        break;
    default:
        break;
    }
    return record;
}

// answered keeps the answers of the type asked for: the answer section also
// holds the CNAME records that led to them.
static void answered(void *context, ares_status_t status, size_t timeouts, const ares_dns_record_t *reply)
{
    Lookup *lookup = context;
    mutex_lock(&lookup->mutex);
    lookup->status = status;
    size_t count = reply != nullptr ? ares_dns_record_rr_cnt(reply, ARES_SECTION_ANSWER) : 0;
    DnsRecords *records = lookup->records;
    records->items = count > 0 ? arena_push(lookup->arena, arena_size_mul(count, sizeof *records->items)) : nullptr;
    for (size_t i = 0; i < count; i++) {
        const ares_dns_rr_t *rr = ares_dns_record_rr_get_const(reply, ARES_SECTION_ANSWER, i);
        if ((DnsType)ares_dns_rr_get_type(rr) == lookup->type) {
            records->items[records->count++] = record_of(lookup->arena, rr);
        }
    }
    lookup->done = true;
    cond_signal(&lookup->answered);
    mutex_unlock(&lookup->mutex);
}

[[nodiscard]] static Error lookup_error(ares_status_t status, String name, Err *err)
{
    switch (status) {
    case ARES_SUCCESS:
    case ARES_ENODATA:
        return ERR_OK;
    case ARES_ENOTFOUND:
        return err_set(err, ERR_NOT_FOUND, "dns: %.*s does not exist", (int)name.len, name.data);
    case ARES_ETIMEOUT:
        return err_set(err, ERR_TIMEOUT, "dns: no server answered for %.*s", (int)name.len, name.data);
    case ARES_EBADNAME:
        return err_set(err, ERR_INVALID_ARGUMENT, "dns: %.*s is not a domain name", (int)name.len, name.data);
    case ARES_ECANCELLED:
    case ARES_EDESTRUCTION:
        return err_set(err, ERR_CANCELLED, "dns: lookup of %.*s cancelled", (int)name.len, name.data);
    default:
        return err_set(err, ERR_NETWORK, "dns: %.*s: %s", (int)name.len, name.data, ares_strerror((int)status));
    }
}

Error dns_lookup(DnsResolver *resolver, Arena *arena, String name, DnsType type, DnsRecords *records, Err *err)
{
    *records = (DnsRecords){ 0 };
    // c-ares takes the name as a C string, which a NUL would cut short.
    if (memchr(name.data, '\0', name.len) != nullptr) {
        return lookup_error(ARES_EBADNAME, name, err);
    }
    Lookup lookup = { .type = type, .arena = arena, .records = records };
    mutex_init(&lookup.mutex);
    cond_init(&lookup.answered);
    // c-ares calls answered exactly once, from this call when the answer is
    // cached or the query cannot be sent, else from its thread.
    unused(ares_query_dnsrec(resolver->channel, str_cstr(arena, name), ARES_CLASS_IN, (ares_dns_rec_type_t)type,
                             answered, &lookup, nullptr));
    mutex_lock(&lookup.mutex);
    while (!lookup.done) {
        cond_wait(&lookup.answered, &lookup.mutex);
    }
    mutex_unlock(&lookup.mutex);
    cond_destroy(&lookup.answered);
    mutex_destroy(&lookup.mutex);
    return lookup_error(lookup.status, name, err);
}
