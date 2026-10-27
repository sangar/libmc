#include "mc/net/tls.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/psa_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>

#include "mc/crypto/sha256.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "tls_acme.h"
#include "tls_session.h"

enum {
    PEM_CAPACITY = 4096,
    SERIAL_BYTES = 16,
    SECONDS_PER_DAY = 86400,
    MAX_CHALLENGES = 100,
    P256_BITS = 256,
    COORDINATE_BYTES = 32,
    PUBLIC_KEY_DER_CAPACITY = 128,
    REQUEST_CAPACITY = 4096,
};

static const char ACME_TLS[] = "acme-tls/1";
// ACME_IDENTIFIER is id-pe-acmeIdentifier, 1.3.6.1.5.5.7.1.31, as DER content (RFC 8737).
static const unsigned char ACME_IDENTIFIER[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x1f };

// Credentials are a certificate chain and its key, shared by the handshakes
// that present them; the last to let go frees them.
typedef struct Credentials {
    Arena *arena;
    String host; // the name a TLS-ALPN-01 challenge certificate answers for
    atomic_size_t references;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
} Credentials;

struct TlsServer {
    Arena *arena;
    const char **protocols; // what ALPN offers, nullptr-terminated, for as long as config
    mbedtls_ssl_config config;
    Mutex mutex; // guards current and the challenges
    Credentials *current;
    Credentials *challenges[MAX_CHALLENGES];
    size_t challenge_count;
};

struct TlsSession {
    mbedtls_ssl_context ssl;
    int fd;
    TlsServer *server;
    Credentials *credentials; // what this session's handshake presents, once it starts
    bool challenge;           // the peer is a CA checking a TLS-ALPN-01 challenge
};

static const String HTTP_PROTOCOLS[] = { { "h2", 2 }, { "http/1.1", 8 }, { "acme-tls/1", 10 } };

[[nodiscard]] static Error tls_fail(Err *err, const char *what, int status)
{
    return err_set(err, ERR_PLATFORM, "tls: %s: -0x%04x", what, (unsigned)-status);
}

[[nodiscard]] static Error crypto_start(Err *err)
{
    return psa_crypto_init() == PSA_SUCCESS ? ERR_OK : err_set(err, ERR_PLATFORM, "tls: cannot start PSA crypto");
}

// pem_bytes is text as mbedTLS parses PEM: with its terminating NUL counted.
static const unsigned char *pem_bytes(Arena *arena, String text, size_t *len)
{
    String copy = str_copy(arena, text);
    *len = copy.len + 1;
    return (const unsigned char *)copy.data;
}

static void credentials_release(Credentials *credentials)
{
    if (atomic_fetch_sub(&credentials->references, 1) == 1) {
        mbedtls_x509_crt_free(&credentials->chain);
        mbedtls_pk_free(&credentials->key);
        arena_destroy(credentials->arena);
    }
}

[[nodiscard]] static Error credentials_create(String certificate_pem, String key_pem, Credentials **credentials,
                                              Err *err)
{
    Arena *arena = arena_create(0);
    Credentials *created = arena_push(arena, sizeof *created);
    created->arena = arena;
    atomic_init(&created->references, 1);
    mbedtls_x509_crt_init(&created->chain);
    mbedtls_pk_init(&created->key);
    size_t certificate_len = 0;
    size_t key_len = 0;
    const unsigned char *certificate = pem_bytes(arena, certificate_pem, &certificate_len);
    const unsigned char *key = pem_bytes(arena, key_pem, &key_len);
    int status = mbedtls_x509_crt_parse(&created->chain, certificate, certificate_len);
    if (status != 0) {
        credentials_release(created);
        return tls_fail(err, "cannot parse the certificate", status);
    }
    status = mbedtls_pk_parse_key(&created->key, key, key_len, nullptr, 0);
    if (status == 0) {
        status = mbedtls_pk_check_pair(&created->chain.pk, &created->key);
    }
    if (status != 0) {
        credentials_release(created);
        return tls_fail(err, "cannot use the key", status);
    }
    *credentials = created;
    return ERR_OK;
}

static Credentials *credentials_current(TlsServer *server)
{
    mutex_lock(&server->mutex);
    Credentials *credentials = server->current;
    atomic_fetch_add(&credentials->references, 1);
    mutex_unlock(&server->mutex);
    return credentials;
}

