#include "test.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nghttp2/nghttp2.h"
#include "psa/crypto.h"

#include "mc/net/http.h"
#include "mc/net/smtp.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

// The tests run the clients against a server on 127.0.0.1 in a thread of this
// process, over plain HTTP or TLS with the certificates in tests/fixtures/tls,
// which a test CA there signed for localhost and 127.0.0.1. Over TLS it can
// offer HTTP/2, which it answers through nghttp2, and it can take mail over
// STARTTLS or TLS from the start. Set LIBMC_LIVE_TESTS=1 to
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

// SmtpServer takes one mail session at a time: over TLS from the start when
// implicit_tls, else in the clear, offering STARTTLS when starttls. It keeps
// what it received in arena, for a test to read once server_stop has joined it.
typedef struct SmtpServer {
    bool implicit_tls;
    bool starttls;
    Arena *arena;
    String auth;
    String mail_from;
    StringList recipients;
    String data;
} SmtpServer;

typedef struct Server {
    int listener;
    String address;
    TlsServer *tls;
    // smtp makes the server take mail instead of answering HTTP.
    SmtpServer *smtp;
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

// tls_accept runs the server's side of a TLS handshake; the connection then reads and writes through tls.
static bool tls_accept(TlsServer *server_tls, Connection *connection, mbedtls_ssl_context *tls)
{
    mbedtls_ssl_init(tls);
    bool ready = mbedtls_ssl_setup(tls, &server_tls->config) == 0;
    mbedtls_ssl_set_bio(tls, connection, tls_send, tls_receive, nullptr);
    if (!ready || mbedtls_ssl_handshake(tls) != 0) {
        mbedtls_ssl_free(tls);
        return false;
    }
    connection->tls = tls;
    return true;
}

static void tls_end(Connection *connection)
{
    if (connection->tls != nullptr) {
        mbedtls_ssl_close_notify(connection->tls);
        mbedtls_ssl_free(connection->tls);
        connection->tls = nullptr;
    }
}

static void serve(Server *server, int client)
{
    Arena *arena = arena_create(0);
    Connection connection = { .fd = client };
    mbedtls_ssl_context tls;
    bool ready = server->tls == nullptr || tls_accept(server->tls, &connection, &tls);
    const char *protocol = connection.tls != nullptr ? mbedtls_ssl_get_alpn_protocol(&tls) : nullptr;
    if (ready && protocol != nullptr && strcmp(protocol, "h2") == 0) {
        serve_http2(&connection);
    } else if (ready) {
        connection_send(&connection, route(arena, read_request(arena, &connection)));
    }
    tls_end(&connection);
    net_close(client);
    arena_destroy(arena);
}

// LineReader reads CRLF-terminated lines from a connection.
typedef struct LineReader {
    Connection *connection;
    char buffer[4096];
    size_t start;
    size_t end;
} LineReader;

static bool read_line(LineReader *reader, Arena *arena, String *line)
{
    StringBuilder text = str_builder_create(arena, 128);
    for (;;) {
        while (reader->start < reader->end) {
            char c = reader->buffer[reader->start++];
            if (c == '\n') {
                *line = str_trim_suffix(str_builder_finish(&text), S("\r"));
                return true;
            }
            str_builder_append_char(&text, c);
        }
        int got = connection_receive(reader->connection, reader->buffer, sizeof reader->buffer);
        if (got <= 0) {
            return false;
        }
        reader->start = 0;
        reader->end = (size_t)got;
    }
}

static void smtp_reply(Connection *connection, const char *reply)
{
    connection_send(connection, S(reply));
}

// read_data reads a message up to the line ".", undoing the dot stuffing.
static String read_data(LineReader *reader, Arena *arena)
{
    StringBuilder data = str_builder_create(arena, 4096);
    String line = { 0 };
    while (read_line(reader, arena, &line) && !str_equal(line, S("."))) {
        str_builder_append(&data, str_trim_prefix(line, S(".")));
        str_builder_append(&data, S("\r\n"));
    }
    return str_builder_finish(&data);
}

// serve_smtp answers one session; AUTH is offered over TLS only, and a recipient containing "refused" is refused.
static void serve_smtp(Server *server, int client)
{
    SmtpServer *smtp = server->smtp;
    Arena *arena = smtp->arena;
    Connection connection = { .fd = client };
    mbedtls_ssl_context tls;
    LineReader reader = { .connection = &connection };
    bool ready = !smtp->implicit_tls || tls_accept(server->tls, &connection, &tls);
    if (ready) {
        smtp_reply(&connection, "220 libmc-test ESMTP\r\n");
    }
    String line = { 0 };
    while (ready && read_line(&reader, arena, &line)) {
        bool secure = connection.tls != nullptr;
        if (str_starts_with(line, S("EHLO "))) {
            smtp_reply(&connection, "250-libmc-test\r\n");
            if (secure) {
                smtp_reply(&connection, "250-AUTH PLAIN\r\n");
            } else if (smtp->starttls) {
                smtp_reply(&connection, "250-STARTTLS\r\n");
            }
            smtp_reply(&connection, "250 8BITMIME\r\n");
        } else if (str_equal(line, S("STARTTLS")) && !secure && smtp->starttls) {
            smtp_reply(&connection, "220 2.0.0 ready\r\n");
            ready = tls_accept(server->tls, &connection, &tls);
            reader.start = reader.end = 0;
        } else if (str_starts_with(line, S("AUTH PLAIN")) && secure) {
            smtp->auth = str_trim(str_trim_prefix(line, S("AUTH PLAIN")));
            if (smtp->auth.len == 0) {
                smtp_reply(&connection, "334 \r\n");
                ready = read_line(&reader, arena, &smtp->auth);
            }
            smtp_reply(&connection, "235 2.7.0 accepted\r\n");
        } else if (str_starts_with(line, S("MAIL FROM:"))) {
            smtp->mail_from = str_trim_prefix(line, S("MAIL FROM:"));
            smtp_reply(&connection, "250 2.1.0 ok\r\n");
        } else if (str_starts_with(line, S("RCPT TO:"))) {
            String recipient = str_trim_prefix(line, S("RCPT TO:"));
            if (str_contains(recipient, S("refused"))) {
                smtp_reply(&connection, "550 5.1.1 no such user\r\n");
            } else {
                strlist_push(arena, &smtp->recipients, recipient);
                smtp_reply(&connection, "250 2.1.5 ok\r\n");
            }
        } else if (str_equal(line, S("DATA"))) {
            smtp_reply(&connection, "354 end with a line holding a dot\r\n");
            smtp->data = read_data(&reader, arena);
            smtp_reply(&connection, "250 2.0.0 queued\r\n");
        } else if (str_equal(line, S("QUIT"))) {
            smtp_reply(&connection, "221 2.0.0 bye\r\n");
            break;
        } else {
            smtp_reply(&connection, "502 5.5.2 not implemented\r\n");
        }
    }
    tls_end(&connection);
    net_close(client);
}

static void *accept_loop(void *argument)
{
    Server *server = argument;
    while (!atomic_load(&server->stopping)) {
        int client = -1;
        if (net_accept(server->listener, ACCEPT_POLL_MS, &client)) {
            (void)net_set_timeouts(client, 5000, nullptr);
            if (server->smtp != nullptr) {
                serve_smtp(server, client);
            } else {
                serve(server, client);
            }
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

static bool smtp_server_start(Test *test, Server *server, TlsServer *tls, SmtpServer *smtp)
{
    *server = (Server){ .tls = tls, .smtp = smtp };
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

// local_url is path on the server under the name its certificate holds, localhost.
static String local_url(Test *test, const Server *server, const char *scheme, const char *path)
{
    String host = { 0 };
    String port = { 0 };
    str_cut(server->address, ':', &host, &port);
    return str_format(test->arena, "%s://localhost:%.*s%s", scheme, (int)port.len, port.data, path);
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
    HttpRequest trusted = { .url = local_url(test, &server, "https", "/echo") };
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
    HttpRequest request = { .url = local_url(test, &server, "https", "/") };
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

static String file_contents(Test *test, String path)
{
    String contents = { 0 };
    return file_read_all(test->arena, path, &contents, nullptr) == ERR_OK ? contents : S("<unreadable>");
}

static void http_streams_bodies_through_files(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 1024, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-http-"), &dir, &err) == ERR_OK, err.msg);
    String source_path = path_join(test->arena, dir, S("source"));
    test_check(test, file_write_all(source_path, S("0123456789"), 0600, &err) == ERR_OK, err.msg);
    int source = -1;
    test_check(test, file_open_read(source_path, &source, &err) == ERR_OK, err.msg);

    HttpFileBody part = { .fd = source, .offset = 2, .length = 5 };
    HttpRequest put = { .method = S("PUT"), .url = str_format(test->arena, "http://%.*s/echo", (int)server.address.len,
                                                             server.address.data),
                        .body_file = &part };
    test_check(test, http_send(client, test->arena, &put, &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("PUT /echo")), "sends the method");
    test_check(test, str_contains(response.body, S("Content-Length: 5")), "declares the range's length");
    test_check(test, str_ends_with(response.body, S("\r\n\r\n23456")), "sends the range of the file");

    put.method = S("POST");
    put.url = str_format(test->arena, "http://%.*s/temporary", (int)server.address.len, server.address.data);
    test_check(test, http_send(client, test->arena, &put, &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("POST /echo")) && str_ends_with(response.body, S("23456")),
               "a 307 sends the file again");

    String sink_path = path_join(test->arena, dir, S("sink"));
    int sink = -1;
    test_check(test, file_create(sink_path, 0600, &sink, &err) == ERR_OK, err.msg);
    HttpDownload download = { .fd = sink };
    HttpRequest get = { .url = str_format(test->arena, "http://%.*s/large", (int)server.address.len, server.address.data),
                        .download = &download };
    test_check(test, http_send(client, test->arena, &get, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200 && response.body.len == 0 && response.downloaded == 4096,
               "a download past max_response_bytes goes to the file");
    test_check_str(test, file_contents(test, sink_path), str_repeat(test->arena, S("x"), 4096), "the file holds the body");

    test_check(test, file_truncate(sink, 0, &err) == ERR_OK && file_seek(sink, 0, &err) == ERR_OK, err.msg);
    get.url = str_format(test->arena, "http://%.*s/unavailable", (int)server.address.len, server.address.data);
    test_check(test, http_send(client, test->arena, &get, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 503 && str_equal(response.body, S("try later")) && response.downloaded == 0,
               "an error status keeps its body in memory");
    test_check_str(test, file_contents(test, sink_path), S(""), "and leaves the file alone");

    file_close(sink);
    file_close(source);
    (void)file_remove(sink_path, nullptr);
    (void)file_remove(source_path, nullptr);
    (void)dir_remove(dir, nullptr);
    http_client_destroy(client);
    server_stop(&server);
}

typedef struct DelayedCancel {
    Cancel *cancel;
    int64_t delay_ms;
} DelayedCancel;

static void *cancel_later(void *argument)
{
    DelayedCancel *delayed = argument;
    clock_sleep_ms(delayed->delay_ms);
    cancel_request(delayed->cancel);
    return nullptr;
}

static void http_cancels_and_times_out_per_request(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 0, 200);
    HttpResponse response = { 0 };
    Err err = { 0 };
    String slow = str_format(test->arena, "http://%.*s/slow", (int)server.address.len, server.address.data);
    HttpRequest request = { .url = slow, .timeout_ms = 5000 };
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK && response.status == 200,
               "a request's own timeout outlasts the client's");
    request.timeout_ms = -1;
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK, "a request without a timeout");

    Cancel cancel;
    cancel_init(&cancel);
    request.cancel = &cancel;
    DelayedCancel delayed = { .cancel = &cancel, .delay_ms = 100 };
    Thread canceller;
    test_check(test, thread_start(&canceller, cancel_later, &delayed, nullptr) == ERR_OK, "starts the canceller");
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_CANCELLED, "cancelled mid-request");
    thread_join(&canceller);
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_CANCELLED,
               "a request on a cancelled token does not start");
    cancel_destroy(&cancel);

    // The server gzips /gzip whatever is asked; a caller that sets Accept-Encoding gets those bytes as sent.
    const HttpHeader identity[] = { { S("Accept-Encoding"), S("identity") } };
    HttpRequest raw = { .url = str_format(test->arena, "http://%.*s/gzip", (int)server.address.len, server.address.data),
                        .headers = identity,
                        .header_count = countof(identity) };
    test_check(test, http_send(client, test->arena, &raw, &response, &err) == ERR_OK, err.msg);
    test_check(test, str_equal(response.body, (String){ (const char *)GZIP_HELLO, sizeof GZIP_HELLO }),
               "a body the caller did not ask to decode stays encoded");
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

static MailMessage short_message(const MailAddress *to)
{
    return (MailMessage){
        .from = { .address = S("nora@example.no") },
        .to = to,
        .to_count = 1,
        .subject = S("Et dukkehjem"),
        .text = S("Hei"),
    };
}

static void smtp_sends_mail_with_starttls(Test *test)
{
    TlsServer tls;
    test_check(test, tls_server_create(test, &tls, false), "loads the server certificate");
    SmtpServer smtp = { .starttls = true, .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, &tls, &smtp), "starts the mail server");
    SmtpConfig config = {
        .url = local_url(test, &server, "smtp", ""),
        .username = S("nora"),
        .password = S("dukkehjem"),
        .ca_file = S(CA_FILE),
    };
    const MailAddress to[] = { { S("Torvald Helmer"), S("torvald@example.no") }, { .address = S("kristine@example.no") } };
    const MailAddress cc[] = { { S("Dr. Rank"), S("rank@example.no") } };
    const MailAddress bcc[] = { { .address = S("krogstad@example.no") } };
    const MailAttachment attachments[] = {
        { S("brev.txt"), S("text/plain"), S("Hello, PDF") },
        { .filename = S("søknad.pdf"), .data = S("%PDF") },
    };
    MailMessage message = {
        .from = { S("Nora Helmer"), S("nora@example.no") },
        .to = to,
        .to_count = countof(to),
        .cc = cc,
        .cc_count = countof(cc),
        .bcc = bcc,
        .bcc_count = countof(bcc),
        .subject = S("Søknad om ferie"),
        .text = S("Hei\nlinje to"),
        .html = S("<p>Hei</p>"),
        .attachments = attachments,
        .attachment_count = countof(attachments),
    };
    Err err = { 0 };
    test_check(test, smtp_send(&config, &message, &err) == ERR_OK, err.msg);
    server_stop(&server);
    String data = smtp.data;
    test_check_str(test, smtp.auth, S("AG5vcmEAZHVra2VoamVt"), "signs in with PLAIN after STARTTLS");
    test_check_str(test, smtp.mail_from, S("<nora@example.no>"), "the envelope sender");
    test_check_str(test, str_join(test->arena, smtp.recipients, S(" ")),
                   S("<torvald@example.no> <kristine@example.no> <rank@example.no> <krogstad@example.no>"),
                   "every recipient, bcc included");
    test_check(test, str_contains(data, S("From: \"Nora Helmer\" <nora@example.no>\r\n")), "the From header");
    test_check(test, str_contains(data, S("To: \"Torvald Helmer\" <torvald@example.no>,\r\n kristine@example.no\r\n")),
               "the To header, folded between mailboxes");
    test_check(test, str_contains(data, S("Cc: \"Dr. Rank\" <rank@example.no>\r\n")), "the Cc header");
    test_check(test, !str_contains(data, S("krogstad")), "bcc is not named in the message");
    test_check(test, str_contains(data, S("Subject: =?UTF-8?Q?S=C3=B8knad_om_ferie?=\r\n")), "a UTF-8 subject");
    test_check(test, str_contains(data, S("Date: ")) && str_contains(data, S("Message-ID: <")), "Date and Message-ID");
    test_check(test, str_contains(data, S("multipart/alternative")) && str_contains(data, S("Hei\r\nlinje to\r\n")) &&
                         str_contains(data, S("<p>Hei</p>")),
               "the text and HTML as alternatives");
    test_check(test, str_contains(data, S("filename=\"brev.txt\"")) && str_contains(data, S("SGVsbG8sIFBERg==")),
               "an attachment in base64");
    test_check(test, str_contains(data, S("filename*=UTF-8''s%C3%B8knad.pdf")), "a UTF-8 file name");
    tls_server_destroy(&tls);
    arena_destroy(smtp.arena);
}

static void smtp_sends_mail_over_tls_from_the_start(Test *test)
{
    TlsServer tls;
    test_check(test, tls_server_create(test, &tls, false), "loads the server certificate");
    SmtpServer smtp = { .implicit_tls = true, .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, &tls, &smtp), "starts the mail server");
    SmtpConfig config = { .url = local_url(test, &server, "smtps", ""), .ca_file = S(CA_FILE) };
    const MailAddress to[] = { { .address = S("torvald@example.no") } };
    MailMessage message = short_message(to);
    Err err = { 0 };
    test_check(test, smtp_send(&config, &message, &err) == ERR_OK, err.msg);
    server_stop(&server);
    test_check(test, str_contains(smtp.data, S("Subject: Et dukkehjem\r\n")), "delivers the message");
    tls_server_destroy(&tls);
    arena_destroy(smtp.arena);
}

static void smtp_refuses_servers_without_starttls(Test *test)
{
    TlsServer tls;
    test_check(test, tls_server_create(test, &tls, false), "loads the server certificate");
    SmtpServer smtp = { .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, &tls, &smtp), "starts the mail server");
    SmtpConfig config = {
        .url = local_url(test, &server, "smtp", ""),
        .username = S("nora"),
        .password = S("dukkehjem"),
        .ca_file = S(CA_FILE),
    };
    const MailAddress to[] = { { .address = S("torvald@example.no") } };
    MailMessage message = short_message(to);
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_NETWORK, "a server that offers no STARTTLS");
    server_stop(&server);
    test_check(test, smtp.auth.len == 0 && smtp.mail_from.len == 0, "sends neither the password nor the mail");
    tls_server_destroy(&tls);
    arena_destroy(smtp.arena);
}

