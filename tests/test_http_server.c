#include "test.h"

#include "mc/net/http.h"
#include "mc/net/http_server.h"
#include "mc/platform/platform.h"

#include <string.h>

// The tests run the server's handler without a socket through
// http_server_exchange, and on 127.0.0.1 against raw sockets and libmc's own
// HTTP client.

enum { SLOW_MS = 600, RECEIVE_TIMEOUT_MS = 3000, BIG_BYTES = 8 << 20 };

// answer echoes the method, path and body; /slow sleeps first, /fail fails,
// /bad-header sets a field value holding a line break and /big answers more
// than a socket buffer holds.
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
    if (str_equal(request->path, S("/big"))) {
        char *big = arena_push(arena, BIG_BYTES);
        memset(big, 'x', BIG_BYTES);
        response->body = (String){ big, BIG_BYTES };
        return ERR_OK;
    }
    response->body = str_format(arena, "%.*s %.*s %.*s", (int)request->method.len, request->method.data,
                                (int)request->path.len, request->path.data, (int)request->body.len, request->body.data);
    return ERR_OK;
}

static String exchange(Test *test, String request)
{
    HttpServerConfig config = { .handler = answer };
    StringList pieces = { 0 };
    strlist_push(test->arena, &pieces, request);
    String reply = S("");
    Err err = { 0 };
    test_check(test, http_server_exchange(test->arena, &config, pieces, &reply, &err) == ERR_OK, "exchange");
    return reply;
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
    { "http_server_refuses_ambiguous_requests", http_server_refuses_ambiguous_requests },
    { "http_server_answers_500_for_failures", http_server_answers_500_for_failures },
    { "http_server_continues_and_answers_head", http_server_continues_and_answers_head },
    { "http_server_serves_the_http_client", http_server_serves_the_http_client },
    { "http_server_runs_slow_handlers_aside", http_server_runs_slow_handlers_aside },
    { "http_server_sends_bodies_larger_than_the_socket_buffer", http_server_sends_bodies_larger_than_the_socket_buffer },
    { "http_server_closes_idle_clients", http_server_closes_idle_clients },
    { "http_server_finishes_requests_when_stopped", http_server_finishes_requests_when_stopped },
    { nullptr, nullptr },
};