// challenge_for is the challenge certificate for host, or nullptr.
static Credentials *challenge_for(TlsServer *server, String host)
{
    Credentials *found = nullptr;
    mutex_lock(&server->mutex);
    for (size_t i = 0; i < server->challenge_count && found == nullptr; i++) {
        if (str_equal_ignore_case(server->challenges[i]->host, host)) {
            found = server->challenges[i];
            atomic_fetch_add(&found->references, 1);
        }
    }
    mutex_unlock(&server->mutex);
    return found;
}

// present_certificate gives each handshake the server's current certificate,
// or, when it negotiated acme-tls/1, the challenge certificate for its name.
static int present_certificate(mbedtls_ssl_context *ssl)
{
    TlsSession *session = mbedtls_ssl_get_user_data_p(ssl);
    const char *protocol = mbedtls_ssl_get_alpn_protocol(ssl);
    session->challenge = protocol != nullptr && str_equal(S(protocol), S(ACME_TLS));
    if (session->credentials == nullptr && session->challenge) {
        size_t len = 0;
        const unsigned char *name = mbedtls_ssl_get_hs_sni(ssl, &len);
        session->credentials = name != nullptr ? challenge_for(session->server, (String){ (const char *)name, len }) : nullptr;
        if (session->credentials == nullptr) {
            return MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE;
        }
    }
    if (session->credentials == nullptr) {
        session->credentials = credentials_current(session->server);
    }
    return mbedtls_ssl_set_hs_own_cert(ssl, &session->credentials->chain, &session->credentials->key);
}

static void server_free(TlsServer *server)
{
    mbedtls_ssl_config_free(&server->config);
    mutex_destroy(&server->mutex);
    arena_destroy(server->arena);
}

// protocol_list copies protocols as mbedTLS takes them.
static const char **protocol_list(Arena *arena, const String *protocols, size_t count)
{
    const char **list = arena_push(arena, (count + 1) * sizeof *list);
    for (size_t i = 0; i < count; i++) {
        list[i] = str_cstr(arena, protocols[i]);
    }
    list[count] = nullptr;
    return list;
}

