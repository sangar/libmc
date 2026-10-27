#include "mc/net/tls.h"

#include <stdatomic.h>
#include <stdlib.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "tls_session.h"

enum { PEM_CAPACITY = 4096, SERIAL_BYTES = 16, SECONDS_PER_DAY = 86400 };

// Credentials are a certificate chain and its key, shared by the handshakes
// that present them; the last to let go frees them.
typedef struct Credentials {
    Arena *arena;
    atomic_size_t references;
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
} Credentials;

struct TlsServer {
    Arena *arena;
    const char **protocols; // what ALPN offers, nullptr-terminated, for as long as config
    mbedtls_ssl_config config;
    Mutex mutex; // guards current
    Credentials *current;
};

struct TlsSession {
    mbedtls_ssl_context ssl;
    int fd;
    TlsServer *server;
    Credentials *credentials; // what this session's handshake presents, once it starts
};

static const String HTTP_1_1[] = { { "http/1.1", 8 } };

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

// present_certificate gives each handshake the server's current certificate.
static int present_certificate(mbedtls_ssl_context *ssl)
{
    TlsSession *session = mbedtls_ssl_get_user_data_p(ssl);
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

// protocol_list copies protocols as mbedTLS takes them, HTTP/1.1 when there are none.
static const char **protocol_list(Arena *arena, const TlsServerConfig *config)
{
    const String *protocols = config->protocol_count > 0 ? config->protocols : HTTP_1_1;
    size_t count = config->protocol_count > 0 ? config->protocol_count : countof(HTTP_1_1);
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
    created->protocols = protocol_list(arena, config);
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

void tls_server_destroy(TlsServer *server)
{
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

// write_certificate describes a self-signed certificate for host.
[[nodiscard]] static Error write_certificate(Arena *arena, mbedtls_x509write_cert *writer, mbedtls_pk_context *key,
                                             String host, unsigned days, Err *err)
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
        e = write_certificate(arena, &writer, &key, host, days, err);
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

// session_start sets up the server's side of TLS on fd, reading and writing
// it with waits when blocking, else as an event loop would.
static TlsSession *session_start(Arena *arena, TlsServer *server, int fd, bool blocking)
{
    TlsSession *session = arena_push(arena, sizeof *session);
    *session = (TlsSession){ .fd = fd, .server = server };
    mbedtls_ssl_init(&session->ssl);
    // Setup fails only when out of memory, which aborts everywhere else in libmc.
    if (mbedtls_ssl_setup(&session->ssl, &server->config) != 0) {
        abort();
    }
    mbedtls_ssl_set_user_data_p(&session->ssl, session);
    mbedtls_ssl_set_bio(&session->ssl, session, blocking ? blocking_send : socket_send,
                        blocking ? blocking_receive : socket_receive, nullptr);
    return session;
}

TlsSession *tls_session_create(Arena *arena, TlsServer *server, int fd)
{
    return session_start(arena, server, fd, false);
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
    return result_of(status);
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
};

Error tls_accept(TlsServer *server, int fd, TlsConnection **connection, Err *err)
{
    Arena *arena = arena_create(0);
    TlsConnection *accepted = arena_push(arena, sizeof *accepted);
    *accepted = (TlsConnection){ .arena = arena, .session = session_start(arena, server, fd, true) };
    int status = mbedtls_ssl_handshake(&accepted->session->ssl);
    if (status != 0) {
        tls_close(accepted);
        return err_set(err, ERR_NETWORK, "tls: handshake failed: -0x%04x", (unsigned)-status);
    }
    *connection = accepted;
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
    tls_session_destroy(connection->session);
    arena_destroy(connection->arena);
}
