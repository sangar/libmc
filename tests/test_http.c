#include "test.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nghttp2/nghttp2.h"
#include "psa/crypto.h"

#include "mc/net/http.h"
#include "mc/platform/platform.h"

// The tests run the client against a server on 127.0.0.1 in a thread of this
// process, over plain HTTP or TLS with the certificates in tests/fixtures/tls,
// which a test CA there signed for localhost and 127.0.0.1. Over TLS it can
// offer HTTP/2, which it answers through nghttp2. Set LIBMC_LIVE_TESTS=1 to
// also call real hosts; where the system has no bundle of trusted roots, as in
// a bare ubuntu:24.04, that checks the roots built into libmc.

static const char CA_FILE[] = "tests/fixtures/tls/ca.pem";
static const char SERVER_CERT_FILE[] = "tests/fixtures/tls/server.pem";
static const char SERVER_KEY_FILE[] = "tests/fixtures/tls/server-key.pem";

enum { ACCEPT_POLL_MS = 50, REQUEST_LIMIT = 64 * 1024, SLOW_ROUTE_MS = 1000 };

// Connection is one accepted socket, read and written in the clear or through TLS.
typedef struct Connection {
    int fd;
    mbedtls_ssl_context *tls;
} Connection;

static int connection_receive(Connection *connection, char *buffer, size_t capacity)
{
    if (connection->tls != nullptr) {
        int got = mbedtls_ssl_read(connection->tls, (unsigned char *)buffer, capacity);
        return got < 0 ? 0 : got;
    }
    size_t got = 0;
    return net_receive(connection->fd, buffer, capacity, &got, nullptr) == ERR_OK ? (int)got : 0;
}

static void connection_send(Connection *connection, String data)
{
    if (connection->tls == nullptr) {
        (void)net_send(connection->fd, data, nullptr);
        return;
    }
    size_t sent = 0;
    while (sent < data.len) {
        int wrote = mbedtls_ssl_write(connection->tls, (const unsigned char *)data.data + sent, data.len - sent);
        if (wrote <= 0) {
            return;
        }
        sent += (size_t)wrote;
    }
}

// read_request reads the head and a Content-Length body, and returns them as one text.
static String read_request(Arena *arena, Connection *connection)
{
    char *buffer = arena_push(arena, REQUEST_LIMIT);
    size_t received = 0;
    size_t head_end = 0;
    size_t wanted = REQUEST_LIMIT;
    while (received < wanted) {
        int got = connection_receive(connection, buffer + received, REQUEST_LIMIT - received);
        if (got <= 0) {
            break;
        }
        received += (size_t)got;
        String text = { buffer, received };
        if (head_end == 0 && str_find(text, S("\r\n\r\n"), &head_end)) {
            head_end += 4;
            wanted = head_end;
            size_t at = 0;
            uint64_t length = 0;
            if (str_find(text, S("Content-Length: "), &at) &&
                str_parse_u64(str_trim(str_first_line(str_slice(text, at + 16, received))), &length)) {
                wanted = head_end + (size_t)length;
            }
        }
    }
    return (String){ buffer, received };
}

static String answer(Arena *arena, int status, const char *headers, String body)
{
    String head = str_format(arena, "HTTP/1.1 %d Test\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n", status,
                             body.len, headers);
    return str_concat(arena, head, body);
}

// GZIP_HELLO is "hello, gzip" and GZIP_LARGE 64 KiB of "x", both gzipped.
static const unsigned char GZIP_HELLO[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x48,
    0xaf, 0xca, 0x2c, 0x00, 0x00, 0x4a, 0x9b, 0xb1, 0x5c, 0x0b, 0x00, 0x00, 0x00,
};
static const unsigned char GZIP_LARGE[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xed, 0xc1, 0x01, 0x01, 0x00, 0x00, 0x00, 0x80,
    0x90, 0xdb, 0xcd, 0xef, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x9a, 0x58,
    0xb4, 0x2c, 0x00, 0x00, 0x01, 0x00,
};

