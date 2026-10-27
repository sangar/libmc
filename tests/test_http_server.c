#include "test.h"

#include "mc/encoding/gzip.h"
#include "mc/net/http.h"
#include "mc/net/http_server.h"
#include "mc/net/tls.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

#include <string.h>

// The tests run the server's handler without a socket through
// http_server_exchange, and on 127.0.0.1 against raw sockets and libmc's own
// HTTP client.

enum { SLOW_MS = 600, RECEIVE_TIMEOUT_MS = 3000, BIG_BYTES = 8 << 20, TLS_BIG_BYTES = 64 << 10, UPLOAD_BYTES = 64 << 10 };

// answer echoes the method, path and body; /slow sleeps first, /fail fails,
// /bad-header sets a field value holding a line break, /version answers
// the request's HTTP version, and /page a few KB of text.
[[nodiscard]] static Error answer(void *context, Arena *arena, const HttpServerRequest *request,
                                  HttpServerResponse *response, Err *err)
{
    if (str_equal(request->path, S("/slow"))) {
        clock_sleep_ms(SLOW_MS);
    }
    if (str_equal(request->path, S("/fail"))) {
        return err_set(err, ERR_IO, "failed");
    }
    response->headers = arena_push(arena, sizeof *response->headers);
    response->headers[0] = str_equal(request->path, S("/bad-header"))
                               ? (HttpHeader){ S("X-Injected"), S("a\r\nSet-Cookie: b") }
                               : (HttpHeader){ S("Content-Type"), S("text/plain; charset=utf-8") };
    response->header_count = 1;
    // /big answers more than a socket buffer holds, /tls-big more than a TLS record.
    size_t big_bytes = str_equal(request->path, S("/big")) ? BIG_BYTES : str_equal(request->path, S("/tls-big")) ? TLS_BIG_BYTES : 0;
    if (big_bytes > 0) {
        char *big = arena_push(arena, big_bytes);
        memset(big, 'x', big_bytes);
        response->body = (String){ big, big_bytes };
        return ERR_OK;
    }
    if (str_equal(request->path, S("/page"))) {
        response->body = str_repeat(arena, S("<p>blåbær og bringebær</p>\n"), 150);
        return ERR_OK;
    }
    if (str_equal(request->path, S("/version"))) {
        response->body = str_format(arena, "%u.%u", request->version_major, request->version_minor);
        return ERR_OK;
    }
    response->body = str_format(arena, "%.*s %.*s %.*s", (int)request->method.len, request->method.data,
                                (int)request->path.len, request->path.data, (int)request->body.len, request->body.data);
    return ERR_OK;
}

static String exchange_with(Test *test, const HttpCompression *compression, String request)
{
    HttpServerConfig config = { .handler = answer, .compression = compression };
    StringList pieces = { 0 };
    strlist_push(test->arena, &pieces, request);
    String reply = S("");
    Err err = { 0 };
    test_check(test, http_server_exchange(test->arena, &config, pieces, &reply, &err) == ERR_OK, "exchange");
    return reply;
}

static String exchange(Test *test, String request)
{
    return exchange_with(test, nullptr, request);
}

static const HttpCompression COMPRESSION = { .min_bytes = 1024, .jitter_bytes = 32, .skip_credentials = true };

// gzipped_body gives a reply's body decompressed, or empty when it is not gzip.
static String gzipped_body(Test *test, String reply)
{
    size_t end = 0;
    String body = { 0 };
    if (!str_contains(reply, S("Content-Encoding: gzip\r\n")) || !str_find(reply, S("\r\n\r\n"), &end) ||
        gzip_decompress(test->arena, str_slice(reply, end + 4, reply.len), 1 << 20, &body, nullptr) != ERR_OK) {
        return S("");
    }
    return body;
}

