#include "mc/net/http_server.h"

#include <assert.h>

#include "mc/concurrency/queue.h"
#include "mc/concurrency/threadpool.h"
#include "mc/platform/platform.h"
#include "mc/platform/poll.h"
#include "http1.h"
#include "tls_session.h"

enum { RECEIVE_BUFFER_BYTES = 16384, MAX_READY = 64, MAX_SWEEP_MS = 1000, NS_PER_MS = 1000000 };

static const HttpServerLimits DEFAULT_LIMITS = {
    .max_header_bytes = 8192,
    .max_body_bytes = 1 << 20,
    .max_connections = 1024,
    .timeout_ms = 30000,
};

typedef struct Loop Loop;

// Connection is one client's socket, owned by one loop for its whole life.
typedef struct Connection {
    Arena *arena;
    Loop *loop;
    Http1 *http;
    int fd;
    TlsSession *tls; // nullptr on a plain connection
    PollInterest interest;
    char *input;
    String pending; // received and not yet parsed, in input
    String output;  // not yet sent
    bool busy;      // a worker is running the handler
    bool closing;   // close once the output is sent
    bool peer_closed;
    bool closed;
    int64_t deadline_ns; // 0 while the handler runs
    struct Connection *previous;
    struct Connection *next;
} Connection;

// Exchange carries a request to a worker and its response back. It lives in
// the request's arena, which the worker owns until the loop takes the
// exchange back; the worker never touches the connection.
typedef struct Exchange {
    Task task;
    Loop *loop;
    Connection *connection;
    HttpHandler handler;
    void *context;
    Arena *arena;
    HttpServerRequest request;
    HttpServerResponse response;
    bool failed;
} Exchange;

// Loop is an event loop thread. Workers hand exchanges back through done,
// and http_server_destroy pushes the loop itself there to stop it.
struct Loop {
    HttpServer *server;
    Poller *poller;
    Queue *done;
    Thread thread;
    Connection *connections;
    Connection *closed; // freed once the ready batch that may name them is handled
    size_t connection_count;
    size_t max_connections;
    bool accepting;
    bool stopping;
    int64_t next_sweep_ns;
};

struct HttpServer {
    Arena *arena;
    HttpServerConfig config;
    int listener;
    uint16_t port;
    ThreadPool *workers;
    Loop *loops;
    size_t started_loops;
};

static int64_t timeout_ns(const Loop *loop)
{
    return loop->server->config.limits.timeout_ms * NS_PER_MS;
}

static void connection_close(Connection *connection)
{
    Loop *loop = connection->loop;
    if (connection->interest != POLL_NONE) {
        unused(poller_watch(loop->poller, connection->fd, POLL_NONE, connection, nullptr));
    }
    if (connection->tls != nullptr) {
        tls_session_destroy(connection->tls);
    }
    net_close(connection->fd);
    connection->closed = true;
    if (connection->previous != nullptr) {
        connection->previous->next = connection->next;
    } else {
        loop->connections = connection->next;
    }
    if (connection->next != nullptr) {
        connection->next->previous = connection->previous;
    }
    connection->next = loop->closed;
    loop->closed = connection;
    loop->connection_count--;
}

static void connections_free_closed(Loop *loop)
{
    while (loop->closed != nullptr) {
        Connection *connection = loop->closed;
        loop->closed = connection->next;
        http1_destroy(connection->http);
        arena_destroy(connection->arena);
    }
}

// connection_watch reports whether the poller took the interest; a
// connection it refuses is closed.
static bool connection_watch(Connection *connection, PollInterest interest)
{
    if (connection->interest == interest) {
        return true;
    }
    if (poller_watch(connection->loop->poller, connection->fd, interest, connection, nullptr) != ERR_OK) {
        connection_close(connection);
        return false;
    }
    connection->interest = interest;
    return true;
}

static void run_exchange(void *argument)
{
    Exchange *exchange = argument;
    exchange->response = (HttpServerResponse){ .status = 200 };
    Err err = { 0 };
    exchange->failed =
        exchange->handler(exchange->context, exchange->arena, &exchange->request, &exchange->response, &err) != ERR_OK;
    Loop *loop = exchange->loop;
    bool pushed = queue_push(loop->done, exchange);
    assert(pushed);
    unused(pushed);
    poller_wake(loop->poller);
}