// route answers the request: /echo sends the request back as the body, the others test one behaviour each.
static String route(Arena *arena, String request)
{
    String line = str_first_line(request);
    String method = { 0 };
    String rest = { 0 };
    String path = { 0 };
    str_cut(line, ' ', &method, &rest);
    str_cut(rest, ' ', &path, &rest);
    if (str_equal(path, S("/echo"))) {
        return answer(arena, 200, "Content-Type: text/plain\r\nX-Test: yes\r\n", request);
    }
    if (str_equal(path, S("/found"))) {
        return answer(arena, 302, "Location: /echo\r\n", S(""));
    }
    if (str_equal(path, S("/see-other"))) {
        return answer(arena, 303, "Location: /echo\r\n", S(""));
    }
    if (str_equal(path, S("/temporary"))) {
        return answer(arena, 307, "Location: /echo\r\n", S(""));
    }
    if (str_equal(path, S("/loop"))) {
        return answer(arena, 302, "Location: /loop\r\n", S(""));
    }
    if (str_equal(path, S("/elsewhere"))) {
        return answer(arena, 302, "Location: http://elsewhere.invalid/echo\r\n", S(""));
    }
    if (str_equal(path, S("/unavailable"))) {
        return answer(arena, 503, "Retry-After: 120\r\n", S("try later"));
    }
    if (str_equal(path, S("/large"))) {
        return answer(arena, 200, "", str_repeat(arena, S("x"), 4096));
    }
    if (str_equal(path, S("/slow"))) {
        clock_sleep_ms(SLOW_ROUTE_MS);
        return answer(arena, 200, "", S("late"));
    }
    if (str_equal(path, S("/large-chunked"))) {
        String chunk = str_repeat(arena, S("x"), 1024);
        return str_format(arena, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                                 "400\r\n%.*s\r\n400\r\n%.*s\r\n0\r\n\r\n",
                          (int)chunk.len, chunk.data, (int)chunk.len, chunk.data);
    }
    if (str_equal(path, S("/gzip"))) {
        return answer(arena, 200, "Content-Encoding: gzip\r\n", (String){ (const char *)GZIP_HELLO, sizeof GZIP_HELLO });
    }
    if (str_equal(path, S("/gzip-large"))) {
        return answer(arena, 200, "Content-Encoding: gzip\r\n", (String){ (const char *)GZIP_LARGE, sizeof GZIP_LARGE });
    }
    if (str_equal(path, S("/chunked"))) {
        return S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                 "6\r\nhello \r\n5\r\nworld\r\n0\r\n\r\n");
    }
    return answer(arena, 404, "", S("no such route"));
}

// TlsServer shares mbedTLS's process-wide crypto state with libcurl, which frees it, keys and all, when the
// last HttpClient is destroyed. pin is an HttpClient that keeps it alive until the server is destroyed.
typedef struct TlsServer {
    mbedtls_ssl_config config;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    HttpClient *pin;
} TlsServer;

typedef struct Server {
    int listener;
    String address;
    TlsServer *tls;
    atomic_bool stopping;
    Thread thread;
} Server;