static void http_server_gzips_what_is_worth_it(Test *test)
{
    String page = str_repeat(test->arena, S("<p>blåbær og bringebær</p>\n"), 150);
    String accepted = exchange_with(test, &COMPRESSION, S("GET /page HTTP/1.1\r\nHost: h\r\nAccept-Encoding: br, gzip\r\n\r\n"));
    test_check_str(test, gzipped_body(test, accepted), page, "a page goes gzipped to a client that accepts it");
    test_check(test, str_contains(accepted, S("Vary: Accept-Encoding\r\n")), "and says it varies");
    static const char *const UNCOMPRESSED[] = {
        "GET /page HTTP/1.1\r\nHost: h\r\n\r\n",
        "GET /page HTTP/1.1\r\nHost: h\r\nAccept-Encoding: gzip;q=0, *\r\n\r\n",
        "GET /small HTTP/1.1\r\nHost: h\r\nAccept-Encoding: gzip\r\n\r\n",
        "GET /page HTTP/1.1\r\nHost: h\r\nAccept-Encoding: gzip\r\nCookie: session=1\r\n\r\n",
    };
    for (size_t i = 0; i < countof(UNCOMPRESSED); i++) {
        test_check(test, !str_contains(exchange_with(test, &COMPRESSION, S(UNCOMPRESSED[i])), S("Content-Encoding")),
                   UNCOMPRESSED[i]);
    }
    test_check(test, !str_contains(exchange(test, S("GET /page HTTP/1.1\r\nHost: h\r\nAccept-Encoding: gzip\r\n\r\n")),
                                   S("Content-Encoding")),
               "a server without compression sends as it is");
    size_t first = accepted.len;
    bool varies = false;
    for (int i = 0; i < 9 && !varies; i++) {
        varies = exchange_with(test, &COMPRESSION, S("GET /page HTTP/1.1\r\nHost: h\r\nAccept-Encoding: gzip\r\n\r\n")).len != first;
    }
    test_check(test, varies, "padding varies the size against BREACH");
}

static void http_server_answers_pipelined_requests_in_order(Test *test)
{
    String reply = exchange(test, S("GET /a HTTP/1.1\r\nHost: h\r\n\r\nPOST /b HTTP/1.1\r\nHost: h\r\n"
                                    "Content-Length: 8\r\n\r\nblåbær"));
    size_t first = 0;
    size_t second = 0;
    test_check(test, str_find(reply, S("GET /a "), &first) && str_find(reply, S("POST /b blåbær"), &second) &&
                         first < second,
               "both answered, in order");
    test_check(test, str_count_char(reply, '\n') > 0 && str_starts_with(reply, S("HTTP/1.1 200 OK\r\nDate: ")),
               "a status line and a date");
}

static void http_server_reports_the_http_version(Test *test)
{
    test_check(test, str_ends_with(exchange(test, S("GET /version HTTP/1.1\r\nHost: h\r\n\r\n")), S("\r\n\r\n1.1")),
               "HTTP/1.1");
    test_check(test, str_ends_with(exchange(test, S("GET /version HTTP/1.0\r\n\r\n")), S("\r\n\r\n1.0")), "HTTP/1.0");
}

static void http_server_refuses_ambiguous_requests(Test *test)
{
    static const struct {
        const char *request;
        const char *status;
    } CASES[] = {
        { "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", "400" },
        { "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", "400" },
        { "GET / HTTP/1.1\r\n\r\n", "400" },
        { "GET / HTTP/1.1\nHost: h\n\n", "400" },
        { "GET  / HTTP/1.1\r\nHost: h\r\n\r\n", "400" },
        { "GET http://h/ HTTP/1.1\r\nHost: h\r\n\r\n", "400" },
        { "CONNECT h:443 HTTP/1.1\r\nHost: h:443\r\n\r\n", "400" },
        { "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n", "501" },
        { "PROPFIND / HTTP/1.1\r\nHost: h\r\n\r\n", "501" },
        { "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2000000\r\n\r\n", "413" },
    };
    for (size_t i = 0; i < countof(CASES); i++) {
        String reply = exchange(test, S(CASES[i].request));
        String expected = str_format(test->arena, "HTTP/1.1 %s ", CASES[i].status);
        test_check(test, str_starts_with(reply, expected) && str_contains(reply, S("Connection: close")),
                   CASES[i].request);
    }
    String header = str_repeat(test->arena, S("a"), 9000);
    String reply = exchange(test, str_format(test->arena, "GET / HTTP/1.1\r\nHost: h\r\nX: %.*s\r\n\r\n",
                                             (int)header.len, header.data));
    test_check(test, str_starts_with(reply, S("HTTP/1.1 431 ")), "a header over the limit");
}