Error tls_server_create(const TlsServerConfig *config, TlsServer **server, Err *err)
{
    Error e = crypto_start(err);
    Credentials *credentials = nullptr;
    if (e == ERR_OK) {
        e = credentials_create(config->certificate_pem, config->key_pem, &credentials, err);
    }
    if (e != ERR_OK) {
        return e;
    }
    Arena *arena = arena_create(0);
    TlsServer *created = arena_push(arena, sizeof *created);
    created->arena = arena;
    // With no protocols named, it offers HTTP/2, HTTP/1.1 and ACME's acme-tls/1.
    created->protocols = config->protocol_count > 0 ? protocol_list(arena, config->protocols, config->protocol_count)
                                                    : protocol_list(arena, HTTP_PROTOCOLS, countof(HTTP_PROTOCOLS));
    created->current = credentials;
    mutex_init(&created->mutex);
    mbedtls_ssl_config_init(&created->config);
    int status = mbedtls_ssl_config_defaults(&created->config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT);
    if (status == 0) {
        mbedtls_ssl_conf_min_tls_version(&created->config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_cert_cb(&created->config, present_certificate);
        status = mbedtls_ssl_conf_alpn_protocols(&created->config, created->protocols);
    }
    if (status != 0) {
        credentials_release(credentials);
        server_free(created);
        return tls_fail(err, "cannot configure", status);
    }
    *server = created;
    return ERR_OK;
}

Error tls_server_set_certificate(TlsServer *server, String certificate_pem, String key_pem, Err *err)
{
    Credentials *credentials = nullptr;
    Error e = credentials_create(certificate_pem, key_pem, &credentials, err);
    if (e != ERR_OK) {
        return e;
    }
    mutex_lock(&server->mutex);
    Credentials *previous = server->current;
    server->current = credentials;
    mutex_unlock(&server->mutex);
    credentials_release(previous);
    return ERR_OK;
}

[[nodiscard]] static Error challenge_credentials(String host, String key_authorization, Credentials **credentials,
                                                Err *err);

Error tls_server_add_challenge(TlsServer *server, String host, String key_authorization, Err *err)
{
    Credentials *credentials = nullptr;
    Error e = challenge_credentials(host, key_authorization, &credentials, err);
    if (e != ERR_OK) {
        return e;
    }
    mutex_lock(&server->mutex);
    bool added = server->challenge_count < MAX_CHALLENGES;
    if (added) {
        server->challenges[server->challenge_count++] = credentials;
    }
    mutex_unlock(&server->mutex);
    if (!added) {
        credentials_release(credentials);
        return err_set(err, ERR_UNSUPPORTED, "tls: more than %d challenges at once", MAX_CHALLENGES);
    }
    return ERR_OK;
}

void tls_server_clear_challenges(TlsServer *server)
{
    mutex_lock(&server->mutex);
    for (size_t i = 0; i < server->challenge_count; i++) {
        credentials_release(server->challenges[i]);
    }
    server->challenge_count = 0;
    mutex_unlock(&server->mutex);
}

void tls_server_destroy(TlsServer *server)
{
    tls_server_clear_challenges(server);
    credentials_release(server->current);
    server_free(server);
}

// x509_time is a Unix time as X.509 writes it: YYYYMMDDhhmmss in UTC.
static const char *x509_time(Arena *arena, int64_t unix_seconds)
{
    int64_t days = unix_seconds / SECONDS_PER_DAY;
    int64_t seconds = unix_seconds % SECONDS_PER_DAY;
    CivilDate date = civil_from_days(days);
    return str_format(arena, "%04lld%02u%02u%02lld%02lld%02lld", (long long)date.year, date.month, date.day,
                      (long long)(seconds / 3600), (long long)(seconds / 60 % 60), (long long)(seconds % 60))
        .data;
}

// generate_key makes a P-256 key in key, which the caller frees.
[[nodiscard]] static Error generate_key(mbedtls_pk_context *key, Err *err)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_generate_key(&attributes, &id) != PSA_SUCCESS) {
        return err_set(err, ERR_PLATFORM, "tls: cannot generate a key");
    }
    int status = mbedtls_pk_copy_from_psa(id, key);
    psa_destroy_key(id);
    return status == 0 ? ERR_OK : tls_fail(err, "cannot copy the key", status);
}

// write_certificate describes a self-signed certificate for host, carrying
// acme_identifier, a SHA-256, in RFC 8737's critical extension when given.
[[nodiscard]] static Error write_certificate(Arena *arena, mbedtls_x509write_cert *writer, mbedtls_pk_context *key,
                                             String host, unsigned days, const uint8_t *acme_identifier, Err *err)
{
    unsigned char serial[SERIAL_BYTES];
    if (psa_generate_random(serial, sizeof serial) != PSA_SUCCESS) {
        return err_set(err, ERR_PLATFORM, "tls: cannot make a serial number");
    }
    serial[0] &= 0x7f; // a serial number is a positive integer
    int64_t now = clock_wall_ns() / 1000000000;
    const char *name = str_format(arena, "CN=%.*s", (int)host.len, host.data).data;
    mbedtls_x509_san_list san = {
        .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                  .san.unstructured_name = { .len = host.len, .p = (unsigned char *)str_copy(arena, host).data } },
    };
    mbedtls_x509write_crt_set_version(writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(writer, key);
    mbedtls_x509write_crt_set_issuer_key(writer, key);
    int status = mbedtls_x509write_crt_set_subject_name(writer, name);
    if (status == 0) {
        status = mbedtls_x509write_crt_set_issuer_name(writer, name);
    }
    if (status == 0) {
        status = mbedtls_x509write_crt_set_serial_raw(writer, serial, sizeof serial);
    }
    if (status == 0) {
        status = mbedtls_x509write_crt_set_validity(writer, x509_time(arena, now),
                                                    x509_time(arena, now + (int64_t)days * SECONDS_PER_DAY));
    }
    if (status == 0) {
        status = mbedtls_x509write_crt_set_basic_constraints(writer, 0, -1);
    }
    if (status == 0) {
        status = mbedtls_x509write_crt_set_subject_alternative_name(writer, &san);
    }
    if (status == 0 && acme_identifier != nullptr) {
        // The extension's value is the digest as a DER OCTET STRING.
        unsigned char value[2 + SHA256_LEN] = { 0x04, SHA256_LEN };
        memcpy(value + 2, acme_identifier, SHA256_LEN);
        status = mbedtls_x509write_crt_set_extension(writer, (const char *)ACME_IDENTIFIER, sizeof ACME_IDENTIFIER, 1,
                                                     value, sizeof value);
    }
    return status == 0 ? ERR_OK : tls_fail(err, "cannot describe the certificate", status);
}