static void smtp_reports_a_refused_recipient(Test *test)
{
    TlsServer tls;
    test_check(test, tls_server_create(test, &tls, false), "loads the server certificate");
    SmtpServer smtp = { .starttls = true, .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, &tls, &smtp), "starts the mail server");
    SmtpConfig config = { .url = local_url(test, &server, "smtp", ""), .ca_file = S(CA_FILE) };
    const MailAddress to[] = { { .address = S("refused@example.no") } };
    MailMessage message = short_message(to);
    Err err = { 0 };
    test_check(test, smtp_send(&config, &message, &err) == ERR_NETWORK, "the server refuses the recipient");
    test_check(test, str_contains(S(err.msg), S("550")), err.msg);
    server_stop(&server);
    tls_server_destroy(&tls);
    arena_destroy(smtp.arena);
}

static void smtp_rejects_malformed_messages(Test *test)
{
    SmtpConfig config = { .url = S("smtp://localhost:1"), .ca_file = S(CA_FILE) };
    const MailAddress to[] = { { .address = S("torvald@example.no") } };
    MailMessage message = short_message(to);
    message.subject = S("Hei\r\nBcc: krogstad@example.no");
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_INVALID_ARGUMENT, "a line break in the subject");
    message = short_message(to);
    message.from.address = S("nora");
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_INVALID_ARGUMENT, "an address without @");
    message = short_message(to);
    message.to_count = 0;
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_INVALID_ARGUMENT, "no recipients");
    message = short_message(to);
    SmtpConfig http = { .url = S("http://localhost:1/"), .ca_file = S(CA_FILE) };
    test_check(test, smtp_send(&http, &message, nullptr) == ERR_INVALID_ARGUMENT, "a URL that is not SMTP");
    SmtpConfig missing = { .url = S("smtp://localhost:1"), .ca_file = S("tests/fixtures/tls/missing.pem") };
    test_check(test, smtp_send(&missing, &message, nullptr) == ERR_NOT_FOUND, "a missing CA bundle");
}