static int tls_send(void *context, const unsigned char *data, size_t length)
{
    Connection *connection = context;
    return net_send(connection->fd, (String){ (const char *)data, length }, nullptr) == ERR_OK ? (int)length
                                                                                                : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_receive(void *context, unsigned char *buffer, size_t capacity)
{
    Connection *connection = context;
    size_t got = 0;
    if (net_receive(connection->fd, buffer, capacity, &got, nullptr) != ERR_OK) {
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return (int)got;
}

// Http2Connection answers each HTTP/2 request on a connection with HTTP2_BODY.
typedef struct Http2Connection {
    Connection *connection;
    String remaining;
} Http2Connection;

static const char HTTP2_BODY[] = "served over HTTP/2";

static nghttp2_ssize http2_send(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *user_data)
{
    Http2Connection *h2 = user_data;
    connection_send(h2->connection, (String){ (const char *)data, length });
    return (nghttp2_ssize)length;
}

static nghttp2_ssize http2_read_body(nghttp2_session *session, int32_t stream_id, uint8_t *buffer, size_t capacity,
                                     uint32_t *flags, nghttp2_data_source *source, void *user_data)
{
    String *remaining = source->ptr;
    size_t length = min_size(capacity, remaining->len);
    memcpy(buffer, remaining->data, length);
    *remaining = str_slice(*remaining, length, remaining->len);
    if (remaining->len == 0) {
        *flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (nghttp2_ssize)length;
}

// http2_on_frame answers a request once the client has sent all of it.
static int http2_on_frame(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    Http2Connection *h2 = user_data;
    bool request_ends = (frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
                        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
    if (!request_ends) {
        return 0;
    }
    h2->remaining = S(HTTP2_BODY);
    const nghttp2_nv status = { (uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP2_NV_FLAG_NONE };
    const nghttp2_data_provider2 body = { .source.ptr = &h2->remaining, .read_callback = http2_read_body };
    return nghttp2_submit_response2(session, frame->hd.stream_id, &status, 1, &body);
}

// serve_http2 answers requests on a connection that negotiated HTTP/2 until the client closes it. Closing
// first would leave the client's last frames unread, and Linux then resets the connection, discarding the
// response before the client reads it.
static void serve_http2(Connection *connection)
{
    nghttp2_session_callbacks *callbacks = nullptr;
    nghttp2_session *session = nullptr;
    Http2Connection h2 = { .connection = connection };
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        return;
    }
    nghttp2_session_callbacks_set_send_callback2(callbacks, http2_send);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, http2_on_frame);
    int created = nghttp2_session_server_new(&session, callbacks, &h2);
    nghttp2_session_callbacks_del(callbacks);
    if (created != 0) {
        return;
    }
    (void)nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
    char buffer[4096];
    while (nghttp2_session_send(session) == 0) {
        int got = connection_receive(connection, buffer, sizeof buffer);
        if (got <= 0 || nghttp2_session_mem_recv2(session, (const uint8_t *)buffer, (size_t)got) < 0) {
            break;
        }
    }
    nghttp2_session_del(session);
}

static void serve(Server *server, int client)
{
    Arena *arena = arena_create(0);
    Connection connection = { .fd = client };
    mbedtls_ssl_context tls;
    bool ready = true;
    if (server->tls != nullptr) {
        mbedtls_ssl_init(&tls);
        ready = mbedtls_ssl_setup(&tls, &server->tls->config) == 0;
        mbedtls_ssl_set_bio(&tls, &connection, tls_send, tls_receive, nullptr);
        ready = ready && mbedtls_ssl_handshake(&tls) == 0;
        connection.tls = &tls;
    }
    const char *protocol = ready && server->tls != nullptr ? mbedtls_ssl_get_alpn_protocol(&tls) : nullptr;
    if (ready && protocol != nullptr && strcmp(protocol, "h2") == 0) {
        serve_http2(&connection);
    } else if (ready) {
        connection_send(&connection, route(arena, read_request(arena, &connection)));
    }
    if (server->tls != nullptr) {
        mbedtls_ssl_close_notify(&tls);
        mbedtls_ssl_free(&tls);
    }
    net_close(client);
    arena_destroy(arena);
}

static void *accept_loop(void *argument)
{
    Server *server = argument;
    while (!atomic_load(&server->stopping)) {
        int client = -1;
        if (net_accept(server->listener, ACCEPT_POLL_MS, &client)) {
            (void)net_set_timeouts(client, 5000, nullptr);
            serve(server, client);
        }
    }
    return nullptr;
}

static const char *const HTTP2_PROTOCOLS[] = { "h2", nullptr };

// tls_server_create loads the server's certificate; with http2 it offers only HTTP/2.
static bool tls_server_create(Test *test, TlsServer *tls, bool http2)
{
    String certificate = { 0 };
    String key = { 0 };
    HttpClientConfig pin = { .ca_file = S(CA_FILE) };
    tls->pin = nullptr;
    if (file_read_all(test->arena, S(SERVER_CERT_FILE), &certificate, nullptr) != ERR_OK ||
        file_read_all(test->arena, S(SERVER_KEY_FILE), &key, nullptr) != ERR_OK ||
        http_client_create(&pin, &tls->pin, nullptr) != ERR_OK || psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    mbedtls_ssl_config_init(&tls->config);
    mbedtls_x509_crt_init(&tls->certificate);
    mbedtls_pk_init(&tls->key);
    // mbedTLS parses PEM including its terminating NUL, which file_read_all writes past the end.
    return mbedtls_x509_crt_parse(&tls->certificate, (const unsigned char *)certificate.data, certificate.len + 1) == 0 &&
           mbedtls_pk_parse_key(&tls->key, (const unsigned char *)key.data, key.len + 1, nullptr, 0) == 0 &&
           mbedtls_ssl_config_defaults(&tls->config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT) == 0 &&
           mbedtls_ssl_conf_own_cert(&tls->config, &tls->certificate, &tls->key) == 0 &&
           (!http2 || mbedtls_ssl_conf_alpn_protocols(&tls->config, HTTP2_PROTOCOLS) == 0);
}

static void tls_server_destroy(TlsServer *tls)
{
    mbedtls_ssl_config_free(&tls->config);
    mbedtls_x509_crt_free(&tls->certificate);
    mbedtls_pk_free(&tls->key);
    if (tls->pin != nullptr) {
        http_client_destroy(tls->pin);
    }
}

static bool server_start(Test *test, Server *server, TlsServer *tls)
{
    *server = (Server){ .tls = tls };
    atomic_init(&server->stopping, false);
    return net_listen(S("127.0.0.1:0"), &server->listener, nullptr) == ERR_OK &&
           net_local_address(test->arena, server->listener, &server->address, nullptr) == ERR_OK &&
           thread_start(&server->thread, accept_loop, server, nullptr) == ERR_OK;
}

static void server_stop(Server *server)
{
    atomic_store(&server->stopping, true);
    thread_join(&server->thread);
    net_close(server->listener);
}

static const String LOCAL_HOSTS[] = { { "127.0.0.1", 9 }, { "localhost", 9 } };

// https_url is path on the server under the name its certificate holds, localhost.
static String https_url(Test *test, const Server *server, const char *path)
{
    String host = { 0 };
    String port = { 0 };
    str_cut(server->address, ':', &host, &port);
    return str_format(test->arena, "https://localhost:%.*s%s", (int)port.len, port.data, path);
}

static HttpClient *local_client(Test *test, size_t max_response_bytes, int64_t timeout_ms)
{
    HttpClientConfig config = {
        .ca_file = S(CA_FILE),
        .user_agent = S("libmc-test/1.0"),
        .allowed_hosts = LOCAL_HOSTS,
        .allowed_host_count = countof(LOCAL_HOSTS),
        .timeout_ms = timeout_ms,
        .max_response_bytes = max_response_bytes,
        .allow_plain_http = true,
    };
    HttpClient *client = nullptr;
    Err err = { 0 };
    test_check(test, http_client_create(&config, &client, &err) == ERR_OK, err.msg);
    return client;
}

[[nodiscard]] static Error send_to(Test *test, HttpClient *client, const Server *server, const char *method, const char *path,
                  String body, HttpResponse *response, Err *err)
{
    const HttpHeader headers[] = { { S("Accept"), S("application/json") } };
    HttpRequest request = {
        .method = S(method),
        .url = str_format(test->arena, "http://%.*s%s", (int)server->address.len, server->address.data, path),
        .headers = headers,
        .header_count = countof(headers),
        .body = body,
    };
    return http_send(client, test->arena, &request, response, err);
}

static void http_sends_requests_and_reads_responses(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/echo", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200, "answers 200");
    test_check(test, str_starts_with(response.body, S("GET /echo HTTP/1.1")), "sends the request line");
    test_check(test, str_contains(response.body, S("User-Agent: libmc-test/1.0")), "sends the user agent");
    test_check(test, str_contains(response.body, S("Accept: application/json")), "sends the request's headers");
    String value = { 0 };
    test_check(test, http_response_header(&response, S("x-test"), &value) && str_equal(value, S("yes")),
               "finds a response header ignoring case");

    test_check(test, send_to(test, client, &server, "POST", "/echo", S("{\"a\":1}"), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_ends_with(response.body, S("\r\n\r\n{\"a\":1}")), "sends a POST body");
    test_check(test, send_to(test, client, &server, "GET", "/chunked", S(""), &response, &err) == ERR_OK, err.msg);
    test_check_str(test, response.body, S("hello world"), "decodes a chunked body");
    test_check(test, send_to(test, client, &server, "GET", "/unavailable", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 503 && http_response_header(&response, S("Retry-After"), &value) &&
                         str_equal(value, S("120")),
               "a 503 is an answer with its Retry-After");
    http_client_destroy(client);
    server_stop(&server);
}

static void http_follows_redirects_within_the_rules(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/found", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200 && str_ends_with(response.url, S("/echo")), "follows a 302");
    test_check(test, send_to(test, client, &server, "POST", "/see-other", S("body"), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("GET /echo")) && !str_contains(response.body, S("body")),
               "a 303 continues as GET without the body");
    test_check(test, send_to(test, client, &server, "POST", "/temporary", S("body"), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("POST /echo")) && str_ends_with(response.body, S("body")),
               "a 307 repeats the method and body");
    test_check(test, send_to(test, client, &server, "GET", "/loop", S(""), &response, &err) == ERR_NETWORK,
               "too many redirects");
    test_check(test, send_to(test, client, &server, "GET", "/elsewhere", S(""), &response, &err) == ERR_INVALID_ARGUMENT,
               "a redirect to a host that is not allowed");
    test_check(test, str_contains(S(err.msg), S("elsewhere.invalid")), "names the host");
    http_client_destroy(client);
    server_stop(&server);
}

static void http_enforces_limits(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 1024, 200);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/large", S(""), &response, &err) == ERR_UNSUPPORTED,
               "a body over the limit, by its Content-Length");
    test_check(test, send_to(test, client, &server, "GET", "/large-chunked", S(""), &response, &err) == ERR_UNSUPPORTED,
               "a chunked body that grows over the limit");
    test_check(test, send_to(test, client, &server, "GET", "/slow", S(""), &response, &err) == ERR_TIMEOUT,
               "a response slower than the timeout");
    test_check(test, send_to(test, client, &server, "BREW", "/echo", S(""), &response, &err) == ERR_INVALID_ARGUMENT,
               "an unsupported method");
    HttpRequest elsewhere = { .url = S("http://example.com/") };
    test_check(test, http_send(client, test->arena, &elsewhere, &response, &err) == ERR_INVALID_ARGUMENT,
               "a host that is not allowed");
    HttpRequest malformed = { .url = S("http://[::1") };
    test_check(test, http_send(client, test->arena, &malformed, &response, &err) == ERR_INVALID_ARGUMENT,
               "a malformed URL");
    const HttpHeader injected[] = { { S("X-A"), S("1\r\nX-B: 2") } };
    HttpRequest injection = { .url = S("http://127.0.0.1/"), .headers = injected, .header_count = 1 };
    test_check(test, http_send(client, test->arena, &injection, &response, &err) == ERR_INVALID_ARGUMENT,
               "a header value with a line break");
    http_client_destroy(client);
    server_stop(&server);

    HttpClientConfig https_only = { .ca_file = S(CA_FILE) };
    test_check(test, http_client_create(&https_only, &client, &err) == ERR_OK, err.msg);
    HttpRequest plain = { .url = S("http://127.0.0.1/") };
    test_check(test, http_send(client, test->arena, &plain, &response, &err) == ERR_INVALID_ARGUMENT,
               "plain http is refused unless allowed");
    http_client_destroy(client);
    HttpClientConfig missing = { .ca_file = S("tests/fixtures/tls/missing.pem") };
    test_check(test, http_client_create(&missing, &client, &err) == ERR_NOT_FOUND, "a missing CA bundle");
}

static void http_verifies_tls_certificates(Test *test)
{
    TlsServer tls;
    test_check(test, tls_server_create(test, &tls, false), "loads the server certificate");
    Server server;
    test_check(test, server_start(test, &server, &tls), "starts the TLS server");
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    HttpRequest trusted = { .url = https_url(test, &server, "/echo") };
    test_check(test, http_send(client, test->arena, &trusted, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200 && str_starts_with(response.body, S("GET /echo")),
               "talks TLS to a server the CA vouches for");
    http_client_destroy(client);

    // The server's own certificate is no trust anchor for itself: the CA that signed it is missing.
    HttpClientConfig untrusted = {
        .ca_file = S(SERVER_CERT_FILE),
        .allowed_hosts = LOCAL_HOSTS,
        .allowed_host_count = countof(LOCAL_HOSTS),
    };
    test_check(test, http_client_create(&untrusted, &client, &err) == ERR_OK, err.msg);
    test_check(test, http_send(client, test->arena, &trusted, &response, &err) == ERR_NETWORK,
               "refuses a server whose issuer is not trusted");
    http_client_destroy(client);
    server_stop(&server);
    tls_server_destroy(&tls);
}

static void http_speaks_http2_when_offered(Test *test)
{
    TlsServer tls;
    test_check(test, tls_server_create(test, &tls, true), "loads the server certificate");
    Server server;
    test_check(test, server_start(test, &server, &tls), "starts the HTTP/2 server");
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    HttpRequest request = { .url = https_url(test, &server, "/") };
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200, "answers 200");
    test_check_str(test, response.body, S(HTTP2_BODY), "reads a response over HTTP/2");
    http_client_destroy(client);
    server_stop(&server);
    tls_server_destroy(&tls);
}

static void http_decodes_compressed_responses(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 1024, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/echo", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_contains(response.body, S("Accept-Encoding: ")) && str_contains(response.body, S("gzip")),
               "asks for gzip");
    test_check(test, send_to(test, client, &server, "GET", "/gzip", S(""), &response, &err) == ERR_OK, err.msg);
    test_check_str(test, response.body, S("hello, gzip"), "decodes a gzip body");
    test_check(test, send_to(test, client, &server, "GET", "/gzip-large", S(""), &response, &err) == ERR_UNSUPPORTED,
               "a small gzip body that decodes to more than the limit");
    http_client_destroy(client);
    server_stop(&server);
}

// http_reaches_live_hosts runs only with LIBMC_LIVE_TESTS=1, since the other tests never use the network.
static void http_reaches_live_hosts(Test *test)
{
    String live = { 0 };
    if (!env_get(test->arena, S("LIBMC_LIVE_TESTS"), &live) || !str_equal(live, S("1"))) {
        return;
    }
    HttpClientConfig config = { .user_agent = S("libmc-test/1.0 (https://github.com/sangar/libmc)") };
    HttpClient *client = nullptr;
    Err err = { 0 };
    test_check(test, http_client_create(&config, &client, &err) == ERR_OK, err.msg);
    const char *const urls[] = {
        "https://api.nb.no/catalog/v1/items?q=isbn:9788205379576",
        "https://www.wikidata.org/entity/Q42",
        "https://openlibrary.org/isbn/9780140328721.json",
    };
    for (size_t i = 0; i < countof(urls); i++) {
        HttpResponse response = { 0 };
        HttpRequest request = { .url = S(urls[i]) };
        Error e = http_send(client, test->arena, &request, &response, &err);
        printf("  live %s: %s, status %d, %zu bytes, %.*s\n", urls[i], error_name(e), response.status,
               response.body.len, (int)response.url.len, response.url.data);
        test_check(test, e == ERR_OK && response.status == 200, urls[i]);
    }
    HttpResponse response = { 0 };
    HttpRequest wrong_host = { .url = S("https://wrong.host.badssl.com/") };
    test_check(test, http_send(client, test->arena, &wrong_host, &response, &err) == ERR_NETWORK,
               "refuses a certificate for another host");
    http_client_destroy(client);
}

const TestCase HTTP_TESTS[] = {
    { "http_sends_requests_and_reads_responses", http_sends_requests_and_reads_responses },
    { "http_follows_redirects_within_the_rules", http_follows_redirects_within_the_rules },
    { "http_enforces_limits", http_enforces_limits },
    { "http_verifies_tls_certificates", http_verifies_tls_certificates },
    { "http_speaks_http2_when_offered", http_speaks_http2_when_offered },
    { "http_decodes_compressed_responses", http_decodes_compressed_responses },
    { "http_reaches_live_hosts", http_reaches_live_hosts },
    { nullptr, nullptr },
};