[[nodiscard]] static Error write_pem(Arena *arena, mbedtls_x509write_cert *writer, mbedtls_pk_context *key, String *certificate_pem,
                       String *key_pem, Err *err)
{
    unsigned char *certificate = arena_push(arena, PEM_CAPACITY);
    unsigned char *key_text = arena_push(arena, PEM_CAPACITY);
    int status = mbedtls_x509write_crt_pem(writer, certificate, PEM_CAPACITY);
    if (status == 0) {
        status = mbedtls_pk_write_key_pem(key, key_text, PEM_CAPACITY);
    }
    if (status != 0) {
        return tls_fail(err, "cannot write PEM", status);
    }
    *certificate_pem = S((const char *)certificate);
    *key_pem = S((const char *)key_text);
    return ERR_OK;
}

[[nodiscard]] static Error make_certificate(Arena *arena, String host, unsigned days, String *certificate_pem,
                                            String *key_pem, Err *err)
{
    Error e = crypto_start(err);
    if (e != ERR_OK) {
        return e;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    e = generate_key(&key, err);
    if (e == ERR_OK) {
        e = write_certificate(arena, &writer, &key, host, days, nullptr, err);
    }
    if (e == ERR_OK) {
        e = write_pem(arena, &writer, &key, certificate_pem, key_pem, err);
    }
    mbedtls_x509write_crt_free(&writer);
    mbedtls_pk_free(&key);
    return e;
}

Error tls_self_signed(Arena *arena, String host, unsigned days, String *certificate_pem, String *key_pem, Err *err)
{
    return make_certificate(arena, host, days, certificate_pem, key_pem, err);
}

// accept_acme_identifier lets the parser keep RFC 8737's critical extension,
// which it does not know, and refuses any other it does not.
static int accept_acme_identifier(void *context, const mbedtls_x509_crt *certificate, const mbedtls_x509_buf *oid,
                                  int critical, const unsigned char *p, const unsigned char *end)
{
    return oid->len == sizeof ACME_IDENTIFIER && memcmp(oid->p, ACME_IDENTIFIER, oid->len) == 0 ? 0 : -1;
}

// challenge_credentials makes the self-signed certificate that answers a
// TLS-ALPN-01 challenge for host: it carries the SHA-256 of
// key_authorization in the critical acmeIdentifier extension.
[[nodiscard]] static Error challenge_credentials(String host, String key_authorization, Credentials **credentials,
                                                Err *err)
{
    Error e = crypto_start(err);
    if (e != ERR_OK) {
        return e;
    }
    uint8_t digest[SHA256_LEN];
    sha256(key_authorization, digest);
    Arena *arena = arena_create(0);
    Credentials *created = arena_push(arena, sizeof *created);
    *created = (Credentials){ .arena = arena, .host = str_copy(arena, host) };
    atomic_init(&created->references, 1);
    mbedtls_x509_crt_init(&created->chain);
    mbedtls_pk_init(&created->key);
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    e = generate_key(&created->key, err);
    if (e == ERR_OK) {
        e = write_certificate(arena, &writer, &created->key, host, 1, digest, err);
    }
    unsigned char *der = arena_push(arena, PEM_CAPACITY);
    int written = e == ERR_OK ? mbedtls_x509write_crt_der(&writer, der, PEM_CAPACITY) : 0;
    mbedtls_x509write_crt_free(&writer);
    if (e == ERR_OK && written <= 0) {
        e = tls_fail(err, "cannot write the challenge certificate", written);
    }
    // mbedTLS writes DER at the end of the buffer.
    int status = e == ERR_OK ? mbedtls_x509_crt_parse_der_with_ext_cb(&created->chain, der + PEM_CAPACITY - written,
                                                                        (size_t)written, 1, accept_acme_identifier, nullptr)
                             : 0;
    if (e == ERR_OK && status != 0) {
        e = tls_fail(err, "cannot read the challenge certificate", status);
    }
    if (e != ERR_OK) {
        credentials_release(created);
        return e;
    }
    *credentials = created;
    return ERR_OK;
}

static int socket_send(void *context, const unsigned char *data, size_t len)
{
    TlsSession *session = context;
    size_t sent = 0;
    switch (net_try_send(session->fd, (String){ (const char *)data, len }, &sent)) {
    case NET_DONE:
        return (int)sent;
    case NET_WOULD_BLOCK:
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    case NET_CLOSED:
        break;
    }
    return MBEDTLS_ERR_NET_CONN_RESET;
}

static int socket_receive(void *context, unsigned char *buffer, size_t capacity)
{
    TlsSession *session = context;
    size_t got = 0;
    switch (net_try_receive(session->fd, buffer, capacity, &got)) {
    case NET_DONE:
        return (int)got;
    case NET_WOULD_BLOCK:
        return MBEDTLS_ERR_SSL_WANT_READ;
    case NET_CLOSED:
        break;
    }
    return 0;
}

static int blocking_send(void *context, const unsigned char *data, size_t len)
{
    TlsSession *session = context;
    return net_send(session->fd, (String){ (const char *)data, len }, nullptr) == ERR_OK ? (int)len
                                                                                          : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int blocking_receive(void *context, unsigned char *buffer, size_t capacity)
{
    TlsSession *session = context;
    size_t got = 0;
    return net_receive(session->fd, buffer, capacity, &got, nullptr) == ERR_OK ? (int)got : MBEDTLS_ERR_NET_RECV_FAILED;
}

// session_start sets up TLS on fd with config, for server unless it is
// nullptr, reading and writing with waits when blocking, else as an event
// loop would.
static TlsSession *session_start(Arena *arena, const mbedtls_ssl_config *config, TlsServer *server, int fd,
                                 bool blocking)
{
    TlsSession *session = arena_push(arena, sizeof *session);
    *session = (TlsSession){ .fd = fd, .server = server };
    mbedtls_ssl_init(&session->ssl);
    // Setup fails only when out of memory, which aborts everywhere else in libmc.
    if (mbedtls_ssl_setup(&session->ssl, config) != 0) {
        abort();
    }
    mbedtls_ssl_set_user_data_p(&session->ssl, session);
    mbedtls_ssl_set_bio(&session->ssl, session, blocking ? blocking_send : socket_send,
                        blocking ? blocking_receive : socket_receive, nullptr);
    return session;
}

TlsSession *tls_session_create(Arena *arena, TlsServer *server, int fd)
{
    return session_start(arena, &server->config, server, fd, false);
}

void tls_session_destroy(TlsSession *session)
{
    (void)mbedtls_ssl_close_notify(&session->ssl);
    mbedtls_ssl_free(&session->ssl);
    if (session->credentials != nullptr) {
        credentials_release(session->credentials);
    }
}

static TlsResult result_of(int status)
{
    switch (status) {
    case MBEDTLS_ERR_SSL_WANT_READ:
        return TLS_WANT_READ;
    case MBEDTLS_ERR_SSL_WANT_WRITE:
        return TLS_WANT_WRITE;
    default:
        return status > 0 ? TLS_DONE : TLS_CLOSED;
    }
}

TlsResult tls_try_receive(TlsSession *session, char *buffer, size_t capacity, size_t *got)
{
    int status = mbedtls_ssl_read(&session->ssl, (unsigned char *)buffer, capacity);
    *got = status > 0 ? (size_t)status : 0;
    // A CA checking a challenge needs only the handshake, so nothing is served on it.
    if (session->challenge && mbedtls_ssl_is_handshake_over(&session->ssl)) {
        *got = 0;
        return TLS_CLOSED;
    }
    return result_of(status);
}

String tls_session_protocol(const TlsSession *session)
{
    const char *protocol = mbedtls_ssl_get_alpn_protocol(&session->ssl);
    return protocol != nullptr ? S(protocol) : S("");
}

TlsResult tls_try_send(TlsSession *session, String data, size_t *sent)
{
    int status = mbedtls_ssl_write(&session->ssl, (const unsigned char *)data.data, data.len);
    *sent = status > 0 ? (size_t)status : 0;
    return result_of(status);
}

struct TlsConnection {
    Arena *arena;
    TlsSession *session;
    bool client; // and so owns what follows
    mbedtls_ssl_config config;
    mbedtls_x509_crt trusted;
};

Error tls_accept(TlsServer *server, int fd, TlsConnection **connection, Err *err)
{
    Arena *arena = arena_create(0);
    TlsConnection *accepted = arena_push(arena, sizeof *accepted);
    *accepted = (TlsConnection){ .arena = arena, .session = session_start(arena, &server->config, server, fd, true) };
    int status = mbedtls_ssl_handshake(&accepted->session->ssl);
    if (status != 0) {
        tls_close(accepted);
        return err_set(err, ERR_NETWORK, "tls: handshake failed: -0x%04x", (unsigned)-status);
    }
    *connection = accepted;
    return ERR_OK;
}

// client_configure trusts config's certificates and offers its protocols.
static int client_configure(TlsConnection *connection, const TlsClientConfig *config)
{
    size_t len = 0;
    const unsigned char *pem = pem_bytes(connection->arena, config->ca_pem, &len);
    int status = mbedtls_x509_crt_parse(&connection->trusted, pem, len);
    if (status == 0) {
        status = mbedtls_ssl_config_defaults(&connection->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (status != 0) {
        return status;
    }
    mbedtls_ssl_conf_min_tls_version(&connection->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&connection->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&connection->config, &connection->trusted, nullptr);
    if (config->protocol_count == 0) {
        return 0;
    }
    return mbedtls_ssl_conf_alpn_protocols(&connection->config,
                                           protocol_list(connection->arena, config->protocols, config->protocol_count));
}

Error tls_connect(const TlsClientConfig *config, int fd, TlsConnection **connection, Err *err)
{
    Error e = crypto_start(err);
    if (e != ERR_OK) {
        return e;
    }
    Arena *arena = arena_create(0);
    TlsConnection *connected = arena_push(arena, sizeof *connected);
    *connected = (TlsConnection){ .arena = arena, .client = true };
    mbedtls_ssl_config_init(&connected->config);
    mbedtls_x509_crt_init(&connected->trusted);
    int status = client_configure(connected, config);
    if (status != 0) {
        tls_close(connected);
        return tls_fail(err, "cannot configure the client", status);
    }
    connected->session = session_start(arena, &connected->config, nullptr, fd, true);
    status = mbedtls_ssl_set_hostname(&connected->session->ssl, str_cstr(arena, config->host));
    if (status == 0) {
        status = mbedtls_ssl_handshake(&connected->session->ssl);
    }
    if (status != 0) {
        tls_close(connected);
        return err_set(err, ERR_NETWORK, "tls: handshake failed: -0x%04x", (unsigned)-status);
    }
    *connection = connected;
    return ERR_OK;
}

String tls_protocol(const TlsConnection *connection)
{
    const char *protocol = mbedtls_ssl_get_alpn_protocol(&connection->session->ssl);
    return protocol != nullptr ? S(protocol) : S("");
}

Error tls_read(TlsConnection *connection, void *buffer, size_t capacity, size_t *got, Err *err)
{
    int status = mbedtls_ssl_read(&connection->session->ssl, buffer, capacity);
    *got = status > 0 ? (size_t)status : 0;
    if (status >= 0 || status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return ERR_OK;
    }
    return err_set(err, ERR_NETWORK, "tls: read failed: -0x%04x", (unsigned)-status);
}

Error tls_write(TlsConnection *connection, String data, Err *err)
{
    for (size_t sent = 0; sent < data.len;) {
        int status = mbedtls_ssl_write(&connection->session->ssl, (const unsigned char *)data.data + sent,
                                       data.len - sent);
        if (status <= 0) {
            return err_set(err, ERR_NETWORK, "tls: write failed: -0x%04x", (unsigned)-status);
        }
        sent += (size_t)status;
    }
    return ERR_OK;
}

Error tls_shutdown(TlsConnection *connection, Err *err)
{
    int status = mbedtls_ssl_close_notify(&connection->session->ssl);
    return status == 0 ? ERR_OK : err_set(err, ERR_NETWORK, "tls: close_notify failed: -0x%04x", (unsigned)-status);
}

void tls_close(TlsConnection *connection)
{
    if (connection->session != nullptr) {
        tls_session_destroy(connection->session);
    }
    if (connection->client) {
        mbedtls_ssl_config_free(&connection->config);
        mbedtls_x509_crt_free(&connection->trusted);
    }
    arena_destroy(connection->arena);
}

// ---- keys for ACME ----

struct TlsKey {
    Arena *arena;
    mbedtls_pk_context pk;
};

Error tls_key_generate(Arena *arena, String *pem, Err *err)
{
    Error e = crypto_start(err);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    if (e == ERR_OK) {
        e = generate_key(&key, err);
    }
    unsigned char *text = arena_push(arena, PEM_CAPACITY);
    int status = e == ERR_OK ? mbedtls_pk_write_key_pem(&key, text, PEM_CAPACITY) : 0;
    mbedtls_pk_free(&key);
    if (e == ERR_OK && status != 0) {
        e = tls_fail(err, "cannot write the key", status);
    }
    if (e == ERR_OK) {
        *pem = S((const char *)text);
    }
    return e;
}

Error tls_key_load(String pem, TlsKey **key, Err *err)
{
    Error e = crypto_start(err);
    if (e != ERR_OK) {
        return e;
    }
    Arena *arena = arena_create(0);
    TlsKey *loaded = arena_push(arena, sizeof *loaded);
    loaded->arena = arena;
    mbedtls_pk_init(&loaded->pk);
    // mbedTLS parses PEM with its terminating NUL counted.
    String text = str_copy(arena, pem);
    int status = mbedtls_pk_parse_key(&loaded->pk, (const unsigned char *)text.data, text.len + 1, nullptr, 0);
    if (status != 0) {
        tls_key_destroy(loaded);
        return tls_fail(err, "cannot parse the key", status);
    }
    if (mbedtls_pk_get_key_type(&loaded->pk) != PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1) ||
        mbedtls_pk_get_bitlen(&loaded->pk) != P256_BITS) {
        tls_key_destroy(loaded);
        return err_set(err, ERR_UNSUPPORTED, "tls: the key is not a P-256 key");
    }
    *key = loaded;
    return ERR_OK;
}

void tls_key_destroy(TlsKey *key)
{
    mbedtls_pk_free(&key->pk);
    arena_destroy(key->arena);
}

Error tls_key_public_point(Arena *arena, const TlsKey *key, String *x, String *y, Err *err)
{
    unsigned char der[PUBLIC_KEY_DER_CAPACITY];
    int written = mbedtls_pk_write_pubkey_der(&key->pk, der, sizeof der);
    // The DER ends in the uncompressed point: 0x04, then x and y.
    size_t point_bytes = 1 + 2 * COORDINATE_BYTES;
    if (written < (int)point_bytes || der[sizeof der - point_bytes] != 0x04) {
        return err_set(err, ERR_PLATFORM, "tls: cannot read the public key");
    }
    const char *point = (const char *)der + sizeof der - point_bytes + 1;
    *x = str_copy(arena, (String){ point, COORDINATE_BYTES });
    *y = str_copy(arena, (String){ point + COORDINATE_BYTES, COORDINATE_BYTES });
    return ERR_OK;
}

Error tls_key_sign(Arena *arena, TlsKey *key, String data, String *signature, Err *err)
{
    unsigned char hash[SHA256_LEN];
    size_t hash_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const unsigned char *)data.data, data.len, hash, sizeof hash, &hash_len) !=
        PSA_SUCCESS) {
        return err_set(err, ERR_PLATFORM, "tls: cannot hash");
    }
    unsigned char der[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t der_len = 0;
    int status = mbedtls_pk_sign(&key->pk, MBEDTLS_MD_SHA256, hash, hash_len, der, sizeof der, &der_len);
    unsigned char *raw = arena_push(arena, 2 * COORDINATE_BYTES);
    size_t raw_len = 0;
    if (status == 0) {
        status = mbedtls_ecdsa_der_to_raw(P256_BITS, der, der_len, raw, 2 * COORDINATE_BYTES, &raw_len);
    }
    if (status != 0) {
        return tls_fail(err, "cannot sign", status);
    }
    *signature = (String){ (const char *)raw, raw_len };
    return ERR_OK;
}

// dns_names lists names as the subject alternative names extension takes them, in arena.
static mbedtls_x509_san_list *dns_names(Arena *arena, const String *names, size_t name_count)
{
    mbedtls_x509_san_list *list = nullptr;
    for (size_t i = name_count; i > 0; i--) {
        mbedtls_x509_san_list *entry = arena_push(arena, sizeof *entry);
        String name = str_copy(arena, names[i - 1]);
        *entry = (mbedtls_x509_san_list){
            .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                      .san.unstructured_name = { .len = name.len, .p = (unsigned char *)name.data } },
            .next = list,
        };
        list = entry;
    }
    return list;
}

