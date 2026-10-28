#include "test.h"

#include "mc/crypto/sha256.h"
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

enum {
    SLOW_MS = 600,
    RECEIVE_TIMEOUT_MS = 3000,
    BIG_BYTES = 8 << 20,
    TLS_BIG_BYTES = 64 << 10,
    UPLOAD_BYTES = 64 << 10,
    // More than socket buffers hold, so the client is still sending when the server answers.
    UPLOAD_REFUSED_BYTES = 4 << 20,
    // Streamed bodies pass through 64 KiB pipes and HTTP/2's 64 KiB windows.
    TLS_STREAM_BYTES = 200 << 10,
    READ_PIECE_BYTES = 4096,
    WRITE_PIECE_BYTES = 10000,
};

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

// hash_body answers the SHA-256 of the body, read a piece at a time.
[[nodiscard]] static Error hash_body(HttpExchange *exchange, Err *err)
{
    Sha256 hash;
    sha256_init(&hash);
    char buffer[READ_PIECE_BYTES];
    size_t got = 0;
    Error e = ERR_OK;
    do {
        e = http_exchange_read(exchange, buffer, sizeof buffer, &got, err);
        sha256_update(&hash, buffer, got);
    } while (e == ERR_OK && got > 0);
    uint8_t digest[SHA256_LEN];
    sha256_final(&hash, digest);
    String hex = hex_encode(http_exchange_arena(exchange), digest, SHA256_LEN);
    if (e == ERR_OK) {
        e = http_exchange_start(exchange, 200, nullptr, 0, (int64_t)hex.len, err);
    }
    return e == ERR_OK ? http_exchange_write(exchange, hex, err) : e;
}