const TestCase HTTP_TESTS[] = {
    { "http_sends_requests_and_reads_responses", http_sends_requests_and_reads_responses },
    { "http_follows_redirects_within_the_rules", http_follows_redirects_within_the_rules },
    { "http_enforces_limits", http_enforces_limits },
    { "http_verifies_tls_certificates", http_verifies_tls_certificates },
    { "http_speaks_http2_when_offered", http_speaks_http2_when_offered },
    { "http_decodes_compressed_responses", http_decodes_compressed_responses },
    { "http_streams_bodies_through_files", http_streams_bodies_through_files },
    { "http_cancels_and_times_out_per_request", http_cancels_and_times_out_per_request },
    { "http_reaches_live_hosts", http_reaches_live_hosts },
    { nullptr, nullptr },
};

const TestCase SMTP_TESTS[] = {
    { "smtp_sends_mail_with_starttls", smtp_sends_mail_with_starttls },
    { "smtp_sends_mail_over_tls_from_the_start", smtp_sends_mail_over_tls_from_the_start },
    { "smtp_refuses_servers_without_starttls", smtp_refuses_servers_without_starttls },
    { "smtp_reports_a_refused_recipient", smtp_reports_a_refused_recipient },
    { "smtp_rejects_malformed_messages", smtp_rejects_malformed_messages },
    { nullptr, nullptr },
};