Error tls_key_request(Arena *arena, TlsKey *key, const String *names, size_t name_count, String *der, Err *err)
{
    if (name_count == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "tls: a certificate request needs a name");
    }
    mbedtls_x509write_csr writer;
    mbedtls_x509write_csr_init(&writer);
    mbedtls_x509write_csr_set_key(&writer, &key->pk);
    mbedtls_x509write_csr_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    const char *subject = str_format(arena, "CN=%.*s", (int)names[0].len, names[0].data).data;
    int status = mbedtls_x509write_csr_set_subject_name(&writer, subject);
    if (status == 0) {
        status = mbedtls_x509write_csr_set_subject_alternative_name(&writer, dns_names(arena, names, name_count));
    }
    unsigned char *buffer = arena_push(arena, REQUEST_CAPACITY);
    int written = status == 0 ? mbedtls_x509write_csr_der(&writer, buffer, REQUEST_CAPACITY) : status;
    mbedtls_x509write_csr_free(&writer);
    if (written <= 0) {
        return tls_fail(err, "cannot write the certificate request", written);
    }
    // mbedTLS writes DER at the end of the buffer.
    *der = (String){ (const char *)buffer + REQUEST_CAPACITY - written, (size_t)written };
    return ERR_OK;
}

static bool names_host(const mbedtls_x509_crt *certificate, String host)
{
    for (const mbedtls_x509_sequence *entry = &certificate->subject_alt_names; entry != nullptr; entry = entry->next) {
        mbedtls_x509_subject_alternative_name name;
        if (mbedtls_x509_parse_subject_alt_name(&entry->buf, &name) != 0) {
            continue;
        }
        bool match = name.type == MBEDTLS_X509_SAN_DNS_NAME &&
                     str_equal_ignore_case((String){ (const char *)name.san.unstructured_name.p,
                                                     name.san.unstructured_name.len },
                                           host);
        mbedtls_x509_free_subject_alt_name(&name);
        if (match) {
            return true;
        }
    }
    return false;
}