// stream_answer streams: POST /hash answers the body's SHA-256, /download?n
// sends n bytes in pieces with their length announced and /chunked?n without,
// and /refuse answers 417 with the body unread.
[[nodiscard]] static Error stream_answer(void *context, HttpExchange *exchange, Err *err)
{
    const HttpServerRequest *request = http_exchange_request(exchange);
    if (str_equal(request->path, S("/refuse"))) {
        return http_exchange_start(exchange, 417, nullptr, 0, 0, err);
    }
    if (str_equal(request->path, S("/hash"))) {
        return hash_body(exchange, err);
    }
    uint64_t total = 0;
    if (!str_parse_u64(request->query, &total)) {
        return http_exchange_start(exchange, 404, nullptr, 0, 0, err);
    }
    char piece[WRITE_PIECE_BYTES];
    memset(piece, 'd', sizeof piece);
    bool announced = str_equal(request->path, S("/download"));
    Error e = http_exchange_start(exchange, 200, nullptr, 0, announced ? (int64_t)total : -1, err);
    for (uint64_t sent = 0; e == ERR_OK && sent < total; sent += sizeof piece) {
        size_t count = total - sent < sizeof piece ? (size_t)(total - sent) : sizeof piece;
        e = http_exchange_write(exchange, (String){ piece, count }, err);
    }
    return e;
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

static String exchange_request(Test *test, String request)
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
    test_check(test, !str_contains(exchange_request(test, S("GET /page HTTP/1.1\r\nHost: h\r\nAccept-Encoding: gzip\r\n\r\n")),
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
    String reply = exchange_request(test, S("GET /a HTTP/1.1\r\nHost: h\r\n\r\nPOST /b HTTP/1.1\r\nHost: h\r\n"
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
    test_check(test, str_ends_with(exchange_request(test, S("GET /version HTTP/1.1\r\nHost: h\r\n\r\n")), S("\r\n\r\n1.1")),
               "HTTP/1.1");
    test_check(test, str_ends_with(exchange_request(test, S("GET /version HTTP/1.0\r\n\r\n")), S("\r\n\r\n1.0")), "HTTP/1.0");
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
        String reply = exchange_request(test, S(CASES[i].request));
        String expected = str_format(test->arena, "HTTP/1.1 %s ", CASES[i].status);
        test_check(test, str_starts_with(reply, expected) && str_contains(reply, S("Connection: close")),
                   CASES[i].request);
    }
    String header = str_repeat(test->arena, S("a"), 9000);
    String reply = exchange_request(test, str_format(test->arena, "GET / HTTP/1.1\r\nHost: h\r\nX: %.*s\r\n\r\n",
                                             (int)header.len, header.data));
    test_check(test, str_starts_with(reply, S("HTTP/1.1 431 ")), "a header over the limit");
}

static void http_fields_read_and_write(Test *test)
{
    Arena *a = test->arena;
    test_check_str(test, http_date(a, 784111777LL * NS_PER_SECOND), S("Sun, 06 Nov 1994 08:49:37 GMT"),
                   "RFC 9110's example date");
    test_check_str(test, http_date(a, -1), S("Wed, 31 Dec 1969 23:59:59 GMT"), "a moment before 1970");
    const HttpHeader headers[] = { { S("Content-Type"), S("text/plain") }, { S("X-A"), S("1") }, { S("x-a"), S("2") } };
    String value = S("");
    test_check(test, http_header_find(headers, countof(headers), S("x-A"), &value) && str_equal(value, S("1")),
               "the first field of a name in any case");
    test_check(test, !http_header_find(headers, countof(headers), S("X-B"), &value), "a missing field");

    String query = S("prefix=bl%C3%A5+b%C3%A6r&uploads&max-keys=10&100%=x");
    test_check(test, http_query_param(a, query, S("prefix"), &value) && str_equal(value, S("blå bær")),
               "a value decoded, '+' a space");
    test_check(test, http_query_param(a, query, S("uploads"), &value) && value.len == 0, "a parameter with no value");
    test_check(test, http_query_param(a, query, S("100%"), &value) && str_equal(value, S("x")),
               "a '%' that begins no escape stays");
    test_check(test, !http_query_param(a, query, S("max"), &value) && !http_query_param(a, S(""), S("a"), &value),
               "missing parameters");

    int64_t first = 0;
    int64_t last = 0;
    test_check(test, http_parse_range(S("bytes=0-4"), 11, &first, &last) == HTTP_RANGE_SATISFIABLE && first == 0 && last == 4,
               "the first five bytes");
    test_check(test, http_parse_range(S("bytes=6-"), 11, &first, &last) == HTTP_RANGE_SATISFIABLE && first == 6 && last == 10,
               "an open end");
    test_check(test, http_parse_range(S("bytes=-5"), 11, &first, &last) == HTTP_RANGE_SATISFIABLE && first == 6 && last == 10,
               "a suffix");
    test_check(test, http_parse_range(S("bytes=-50"), 11, &first, &last) == HTTP_RANGE_SATISFIABLE && first == 0 && last == 10,
               "a suffix longer than the body");
    test_check(test, http_parse_range(S("bytes=5-100"), 11, &first, &last) == HTTP_RANGE_SATISFIABLE && last == 10,
               "an end past the body is cut");
    test_check(test, http_parse_range(S("bytes=11-"), 11, &first, &last) == HTTP_RANGE_UNSATISFIABLE, "a start past the end");
    test_check(test, http_parse_range(S("bytes=-0"), 11, &first, &last) == HTTP_RANGE_UNSATISFIABLE, "an empty suffix");
    test_check(test, http_parse_range(S("bytes=0-1,4-5"), 11, &first, &last) == HTTP_RANGE_NONE, "several ranges");
    test_check(test, http_parse_range(S("items=0-1"), 11, &first, &last) == HTTP_RANGE_NONE, "another unit");
    test_check(test, http_parse_range(S("bytes=4-1"), 11, &first, &last) == HTTP_RANGE_NONE, "an end before the start");
}

static void http_server_answers_500_for_failures(Test *test)
{
    test_check(test, str_starts_with(exchange_request(test, S("GET /fail HTTP/1.1\r\nHost: h\r\n\r\n")), S("HTTP/1.1 500 ")),
               "a failing handler");
    String injected = exchange_request(test, S("GET /bad-header HTTP/1.1\r\nHost: h\r\n\r\n"));
    test_check(test, str_starts_with(injected, S("HTTP/1.1 500 ")) && !str_contains(injected, S("Set-Cookie")),
               "a line break in a response field");
}

static void http_server_continues_and_answers_head(Test *test)
{
    String continued = exchange_request(test, S("POST /c HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                                        "Content-Length: 2\r\n\r\nhi"));
    test_check(test, str_starts_with(continued, S("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n")) &&
                         str_ends_with(continued, S("POST /c hi")),
               "100-continue comes before the response");
    String head = exchange_request(test, S("HEAD /æ HTTP/1.1\r\nHost: h\r\n\r\n"));
    test_check(test, str_starts_with(head, S("HTTP/1.1 400 ")), "a target that is not ASCII");
    head = exchange_request(test, S("HEAD /h HTTP/1.1\r\nHost: h\r\n\r\n"));
    test_check(test, str_contains(head, S("Content-Length: 8\r\n")) && str_ends_with(head, S("\r\n\r\n")),
               "HEAD has the length but no body");
}

static void http_server_exchanges_http2_with_prior_knowledge(Test *test)
{
    // The preface, empty SETTINGS, and HEADERS for GET / on stream 1 that end
    // it: :method GET, :scheme http and :path / from HPACK's static table,
    // and :authority h.
    static const char REQUEST[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
                                  "\x00\x00\x00\x04\x00\x00\x00\x00\x00"
                                  "\x00\x00\x06\x01\x05\x00\x00\x00\x01\x82\x86\x84\x41\x01h";
    String reply = exchange_request(test, (String){ REQUEST, sizeof REQUEST - 1 });
    static const char STATUS_200[] = "\x01\x04\x00\x00\x00\x01\x88";
    test_check(test, str_contains(reply, (String){ STATUS_200, sizeof STATUS_200 - 1 }) && str_contains(reply, S("GET / ")),
               "the request is answered 200 on its stream");
    // The same request, reset by RST_STREAM in the same read, before a handler took it.
    static const char RESET[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
                                "\x00\x00\x00\x04\x00\x00\x00\x00\x00"
                                "\x00\x00\x06\x01\x05\x00\x00\x00\x01\x82\x86\x84\x41\x01h"
                                "\x00\x00\x04\x03\x00\x00\x00\x00\x01\x00\x00\x00\x08";
    reply = exchange_request(test, (String){ RESET, sizeof RESET - 1 });
    test_check(test, !str_contains(reply, S("GET / ")), "a request reset before its handler ran is not answered");
}

static HttpServer *start_server_with(Test *test, HttpStreamHandler stream_handler)
{
    HttpServerConfig config = {
        .address = S("127.0.0.1:0"),
        .limits = { .timeout_ms = 300 },
        .worker_count = 2,
        .handler = answer,
        .stream_handler = stream_handler,
    };
    HttpServer *server = nullptr;
    Err err = { 0 };
    test_check(test, http_server_create(&config, &server, &err) == ERR_OK, "start a server");
    return server;
}

static HttpServer *start_server(Test *test)
{
    return start_server_with(test, nullptr);
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
    String certificate;
    String ca_file;
    String other_ca_file;
    String other_certificate;
    String other_key;
    TlsServer *tls;
    HttpServer *server;
} Https;

static bool https_start(Test *test, Https *https, HttpStreamHandler stream_handler)
{
    String certificate = { 0 };
    String key = { 0 };
    Err err = { 0 };
    *https = (Https){ 0 };
    HttpServerConfig config = {
        .address = S("127.0.0.1:0"),
        .handler = answer,
        .stream_handler = stream_handler,
        .worker_count = 2,
    };
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
    https->certificate = certificate;
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
    if (!https_start(test, &https, nullptr)) {
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

static String sha256_hex(Arena *arena, String data)
{
    uint8_t digest[SHA256_LEN];
    sha256(data, digest);
    return hex_encode(arena, digest, SHA256_LEN);
}

static String header_value(const HttpResponse *response, const char *name)
{
    for (size_t i = 0; i < response->header_count; i++) {
        if (str_equal_ignore_case(response->headers[i].name, S(name))) {
            return response->headers[i].value;
        }
    }
    return S("");
}

// converse sends each of messages in turn on a new connection, receiving
// after each until the reply holds what follows it, or the server closes.
static String converse(Arena *arena, uint16_t port, const char *const *messages, size_t count)
{
    int fd = -1;
    StringBuilder reply = str_builder_create(arena, 256);
    if (net_connect(S("127.0.0.1"), str_format(arena, "%u", port), RECEIVE_TIMEOUT_MS, &fd, nullptr) != ERR_OK ||
        net_set_timeouts(fd, RECEIVE_TIMEOUT_MS, nullptr) != ERR_OK) {
        return S("");
    }
    for (size_t i = 0; i + 1 < count; i += 2) {
        if (net_send(fd, S(messages[i]), nullptr) != ERR_OK) {
            break;
        }
        char buffer[1024];
        size_t got = 0;
        while (!str_contains(str_builder_finish(&reply), S(messages[i + 1])) &&
               net_receive(fd, buffer, sizeof buffer, &got, nullptr) == ERR_OK && got > 0) {
            str_builder_append(&reply, (String){ buffer, got });
        }
    }
    net_close(fd);
    return str_builder_finish(&reply);
}

// check_streams uploads and downloads size bytes through stream_answer at
// base, a URL without a path.
static void check_streams(Test *test, HttpClient *client, String base, size_t size)
{
    char *upload = arena_push(test->arena, size);
    for (size_t i = 0; i < size; i++) {
        upload[i] = (char)('a' + i % 26);
    }
    HttpRequest hash = {
        .method = S("POST"),
        .url = str_format(test->arena, "%.*s/hash", (int)base.len, base.data),
        .body = (String){ upload, size },
    };
    HttpRequest download = { .url = str_format(test->arena, "%.*s/download?%zu", (int)base.len, base.data, size) };
    HttpRequest chunked = { .url = str_format(test->arena, "%.*s/chunked?%zu", (int)base.len, base.data, size) };
    HttpRequest head = { .method = S("HEAD"), .url = download.url };
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, http_send(client, test->arena, &hash, &response, &err) == ERR_OK, err.msg);
    test_check_str(test, response.body, sha256_hex(test->arena, hash.body), "an upload is hashed as it arrives");
    test_check(test, http_send(client, test->arena, &download, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.body.len == size && response.body.data[size - 1] == 'd', "a download sent in pieces");
    test_check(test, http_send(client, test->arena, &chunked, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.body.len == size, "and one of a length not announced");
    test_check(test, http_send(client, test->arena, &head, &response, &err) == ERR_OK, err.msg);
    test_check_str(test, header_value(&response, "Content-Length"), str_format(test->arena, "%zu", size),
                   "HEAD has the announced length");
}

static void http_server_streams_bodies_each_way(Test *test)
{
    HttpServer *server = start_server_with(test, stream_answer);
    if (server == nullptr) {
        return;
    }
    HttpClientConfig config = { .allow_plain_http = true, .max_response_bytes = BIG_BYTES };
    HttpClient *client = nullptr;
    Err err = { 0 };
    test_check(test, http_client_create(&config, &client, &err) == ERR_OK, err.msg);
    if (client != nullptr) {
        check_streams(test, client, str_format(test->arena, "http://127.0.0.1:%u", http_server_port(server)),
                      BIG_BYTES);
        http_client_destroy(client);
    }
    static const char *const ABANDONED[] = { "GET /download?1000000000 HTTP/1.1\r\nHost: h\r\n\r\n", "dddd" };
    test_check(test, str_contains(converse(test->arena, http_server_port(server), ABANDONED, countof(ABANDONED)), S("dddd")),
               "a download its client leaves, which must not hold up stopping");
    http_server_destroy(server);
}

static void http_server_streams_over_http2(Test *test)
{
    Https https;
    HttpClient *client = nullptr;
    Err err = { 0 };
    if (https_start(test, &https, stream_answer)) {
        test_check(test, https_client(https.ca_file, &client, &err) == ERR_OK, err.msg);
    }
    if (client != nullptr) {
        String base = str_format(test->arena, "https://localhost:%u", http_server_port(https.server));
        check_streams(test, client, base, TLS_STREAM_BYTES);
        http_client_destroy(client);
    }
    https_stop(&https);
}

static void http_server_continues_streamed_uploads_only_when_read(Test *test)
{
    HttpServer *server = start_server_with(test, stream_answer);
    if (server == nullptr) {
        return;
    }
    uint16_t port = http_server_port(server);
    static const char *const REFUSED[] = {
        "POST /refuse HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 1000000\r\n\r\n",
        "never sent",
    };
    String refused = converse(test->arena, port, REFUSED, countof(REFUSED));
    test_check(test, str_starts_with(refused, S("HTTP/1.1 417 ")) && str_contains(refused, S("Connection: close")),
               "an upload refused unread is not continued, and its connection closes");
    String digest = sha256_hex(test->arena, S("hello"));
    String hello = str_format(test->arena, "%.*s", (int)digest.len, digest.data);
    const char *const READ[] = {
        "POST /hash HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\n",
        "5\r\nhello\r\n0\r\n\r\n",
        hello.data,
    };
    String read = converse(test->arena, port, READ, countof(READ));
    test_check(test, str_starts_with(read, S("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\n")) &&
                         str_ends_with(read, hello),
               "an upload read is continued, and a chunked one hashed");
    static const char *const MALFORMED[] = {
        "POST /hash HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
        "never sent",
    };
    test_check(test, str_starts_with(converse(test->arena, port, MALFORMED, countof(MALFORMED)), S("HTTP/1.1 400 ")),
               "a malformed body read is answered 400");
    http_server_destroy(server);
}

// upload sends a POST of size bytes as a client that reads the response only
// once its body is sent, and gives the response, or empty when the server
// reset the connection on the way.
static String upload(Arena *arena, uint16_t port, const char *path, size_t size)
{
    int fd = -1;
    if (net_connect(S("127.0.0.1"), str_format(arena, "%u", port), RECEIVE_TIMEOUT_MS, &fd, nullptr) != ERR_OK) {
        return S("");
    }
    String head = str_format(arena, "POST %s HTTP/1.1\r\nHost: h\r\nContent-Length: %zu\r\n\r\n", path, size);
    char *body = arena_push(arena, size);
    memset(body, 'u', size);
    StringBuilder reply = str_builder_create(arena, 256);
    if (net_set_timeouts(fd, RECEIVE_TIMEOUT_MS, nullptr) == ERR_OK && net_send(fd, head, nullptr) == ERR_OK &&
        net_send(fd, (String){ body, size }, nullptr) == ERR_OK) {
        char buffer[1024];
        size_t got = 0;
        while (net_receive(fd, buffer, sizeof buffer, &got, nullptr) == ERR_OK && got > 0) {
            str_builder_append(&reply, (String){ buffer, got });
        }
    }
    net_close(fd);
    return str_builder_finish(&reply);
}

static void http_server_lets_a_refused_upload_finish(Test *test)
{
    HttpServer *streaming = start_server_with(test, stream_answer);
    if (streaming != nullptr) {
        test_check(test, str_starts_with(upload(test->arena, http_server_port(streaming), "/refuse", UPLOAD_REFUSED_BYTES),
                                         S("HTTP/1.1 417 ")),
                   "an upload its handler refused unread");
        http_server_destroy(streaming);
    }
    HttpServer *buffered = start_server(test);
    if (buffered != nullptr) {
        test_check(test, str_starts_with(upload(test->arena, http_server_port(buffered), "/", UPLOAD_REFUSED_BYTES),
                                         S("HTTP/1.1 413 ")),
                   "an upload over the size limit");
        http_server_destroy(buffered);
    }
}

// tls_talk connects to port over TLS trusting ca_pem, offering protocol, and
// reports whether the handshake held; it then asks for /version over HTTP/1.1.
static bool tls_talk(Test *test, uint16_t port, String ca_pem, String protocol, String *chosen, String *reply)
{
    int fd = -1;
    TlsConnection *tls = nullptr;
    TlsClientConfig config = { .host = S("localhost"), .ca_pem = ca_pem, .protocols = &protocol, .protocol_count = 1 };
    bool connected = net_connect(S("127.0.0.1"), str_format(test->arena, "%u", port), 1000, &fd, nullptr) == ERR_OK &&
                     net_set_timeouts(fd, RECEIVE_TIMEOUT_MS, nullptr) == ERR_OK &&
                     tls_connect(&config, fd, &tls, nullptr) == ERR_OK;
    if (connected) {
        *chosen = str_copy(test->arena, tls_protocol(tls));
        char buffer[1024];
        size_t got = 0;
        if (tls_write(tls, S("GET /version HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"), nullptr) ==
            ERR_OK) {
            StringBuilder text = str_builder_create(test->arena, 256);
            while (tls_read(tls, buffer, sizeof buffer, &got, nullptr) == ERR_OK && got > 0) {
                str_builder_append(&text, (String){ buffer, got });
            }
            *reply = str_builder_finish(&text);
        }
        tls_close(tls);
    }
    if (fd >= 0) {
        net_close(fd);
    }
    return connected;
}

static void tls_connect_checks_the_server_and_offers_alpn(Test *test)
{
    Https https;
    if (!https_start(test, &https, nullptr)) {
        https_stop(&https);
        return;
    }
    uint16_t port = http_server_port(https.server);
    String chosen = { 0 };
    String reply = { 0 };
    test_check(test, tls_talk(test, port, https.certificate, S("http/1.1"), &chosen, &reply), "a trusted server");
    test_check_str(test, chosen, S("http/1.1"), "ALPN chooses what was offered");
    test_check(test, str_ends_with(reply, S("\r\n\r\n1.1")), "and it is spoken");
    test_check(test, !tls_talk(test, port, https.other_certificate, S("h2"), &chosen, &reply),
               "a certificate that does not chain to the trusted one fails");
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
    { "http_fields_read_and_write", http_fields_read_and_write },
    { "http_server_answers_500_for_failures", http_server_answers_500_for_failures },
    { "http_server_exchanges_http2_with_prior_knowledge", http_server_exchanges_http2_with_prior_knowledge },
    { "http_server_continues_and_answers_head", http_server_continues_and_answers_head },
    { "http_server_serves_the_http_client", http_server_serves_the_http_client },
    { "http_server_runs_slow_handlers_aside", http_server_runs_slow_handlers_aside },
    { "http_server_sends_bodies_larger_than_the_socket_buffer", http_server_sends_bodies_larger_than_the_socket_buffer },
    { "http_server_closes_idle_clients", http_server_closes_idle_clients },
    { "http_server_serves_https", http_server_serves_https },
    { "http_server_lets_a_refused_upload_finish", http_server_lets_a_refused_upload_finish },
    { "http_server_finishes_requests_when_stopped", http_server_finishes_requests_when_stopped },
    { "tls_connect_checks_the_server_and_offers_alpn", tls_connect_checks_the_server_and_offers_alpn },
    { "http_server_streams_bodies_each_way", http_server_streams_bodies_each_way },
    { "http_server_streams_over_http2", http_server_streams_over_http2 },
    { "http_server_continues_streamed_uploads_only_when_read", http_server_continues_streamed_uploads_only_when_read },
    { nullptr, nullptr },
};