static void start_exchange(Connection *connection)
{
    Arena *arena = http1_arena(connection->http);
    Exchange *exchange = arena_push(arena, sizeof *exchange);
    const HttpServerConfig *config = &connection->loop->server->config;
    *exchange = (Exchange){
        .loop = connection->loop,
        .connection = connection,
        .handler = config->handler,
        .context = config->context,
        .arena = arena,
        .request = *http1_request(connection->http),
    };
    exchange->task = (Task){ .run = run_exchange, .argument = exchange };
    connection->busy = true;
    connection->deadline_ns = 0;
    threadpool_submit(connection->loop->server->workers, &exchange->task);
}

// socket_result reads a plain socket's result as TLS reports one, which may
// also wait on the other direction while it handshakes.
static TlsResult socket_result(NetResult result, TlsResult would_block)
{
    switch (result) {
    case NET_DONE:
        return TLS_DONE;
    case NET_WOULD_BLOCK:
        return would_block;
    case NET_CLOSED:
        break;
    }
    return TLS_CLOSED;
}

static TlsResult connection_send(Connection *connection, size_t *sent)
{
    if (connection->tls != nullptr) {
        return tls_try_send(connection->tls, connection->output, sent);
    }
    return socket_result(net_try_send(connection->fd, connection->output, sent), TLS_WANT_WRITE);
}

static TlsResult connection_receive(Connection *connection, size_t *got)
{
    if (connection->tls != nullptr) {
        return tls_try_receive(connection->tls, connection->input, RECEIVE_BUFFER_BYTES, got);
    }
    return socket_result(net_try_receive(connection->fd, connection->input, RECEIVE_BUFFER_BYTES, got), TLS_WANT_READ);
}

// send_output reports whether to try again: it sent some, and did not have to
// wait or close.
static bool send_output(Connection *connection)
{
    size_t sent = 0;
    switch (connection_send(connection, &sent)) {
    case TLS_DONE:
        connection->output = str_slice(connection->output, sent, connection->output.len);
        connection->deadline_ns = clock_monotonic_ns() + timeout_ns(connection->loop);
        return true;
    case TLS_WANT_READ:
        connection_watch(connection, POLL_READ);
        return false;
    case TLS_WANT_WRITE:
        connection_watch(connection, POLL_WRITE);
        return false;
    case TLS_CLOSED:
        connection_close(connection);
        return false;
    }
    return false;
}

// receive_input reports whether there is something to parse: bytes, or the
// peer having closed.
static bool receive_input(Connection *connection)
{
    size_t got = 0;
    switch (connection_receive(connection, &got)) {
    case TLS_DONE:
        connection->pending = (String){ connection->input, got };
        return true;
    case TLS_WANT_READ:
        connection_watch(connection, POLL_READ);
        return false;
    case TLS_WANT_WRITE:
        connection_watch(connection, POLL_WRITE);
        return false;
    case TLS_CLOSED:
        connection->peer_closed = true;
        return true;
    }
    return false;
}

static void parse_pending(Connection *connection)
{
    size_t used = 0;
    Http1Event event = http1_feed(connection->http, connection->pending, &used);
    connection->pending = str_slice(connection->pending, used, connection->pending.len);
    connection->output = http1_take_output(connection->http);
    switch (event) {
    case HTTP1_MORE:
        break;
    case HTTP1_REQUEST:
        start_exchange(connection);
        break;
    case HTTP1_CLOSE:
        connection->closing = true;
        break;
    }
}

// connection_advance moves the connection as far as it can go without
// waiting: send, then parse what has arrived, then receive more.
static void connection_advance(Connection *connection)
{
    while (!connection->closed) {
        if (connection->output.len > 0) {
            if (!send_output(connection)) {
                return;
            }
            continue;
        }
        if (connection->busy) {
            connection_watch(connection, POLL_NONE);
            return;
        }
        if (connection->closing) {
            connection_close(connection);
            return;
        }
        if (connection->pending.len == 0 && !connection->peer_closed && !receive_input(connection)) {
            return;
        }
        parse_pending(connection);
    }
}

static void connection_open(Loop *loop, int fd)
{
    Arena *arena = arena_create(0);
    Connection *connection = arena_push(arena, sizeof *connection);
    *connection = (Connection){
        .arena = arena,
        .loop = loop,
        .http = http1_create(loop->server->config.limits),
        .fd = fd,
        .tls = loop->server->config.tls != nullptr ? tls_session_create(arena, loop->server->config.tls, fd) : nullptr,
        .input = arena_push(arena, RECEIVE_BUFFER_BYTES),
        .deadline_ns = clock_monotonic_ns() + timeout_ns(loop),
        .next = loop->connections,
    };
    if (loop->connections != nullptr) {
        loop->connections->previous = connection;
    }
    loop->connections = connection;
    loop->connection_count++;
    connection_advance(connection);
}