bool tls_certificate_matches(String chain_pem, const TlsKey *key, String host)
{
    Arena *scratch = arena_create(0);
    String text = str_copy(scratch, chain_pem);
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    bool matches = mbedtls_x509_crt_parse(&chain, (const unsigned char *)text.data, text.len + 1) == 0 &&
                   mbedtls_pk_check_pair(&chain.pk, &key->pk) == 0 && names_host(&chain, host);
    mbedtls_x509_crt_free(&chain);
    arena_destroy(scratch);
    return matches;
}

static int64_t x509_unix_seconds(const mbedtls_x509_time *time)
{
    CivilDate date = { .year = time->year, .month = (unsigned)time->mon, .day = (unsigned)time->day };
    return civil_to_days(date) * SECONDS_PER_DAY + time->hour * 3600 + time->min * 60 + time->sec;
}

Error tls_certificate_validity(String chain_pem, int64_t *not_before, int64_t *not_after, Err *err)
{
    Arena *scratch = arena_create(0);
    String text = str_copy(scratch, chain_pem);
    mbedtls_x509_crt chain;
    mbedtls_x509_crt_init(&chain);
    int status = mbedtls_x509_crt_parse(&chain, (const unsigned char *)text.data, text.len + 1);
    if (status == 0) {
        *not_before = x509_unix_seconds(&chain.valid_from);
        *not_after = x509_unix_seconds(&chain.valid_to);
    }
    mbedtls_x509_crt_free(&chain);
    arena_destroy(scratch);
    return status == 0 ? ERR_OK : tls_fail(err, "cannot parse the certificate", status);
}