static void http_server_answers_500_for_failures(Test *test)
{
    test_check(test, str_starts_with(exchange(test, S("GET /fail HTTP/1.1\r\nHost: h\r\n\r\n")), S("HTTP/1.1 500 ")),
               "a failing handler");
    String injected = exchange(test, S("GET /bad-header HTTP/1.1\r\nHost: h\r\n\r\n"));
    test_check(test, str_starts_with(injected, S("HTTP/1.1 500 ")) && !str_contains(injected, S("Set-Cookie")),
               "a line break in a response field");
}

static void http_server_continues_and_answers_head(Test *test)
{
    String continued = exchange(test, S("POST /c HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                                        "Content-Length: 2\r\n\r\nhi"));
    test_check(test, str_starts_with(continued, S("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n")) &&
                         str_ends_with(continued, S("POST /c hi")),
               "100-continue comes before the response");
    String head = exchange(test, S("HEAD /æ HTTP/1.1\r\nHost: h\r\n\r\n"));
    test_check(test, str_starts_with(head, S("HTTP/1.1 400 ")), "a target that is not ASCII");
    head = exchange(test, S("HEAD /h HTTP/1.1\r\nHost: h\r\n\r\n"));
    test_check(test, str_contains(head, S("Content-Length: 8\r\n")) && str_ends_with(head, S("\r\n\r\n")),
               "HEAD has the length but no body");
}

static HttpServer *start_server(Test *test)
{
    HttpServerConfig config = {
        .address = S("127.0.0.1:0"),
        .limits = { .timeout_ms = 300 },
        .worker_count = 2,
        .handler = answer,
    };
    HttpServer *server = nullptr;
    Err err = { 0 };
    test_check(test, http_server_create(&config, &server, &err) == ERR_OK, "start a server");
    return server;
}

// get sends a GET for path on a new connection and returns the response, or
// empty when the server closed without one.
static String get(Arena *arena, uint16_t port, const char *path)
{
    int fd = -1;
    if (net_connect(S("127.0.0.1"), str_format(arena, "%u", port), RECEIVE_TIMEOUT_MS, &fd, nullptr) != ERR_OK) {
        return S("");
    }
    StringBuilder reply = str_builder_create(arena, 256);
    String request = str_format(arena, "GET %s HTTP/1.1\r\nHost: h\r\n\r\n", path);
    if (net_set_timeouts(fd, RECEIVE_TIMEOUT_MS, nullptr) == ERR_OK && net_send(fd, request, nullptr) == ERR_OK) {
        char buffer[1024];
        size_t got = 0;
        while (!str_contains(str_builder_finish(&reply), S(path)) &&
               net_receive(fd, buffer, sizeof buffer, &got, nullptr) == ERR_OK && got > 0) {
            str_builder_append(&reply, (String){ buffer, got });
        }
    }
    net_close(fd);
    return str_builder_finish(&reply);
}

static void http_server_serves_the_http_client(Test *test)
{
    HttpServer *server = start_server(test);
    if (server == nullptr) {
        return;
    }
    HttpClientConfig config = { .allow_plain_http = true };
    HttpClient *client = nullptr;
    Err err = { 0 };
    test_check(test, http_client_create(&config, &client, &err) == ERR_OK, "create a client");
    String url = str_format(test->arena, "http://127.0.0.1:%u/syltetøy", http_server_port(server));
    for (int i = 0; i < 2 && client != nullptr; i++) {
        HttpRequest request = { .method = S("POST"), .url = url, .body = S("blåbær og bringebær") };
        HttpResponse response = { 0 };
        test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK, err.msg);
        test_check_str(test, response.body, S("POST /syltet%C3%B8y blåbær og bringebær"), "the echo");
    }
    if (client != nullptr) {
        http_client_destroy(client);
    }
    http_server_destroy(server);
}

static void http_server_runs_slow_handlers_aside(Test *test)
{
    HttpServer *server = start_server(test);
    if (server == nullptr) {
        return;
    }
    uint16_t port = http_server_port(server);
    int fd = -1;
    Err err = { 0 };
    test_check(test, net_connect(S("127.0.0.1"), str_format(test->arena, "%u", port), 1000, &fd, &err) == ERR_OK,
               "connect a slow request");
    test_check(test, net_send(fd, S("GET /slow HTTP/1.1\r\nHost: h\r\n\r\n"), &err) == ERR_OK, "send it");
    clock_sleep_ms(100);
    int64_t start = clock_monotonic_ns();
    String fast = get(test->arena, port, "/fast");
    int64_t elapsed_ms = (clock_monotonic_ns() - start) / 1000000;
    test_check(test, str_starts_with(fast, S("HTTP/1.1 200 OK\r\n")) && elapsed_ms < SLOW_MS / 2,
               "a slow handler does not hold up another connection");
    net_close(fd);
    http_server_destroy(server);
}

static void http_server_closes_idle_clients(Test *test)
{
    HttpServer *server = start_server(test);
    if (server == nullptr) {
        return;
    }
    int fd = -1;
    Err err = { 0 };
    String port = str_format(test->arena, "%u", http_server_port(server));
    test_check(test, net_connect(S("127.0.0.1"), port, 1000, &fd, &err) == ERR_OK, "connect");
    char buffer[16];
    size_t got = 1;
    int64_t start = clock_monotonic_ns();
    bool closed = net_set_timeouts(fd, RECEIVE_TIMEOUT_MS, &err) == ERR_OK &&
                  net_receive(fd, buffer, sizeof buffer, &got, &err) == ERR_OK && got == 0;
    int64_t elapsed_ms = (clock_monotonic_ns() - start) / 1000000;
    test_check(test, closed && elapsed_ms < RECEIVE_TIMEOUT_MS, "a client that sends nothing is closed");
    net_close(fd);
    http_server_destroy(server);
}

static size_t body_length(String reply)
{
    size_t header_end = 0;
    return str_find(reply, S("\r\n\r\n"), &header_end) ? reply.len - header_end - 4 : 0;
}

static void http_server_sends_bodies_larger_than_the_socket_buffer(Test *test)
{
    HttpServer *server = start_server(test);
    if (server == nullptr) {
        return;
    }
    int fd = -1;
    Err err = { 0 };
    String port = str_format(test->arena, "%u", http_server_port(server));
    test_check(test, net_connect(S("127.0.0.1"), port, 1000, &fd, &err) == ERR_OK, "connect");
    StringBuilder reply = str_builder_create(test->arena, BIG_BYTES + 256);
    if (net_set_timeouts(fd, RECEIVE_TIMEOUT_MS, &err) == ERR_OK &&
        net_send(fd, S("GET /big HTTP/1.1\r\nHost: h\r\n\r\n"), &err) == ERR_OK) {
        char buffer[65536];
        size_t got = 0;
        while (body_length(str_builder_finish(&reply)) < BIG_BYTES &&
               net_receive(fd, buffer, sizeof buffer, &got, &err) == ERR_OK && got > 0) {
            str_builder_append(&reply, (String){ buffer, got });
        }
    }
    test_check(test, body_length(str_builder_finish(&reply)) == BIG_BYTES, "the whole body arrives");
    net_close(fd);
    http_server_destroy(server);
}

// Https is a server over TLS with a self-signed certificate for localhost,
// another such certificate, and files of both a client can trust.
typedef struct Https {
    String directory;
    String ca_file;
    String other_ca_file;
    String other_certificate;
    String other_key;
    TlsServer *tls;
    HttpServer *server;
} Https;

static bool https_start(Test *test, Https *https)
{
    String certificate = { 0 };
    String key = { 0 };
    Err err = { 0 };
    *https = (Https){ 0 };
    HttpServerConfig config = { .address = S("127.0.0.1:0"), .handler = answer, .worker_count = 2 };
    bool started = dir_create_temp(test->arena, S("libmc-https-"), &https->directory, &err) == ERR_OK &&
                   tls_self_signed(test->arena, S("localhost"), 1, &certificate, &key, &err) == ERR_OK &&
                   tls_self_signed(test->arena, S("localhost"), 1, &https->other_certificate, &https->other_key, &err) ==
                       ERR_OK;
    if (started) {
        https->ca_file = path_join(test->arena, https->directory, S("ca.pem"));
        https->other_ca_file = path_join(test->arena, https->directory, S("other-ca.pem"));
        started = file_write_all(https->ca_file, certificate, 0600, &err) == ERR_OK &&
                  file_write_all(https->other_ca_file, https->other_certificate, 0600, &err) == ERR_OK &&
                  tls_server_create(&(TlsServerConfig){ .certificate_pem = certificate, .key_pem = key }, &https->tls,
                                    &err) == ERR_OK;
    }
    config.tls = https->tls;
    started = started && http_server_create(&config, &https->server, &err) == ERR_OK;
    test_check(test, started, err.msg);
    return started;
}

static void https_stop(Https *https)
{
    if (https->server != nullptr) {
        http_server_destroy(https->server);
    }
    if (https->tls != nullptr) {
        tls_server_destroy(https->tls);
    }
    if (https->directory.len > 0) {
        unused(dir_remove_all(https->directory, nullptr));
    }
}

// https_client trusts ca_file. Each handshake costs much under the
// sanitizers, so a test keeps one client, whose connection is reused.
[[nodiscard]] static Error https_client(String ca_file, HttpClient **client, Err *err)
{
    static const String LOCALHOST[] = { { "localhost", 9 } };
    HttpClientConfig config = {
        .ca_file = ca_file,
        .allowed_hosts = LOCALHOST,
        .allowed_host_count = countof(LOCALHOST),
        .allow_plain_http = true,
        .max_response_bytes = BIG_BYTES + 256,
    };
    return http_client_create(&config, client, err);
}

// http_server_serves_https makes its certificates once, does three
// handshakes and moves a few TLS records each way, as gcc's sanitized
// cryptography is slow. libmc's client speaks HTTP/2, which TLS offers.
static void http_server_serves_https(Test *test)
{
    Https https;
    if (!https_start(test, &https)) {
        https_stop(&https);
        return;
    }
    uint16_t port = http_server_port(https.server);
    char *upload = arena_push(test->arena, UPLOAD_BYTES);
    memset(upload, 'u', UPLOAD_BYTES);
    HttpRequest big = { .url = str_format(test->arena, "https://localhost:%u/tls-big", port) };
    HttpRequest post = {
        .method = S("POST"),
        .url = str_format(test->arena, "https://localhost:%u/up", port),
        .body = (String){ upload, UPLOAD_BYTES },
    };
    HttpRequest plain = { .url = str_format(test->arena, "http://localhost:%u/", port) };
    HttpRequest version = { .url = str_format(test->arena, "https://localhost:%u/version", port) };
    HttpResponse response = { 0 };
    HttpClient *client = nullptr;
    HttpClient *other = nullptr;
    Err err = { 0 };
    test_check(test, https_client(https.ca_file, &client, &err) == ERR_OK, err.msg);
    test_check(test, https_client(https.other_ca_file, &other, &err) == ERR_OK, err.msg);
    if (client != nullptr && other != nullptr) {
        test_check(test, http_send(client, test->arena, &version, &response, &err) == ERR_OK, err.msg);
        test_check_str(test, response.body, S("2.0"), "a client that speaks HTTP/2 gets it over TLS");
        test_check(test, http_send(client, test->arena, &big, &response, &err) == ERR_OK, err.msg);
        test_check(test, response.body.len == TLS_BIG_BYTES, "more than a TLS record comes back");
        test_check(test, http_send(client, test->arena, &post, &response, &err) == ERR_OK, err.msg);
        test_check(test, response.body.len == S("POST /up ").len + UPLOAD_BYTES, "a large upload is read");
        test_check(test, http_send(client, test->arena, &plain, &response, nullptr) == ERR_NETWORK,
                   "plain HTTP to the TLS port fails");
        test_check(test, http_send(other, test->arena, &big, &response, nullptr) == ERR_NETWORK,
                   "a client trusting only the other certificate is refused");
        test_check(test, tls_server_set_certificate(https.tls, https.other_certificate, https.other_key, &err) == ERR_OK,
                   err.msg);
        test_check(test, http_send(other, test->arena, &post, &response, &err) == ERR_OK, err.msg);
        test_check(test, response.status == 200, "after swapping the certificate, the next handshake presents it");
    }
    if (client != nullptr) {
        http_client_destroy(client);
    }
    if (other != nullptr) {
        http_client_destroy(other);
    }
    https_stop(&https);
}

typedef struct SlowGet {
    Arena *arena;
    uint16_t port;
    String reply;
} SlowGet;

static void *get_slow(void *argument)
{
    SlowGet *slow = argument;
    slow->reply = get(slow->arena, slow->port, "/slow");
    return nullptr;
}

static void http_server_finishes_requests_when_stopped(Test *test)
{
    HttpServer *server = start_server(test);
    if (server == nullptr) {
        return;
    }
    SlowGet slow = { .arena = test->arena, .port = http_server_port(server) };
    Thread thread;
    Err err = { 0 };
    bool started = thread_start(&thread, get_slow, &slow, &err) == ERR_OK;
    test_check(test, started, "start a slow request");
    clock_sleep_ms(100);
    http_server_destroy(server);
    if (started) {
        thread_join(&thread);
    }
    test_check(test, str_starts_with(slow.reply, S("HTTP/1.1 200 OK\r\n")) && str_contains(slow.reply, S("Connection: close")),
               "the request in flight is answered, and the connection closed");
}

const TestCase HTTP_SERVER_TESTS[] = {
    { "http_server_answers_pipelined_requests_in_order", http_server_answers_pipelined_requests_in_order },
    { "http_server_reports_the_http_version", http_server_reports_the_http_version },
    { "http_server_refuses_ambiguous_requests", http_server_refuses_ambiguous_requests },
    { "http_server_gzips_what_is_worth_it", http_server_gzips_what_is_worth_it },
    { "http_server_answers_500_for_failures", http_server_answers_500_for_failures },
    { "http_server_continues_and_answers_head", http_server_continues_and_answers_head },
    { "http_server_serves_the_http_client", http_server_serves_the_http_client },
    { "http_server_runs_slow_handlers_aside", http_server_runs_slow_handlers_aside },
    { "http_server_sends_bodies_larger_than_the_socket_buffer", http_server_sends_bodies_larger_than_the_socket_buffer },
    { "http_server_closes_idle_clients", http_server_closes_idle_clients },
    { "http_server_serves_https", http_server_serves_https },
    { "http_server_finishes_requests_when_stopped", http_server_finishes_requests_when_stopped },
    { nullptr, nullptr },
};