static void watch_listener(Loop *loop, bool accepting)
{
    if (loop->accepting == accepting) {
        return;
    }
    PollInterest interest = accepting ? POLL_READ : POLL_NONE;
    loop->accepting = poller_watch(loop->poller, loop->server->listener, interest, loop, nullptr) == ERR_OK && accepting;
}

static void accept_connections(Loop *loop)
{
    int fd = -1;
    while (loop->connection_count < loop->max_connections && net_try_accept(loop->server->listener, &fd)) {
        connection_open(loop, fd);
    }
    watch_listener(loop, loop->connection_count < loop->max_connections);
}

static void respond(Exchange *exchange)
{
    Connection *connection = exchange->connection;
    Loop *loop = connection->loop;
    connection->busy = false;
    bool open = http1_respond(connection->http, exchange->failed ? nullptr : &exchange->response, loop->stopping);
    connection->output = http1_take_output(connection->http);
    connection->closing = !open;
    connection->deadline_ns = clock_monotonic_ns() + timeout_ns(loop);
    connection_advance(connection);
}

// stop stops accepting and closes the connections waiting for a request;
// the rest close after their response.
static void stop(Loop *loop)
{
    loop->stopping = true;
    watch_listener(loop, false);
    Connection *next = nullptr;
    for (Connection *connection = loop->connections; connection != nullptr; connection = next) {
        next = connection->next;
        if (!connection->busy && connection->output.len == 0 && connection->pending.len == 0 &&
            http1_idle(connection->http)) {
            connection_close(connection);
        }
    }
}

static void take_done(Loop *loop)
{
    void *item = nullptr;
    while (queue_pop_timeout(loop->done, 0, &item)) {
        if (item == loop) {
            stop(loop);
        } else {
            respond(item);
        }
    }
}

static void close_expired(Loop *loop)
{
    int64_t now = clock_monotonic_ns();
    if (now < loop->next_sweep_ns) {
        return;
    }
    loop->next_sweep_ns = now + MAX_SWEEP_MS * NS_PER_MS;
    Connection *next = nullptr;
    for (Connection *connection = loop->connections; connection != nullptr; connection = next) {
        next = connection->next;
        if (connection->deadline_ns != 0 && now >= connection->deadline_ns) {
            connection_close(connection);
        }
    }
    if (!loop->stopping) {
        watch_listener(loop, loop->connection_count < loop->max_connections);
    }
}

static void *loop_run(void *argument)
{
    Loop *loop = argument;
    void *ready[MAX_READY];
    int sweep_ms = (int)(loop->server->config.limits.timeout_ms < MAX_SWEEP_MS ? loop->server->config.limits.timeout_ms
                                                                                : MAX_SWEEP_MS);
    while (!loop->stopping || loop->connection_count > 0) {
        size_t count = poller_wait(loop->poller, ready, MAX_READY, sweep_ms);
        for (size_t i = 0; i < count; i++) {
            if (ready[i] == loop) {
                accept_connections(loop);
            } else if (ready[i] != nullptr && !((Connection *)ready[i])->closed) {
                connection_advance(ready[i]);
            }
        }
        take_done(loop);
        close_expired(loop);
        connections_free_closed(loop);
    }
    return nullptr;
}

static HttpServerConfig config_with_defaults(Arena *arena, const HttpServerConfig *config)
{
    assert(config->handler != nullptr);
    HttpServerConfig resolved = *config;
    resolved.address = str_copy(arena, config->address);
    HttpServerLimits *limits = &resolved.limits;
    limits->max_header_bytes = limits->max_header_bytes != 0 ? limits->max_header_bytes : DEFAULT_LIMITS.max_header_bytes;
    limits->max_body_bytes = limits->max_body_bytes != 0 ? limits->max_body_bytes : DEFAULT_LIMITS.max_body_bytes;
    limits->max_connections = limits->max_connections != 0 ? limits->max_connections : DEFAULT_LIMITS.max_connections;
    limits->timeout_ms = limits->timeout_ms > 0 ? limits->timeout_ms : DEFAULT_LIMITS.timeout_ms;
    resolved.loop_count = resolved.loop_count != 0 ? resolved.loop_count : 1;
    return resolved;
}

[[nodiscard]] static Error listen_on(HttpServer *server, Err *err)
{
    Error e = net_listen(server->config.address, &server->listener, err);
    if (e != ERR_OK) {
        return e;
    }
    String address = { 0 };
    size_t colon = 0;
    uint64_t port = 0;
    e = net_set_nonblocking(server->listener, err);
    if (e == ERR_OK) {
        e = net_local_address(server->arena, server->listener, &address, err);
    }
    if (e == ERR_OK && (!str_find_last_char(address, ':', &colon) ||
                        !str_parse_u64(str_slice(address, colon + 1, address.len), &port) || port > UINT16_MAX)) {
        e = err_set(err, ERR_PLATFORM, "unexpected local address %.*s", (int)address.len, address.data);
    }
    if (e != ERR_OK) {
        net_close(server->listener);
        return e;
    }
    server->port = (uint16_t)port;
    return ERR_OK;
}

// server_free stops the loops started so far and releases everything.
static void server_free(HttpServer *server)
{
    for (size_t i = 0; i < server->started_loops; i++) {
        bool pushed = queue_push(server->loops[i].done, &server->loops[i]);
        assert(pushed);
        unused(pushed);
        poller_wake(server->loops[i].poller);
    }
    for (size_t i = 0; i < server->started_loops; i++) {
        thread_join(&server->loops[i].thread);
    }
    // The workers go before the pollers they wake.
    if (server->workers != nullptr) {
        threadpool_destroy(server->workers);
    }
    for (size_t i = 0; i < server->config.loop_count; i++) {
        Loop *loop = &server->loops[i];
        connections_free_closed(loop);
        if (loop->poller != nullptr) {
            poller_destroy(loop->poller);
        }
        if (loop->done != nullptr) {
            queue_destroy(loop->done);
        }
    }
    net_close(server->listener);
    arena_destroy(server->arena);
}

[[nodiscard]] static Error loop_init(HttpServer *server, Loop *loop, Err *err)
{
    size_t share = server->config.limits.max_connections / server->config.loop_count;
    *loop = (Loop){ .server = server, .done = queue_create(), .max_connections = share != 0 ? share : 1 };
    Error e = poller_create(&loop->poller, err);
    if (e != ERR_OK) {
        return e;
    }
    e = poller_watch(loop->poller, server->listener, POLL_READ, loop, err);
    loop->accepting = e == ERR_OK;
    return e;
}

Error http_server_create(const HttpServerConfig *config, HttpServer **server, Err *err)
{
    Arena *arena = arena_create(0);
    HttpServer *created = arena_push(arena, sizeof *created);
    *created = (HttpServer){ .arena = arena, .config = config_with_defaults(arena, config) };
    Error e = listen_on(created, err);
    if (e != ERR_OK) {
        arena_destroy(arena);
        return e;
    }
    size_t loop_count = created->config.loop_count;
    created->loops = arena_push(arena, loop_count * sizeof *created->loops);
    for (size_t i = 0; i < loop_count; i++) {
        created->loops[i] = (Loop){ 0 };
    }
    e = threadpool_create(created->config.worker_count, &created->workers, err);
    for (size_t i = 0; e == ERR_OK && i < loop_count; i++) {
        e = loop_init(created, &created->loops[i], err);
    }
    for (size_t i = 0; e == ERR_OK && i < loop_count; i++) {
        e = thread_start(&created->loops[i].thread, loop_run, &created->loops[i], err);
        created->started_loops += e == ERR_OK ? 1 : 0;
    }
    if (e != ERR_OK) {
        server_free(created);
        return e;
    }
    *server = created;
    return ERR_OK;
}

uint16_t http_server_port(const HttpServer *server)
{
    return server->port;
}

void http_server_destroy(HttpServer *server)
{
    server_free(server);
}

Error http_server_exchange(Arena *arena, const HttpServerConfig *config, StringList pieces, String *answer, Err *err)
{
    Arena *scratch = arena_create(0);
    HttpServerConfig resolved = config_with_defaults(scratch, config);
    Http1 *http = http1_create(resolved.limits);
    StringBuilder output = str_builder_create(arena, 1024);
    bool open = true;
    for (size_t i = 0; open && i <= pieces.count; i++) {
        // After the last piece the client closes, which an empty piece tells http1_feed.
        String data = i < pieces.count ? pieces.items[i] : (String){ 0 };
        do {
            size_t used = 0;
            Http1Event event = http1_feed(http, data, &used);
            data = str_slice(data, used, data.len);
            str_builder_append(&output, http1_take_output(http));
            if (event == HTTP1_REQUEST) {
                HttpServerResponse response = { .status = 200 };
                Error e = resolved.handler(resolved.context, http1_arena(http), http1_request(http), &response, nullptr);
                open = http1_respond(http, e == ERR_OK ? &response : nullptr, false);
                str_builder_append(&output, http1_take_output(http));
            }
            open = open && event != HTTP1_CLOSE;
        } while (open && data.len > 0);
    }
    *answer = str_builder_finish(&output);
    http1_destroy(http);
    arena_destroy(scratch);
    return ERR_OK;
}
