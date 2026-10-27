#include "mc/net/http_server.h"

#include <assert.h>
#include <string.h>

#include "mc/concurrency/queue.h"
#include "mc/concurrency/threadpool.h"
#include "mc/platform/platform.h"
#include "mc/platform/poll.h"
#include "http1.h"
#include "http2.h"
#include "http_compress.h"
#include "tls_session.h"

enum { RECEIVE_BUFFER_BYTES = 16384, PIPE_BYTES = 65536, MAX_READY = 64, MAX_SWEEP_MS = 1000, NS_PER_MS = 1000000 };

static const HttpServerLimits DEFAULT_LIMITS = {
    .max_header_bytes = 8192,
    .max_body_bytes = 1 << 20,
    .max_connections = 1024,
    .timeout_ms = 30000,
};

typedef struct Loop Loop;
typedef struct Connection Connection;

// Connection is one client's socket, owned by one loop for its whole life.
struct Connection {
    Arena *arena;
    Loop *loop;
    Http1 *http;  // HTTP/1.1, until ALPN chooses h2
    Http2 *http2; // HTTP/2 once ALPN chose it
    bool protocol_chosen;
    int fd;
    TlsSession *tls; // nullptr on a plain connection
    PollInterest interest;
    char *input;
    String pending; // received and not yet parsed, in input
    String output;  // not yet sent
    size_t in_flight; // requests a worker holds; HTTP/1.1 has at most one
    HttpExchange *exchanges; // the streamed ones among them
    bool closing;   // close once the output is sent
    bool peer_closed;
    bool closed;
    int64_t deadline_ns; // 0 while the handler runs
    Connection *previous;
    Connection *next;
};

// HttpExchange carries a request to a worker and its response back. It lives
// in the request's arena, which the worker owns until the loop takes the
// exchange back; the worker never touches the connection. A streamed one
// lives in an arena of its own, and the worker and the loop pass the body
// each way through two bounded pipes, under its mutex.
struct HttpExchange {
    Task task;
    Loop *loop;
    Connection *connection;
    HttpHandler handler;
    HttpStreamHandler stream_handler;
    void *context;
    Arena *arena;
    HttpServerRequest request;
    HttpServerResponse response;
    bool failed;
    Http2Stream *stream; // the request's stream on HTTP/2, nullptr on HTTP/1.1
    const HttpCompression *compression;

    Mutex mutex;
    Cond changed; // the worker waits on it for the loop
    char *in;     // body received and not yet read
    size_t in_len;
    bool in_ended;
    Error in_error; // why the body stopped short
    bool continue_wanted;
    bool head_ready; // the worker started the response
    unsigned status;
    HttpHeader *headers;
    size_t header_count;
    int64_t content_length;
    char *out; // body written and not yet sent
    size_t out_len;
    bool finished; // the handler returned
    bool complete; // and its response is whole
    bool gone;     // the client is
    bool queued;   // on the loop's done queue
    bool head_sent; // the loop's alone from here on
    HttpExchange *previous;
    HttpExchange *next;
    int64_t written; // the worker's alone
    bool bodyless;
};

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
    for (HttpExchange *exchange = connection->exchanges; exchange != nullptr; exchange = exchange->next) {
        mutex_lock(&exchange->mutex);
        exchange->gone = true;
        exchange->out_len = 0;
        cond_broadcast(&exchange->changed);
        mutex_unlock(&exchange->mutex);
    }
}

// connections_free_closed frees the closed connections no worker still answers on.
static void connections_free_closed(Loop *loop)
{
    Connection **link = &loop->closed;
    while (*link != nullptr) {
        Connection *connection = *link;
        if (connection->in_flight > 0) {
            link = &connection->next;
            continue;
        }
        *link = connection->next;
        if (connection->http != nullptr) {
            http1_destroy(connection->http);
        }
        if (connection->http2 != nullptr) {
            http2_destroy(connection->http2);
        }
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
    HttpExchange *exchange = argument;
    exchange->response = (HttpServerResponse){ .status = 200 };
    Err err = { 0 };
    exchange->failed =
        exchange->handler(exchange->context, exchange->arena, &exchange->request, &exchange->response, &err) != ERR_OK;
    if (!exchange->failed && exchange->compression != nullptr) {
        http_compress(exchange->arena, &exchange->request, &exchange->response, exchange->compression);
    }
    Loop *loop = exchange->loop;
    bool pushed = queue_push(loop->done, exchange);
    assert(pushed);
    unused(pushed);
    poller_wake(loop->poller);
}

// notify hands exchange to its loop, once until the loop takes it. The
// caller holds its mutex.
static void notify(HttpExchange *exchange)
{
    if (!exchange->queued) {
        exchange->queued = true;
        bool pushed = queue_push(exchange->loop->done, exchange);
        assert(pushed);
        unused(pushed);
        poller_wake(exchange->loop->poller);
    }
}

static void run_streamed_exchange(void *argument)
{
    HttpExchange *exchange = argument;
    Err err = { 0 };
    bool failed = exchange->stream_handler(exchange->context, exchange, &err) != ERR_OK;
    bool announced = exchange->bodyless || exchange->content_length < 0 || exchange->written == exchange->content_length;
    mutex_lock(&exchange->mutex);
    exchange->finished = true;
    exchange->complete = !exchange->head_ready || (!failed && announced);
    notify(exchange);
    mutex_unlock(&exchange->mutex);
}

static void start_streamed_exchange(Connection *connection, const HttpServerRequest *request, Http2Stream *stream)
{
    Arena *arena = arena_create(0);
    HttpExchange *exchange = arena_push(arena, sizeof *exchange);
    const HttpServerConfig *config = &connection->loop->server->config;
    *exchange = (HttpExchange){
        .loop = connection->loop,
        .connection = connection,
        .stream_handler = config->stream_handler,
        .context = config->context,
        .arena = arena,
        .request = *request,
        .stream = stream,
        .in = arena_push(arena, PIPE_BYTES),
        .in_ended = connection->http != nullptr && http1_body_complete(connection->http),
        .out = arena_push(arena, PIPE_BYTES),
        .next = connection->exchanges,
    };
    mutex_init(&exchange->mutex);
    cond_init(&exchange->changed);
    if (connection->exchanges != nullptr) {
        connection->exchanges->previous = exchange;
    }
    connection->exchanges = exchange;
    exchange->task = (Task){ .run = run_streamed_exchange, .argument = exchange };
    connection->in_flight++;
    connection->deadline_ns = 0;
    threadpool_submit(connection->loop->server->workers, &exchange->task);
}

static void start_exchange(Connection *connection, Arena *arena, const HttpServerRequest *request,
                           Http2Stream *stream)
{
    if (connection->loop->server->config.stream_handler != nullptr) {
        start_streamed_exchange(connection, request, stream);
        return;
    }
    HttpExchange *exchange = arena_push(arena, sizeof *exchange);
    const HttpServerConfig *config = &connection->loop->server->config;
    *exchange = (HttpExchange){
        .loop = connection->loop,
        .connection = connection,
        .handler = config->handler,
        .context = config->context,
        .arena = arena,
        .request = *request,
        .stream = stream,
        .compression = config->compression,
    };
    exchange->task = (Task){ .run = run_exchange, .argument = exchange };
    connection->in_flight++;
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
        if (connection->in_flight > 0 && connection->http != nullptr) {
            // A streamed body may take long; only a pause in it is too long.
            connection->deadline_ns = clock_monotonic_ns() + timeout_ns(connection->loop);
        }
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

// choose_protocol switches to HTTP/2 when the TLS handshake chose it, which
// it has once the first bytes after it arrive.
static void choose_protocol(Connection *connection)
{
    connection->protocol_chosen = true;
    if (connection->tls != nullptr && str_equal(tls_session_protocol(connection->tls), S("h2"))) {
        http1_destroy(connection->http);
        connection->http = nullptr;
        const HttpServerConfig *config = &connection->loop->server->config;
        connection->http2 = http2_create(config->limits, config->stream_handler != nullptr);
    }
}

static void parse_pending_http2(Connection *connection)
{
    bool open = http2_feed(connection->http2, connection->pending);
    connection->pending = (String){ 0 };
    Http2Request request;
    while (http2_next_request(connection->http2, &request)) {
        start_exchange(connection, request.arena, request.request, request.stream);
    }
    connection->closing = connection->closing || !open;
}

static void parse_pending(Connection *connection)
{
    if (!connection->protocol_chosen) {
        choose_protocol(connection);
    }
    if (connection->http2 != nullptr) {
        parse_pending_http2(connection);
        return;
    }
    // A streamed body is fed no faster than its pipe empties.
    HttpExchange *streamed = connection->exchanges;
    String data = connection->pending;
    if (streamed != nullptr) {
        mutex_lock(&streamed->mutex);
        size_t room = PIPE_BYTES - streamed->in_len;
        mutex_unlock(&streamed->mutex);
        data.len = data.len < room ? data.len : room;
    }
    size_t used = 0;
    Http1Event event = http1_feed(connection->http, data, &used);
    connection->pending = str_slice(connection->pending, used, connection->pending.len);
    connection->output = http1_take_output(connection->http);
    switch (event) {
    case HTTP1_MORE:
        break;
    case HTTP1_REQUEST:
        start_exchange(connection, http1_arena(connection->http), http1_request(connection->http), nullptr);
        break;
    case HTTP1_BODY_END:
        mutex_lock(&streamed->mutex);
        streamed->in_ended = true;
        cond_broadcast(&streamed->changed);
        mutex_unlock(&streamed->mutex);
        break;
    case HTTP1_CLOSE:
        connection->closing = true;
        if (streamed != nullptr) {
            mutex_lock(&streamed->mutex);
            streamed->in_error = connection->peer_closed ? ERR_NETWORK : ERR_PARSE;
            cond_broadcast(&streamed->changed);
            mutex_unlock(&streamed->mutex);
        }
        break;
    }
}

// receive_body puts a piece of a streamed HTTP/1.1 body in its pipe, which
// parse_pending made room for.
static void receive_body(void *context, String piece)
{
    Connection *connection = context;
    HttpExchange *exchange = connection->exchanges;
    mutex_lock(&exchange->mutex);
    assert(piece.len <= PIPE_BYTES - exchange->in_len);
    memcpy(exchange->in + exchange->in_len, piece.data, piece.len);
    exchange->in_len += piece.len;
    cond_broadcast(&exchange->changed);
    mutex_unlock(&exchange->mutex);
}

// pump_http1 passes what the worker wrote to the connection, once what it
// passed before is sent.
static void pump_http1(Connection *connection, HttpExchange *exchange)
{
    Http1 *http = connection->http;
    if (connection->output.len > 0) {
        return;
    }
    if (exchange->continue_wanted) {
        http1_continue(http);
    }
    if (exchange->head_ready && !exchange->head_sent) {
        http1_start(http, exchange->status, exchange->headers, exchange->header_count, exchange->content_length,
                    connection->loop->stopping);
        exchange->head_sent = true;
    }
    if (exchange->head_sent && exchange->out_len > 0) {
        http1_write(http, (String){ exchange->out, exchange->out_len });
        exchange->out_len = 0;
        cond_broadcast(&exchange->changed);
    }
    connection->output = http1_take_output(http);
}

// pump_http2 moves the body each way between the stream and the worker.
static void pump_http2(Connection *connection, HttpExchange *exchange)
{
    Http2 *http = connection->http2;
    Http2Stream *stream = exchange->stream;
    if (http2_stream_closed(stream)) {
        exchange->gone = true;
        exchange->out_len = 0;
        cond_broadcast(&exchange->changed);
        return;
    }
    String body = http2_body(stream);
    size_t count = body.len < PIPE_BYTES - exchange->in_len ? body.len : PIPE_BYTES - exchange->in_len;
    if (count > 0) {
        memcpy(exchange->in + exchange->in_len, body.data, count);
        exchange->in_len += count;
        http2_body_take(http, stream, count);
        cond_broadcast(&exchange->changed);
    }
    if (!exchange->in_ended && http2_body_ended(stream)) {
        exchange->in_ended = true;
        cond_broadcast(&exchange->changed);
    }
    if (exchange->head_ready && !exchange->head_sent) {
        http2_start(http, stream, exchange->status, exchange->headers, exchange->header_count,
                    exchange->content_length);
        exchange->head_sent = true;
    }
    if (exchange->head_sent && exchange->out_len > 0 && http2_writable(stream)) {
        http2_write(http, stream, (String){ exchange->out, exchange->out_len });
        exchange->out_len = 0;
        cond_broadcast(&exchange->changed);
    }
}

// end_response ends the response of a handler that returned, once all it
// wrote has gone to the connection, and reports whether it did.
static bool end_response(Connection *connection, HttpExchange *exchange)
{
    if (!exchange->finished || exchange->queued || exchange->out_len > 0) {
        return false;
    }
    if (connection->closed) {
        return true;
    }
    if (connection->http2 != nullptr) {
        if (!exchange->head_sent && !http2_stream_closed(exchange->stream)) {
            http2_start(connection->http2, exchange->stream, 500, nullptr, 0, 0);
        }
        http2_end(connection->http2, exchange->stream, exchange->complete);
        return true;
    }
    if (connection->output.len > 0) {
        return false;
    }
    if (!exchange->head_sent) {
        http1_start(connection->http, 500, nullptr, 0, 0, connection->loop->stopping);
    }
    connection->closing = !http1_end(connection->http, exchange->complete);
    connection->output = http1_take_output(connection->http);
    return true;
}

static void exchange_free(HttpExchange *exchange)
{
    Connection *connection = exchange->connection;
    if (exchange->previous != nullptr) {
        exchange->previous->next = exchange->next;
    } else {
        connection->exchanges = exchange->next;
    }
    if (exchange->next != nullptr) {
        exchange->next->previous = exchange->previous;
    }
    connection->in_flight--;
    if (connection->in_flight == 0) {
        connection->deadline_ns = clock_monotonic_ns() + timeout_ns(connection->loop);
    }
    mutex_destroy(&exchange->mutex);
    cond_destroy(&exchange->changed);
    arena_destroy(exchange->arena);
}

// pump moves what it can between a streamed exchange and its connection,
// and frees the exchange once it is over.
static void pump(HttpExchange *exchange)
{
    Connection *connection = exchange->connection;
    mutex_lock(&exchange->mutex);
    if (!connection->closed && !exchange->gone) {
        if (connection->http2 != nullptr) {
            pump_http2(connection, exchange);
        } else {
            pump_http1(connection, exchange);
        }
    }
    bool over = end_response(connection, exchange);
    mutex_unlock(&exchange->mutex);
    if (over) {
        exchange_free(exchange);
    }
}

static void pump_all(Connection *connection)
{
    HttpExchange *next = nullptr;
    for (HttpExchange *exchange = connection->exchanges; exchange != nullptr; exchange = next) {
        next = exchange->next;
        pump(exchange);
    }
}

// waiting_on_worker says whether the connection has nothing to do until a
// handler moves on: HTTP/1.1 answers one request at a time, and reads a
// streamed body only into room in its pipe; HTTP/2 reads on while handlers run.
static bool waiting_on_worker(Connection *connection)
{
    if (connection->in_flight == 0) {
        return false;
    }
    if (connection->http2 != nullptr) {
        return connection->closing || http2_done(connection->http2);
    }
    HttpExchange *streamed = connection->exchanges;
    if (streamed == nullptr || connection->closing || http1_body_complete(connection->http)) {
        return true;
    }
    mutex_lock(&streamed->mutex);
    bool full = streamed->in_len == PIPE_BYTES;
    mutex_unlock(&streamed->mutex);
    return full;
}

// connection_advance moves the connection as far as it can go without
// waiting: send, then parse what has arrived, then receive more.
static void connection_advance(Connection *connection)
{
    while (!connection->closed) {
        pump_all(connection);
        if (connection->http2 != nullptr && connection->output.len == 0) {
            connection->output = http2_take_output(connection->http2);
        }
        if (connection->output.len > 0) {
            if (!send_output(connection)) {
                return;
            }
            continue;
        }
        if (waiting_on_worker(connection)) {
            if (connection->http != nullptr) {
                connection->deadline_ns = 0;
            }
            connection_watch(connection, POLL_NONE);
            return;
        }
        if (connection->http != nullptr && connection->in_flight > 0 && connection->deadline_ns == 0) {
            connection->deadline_ns = clock_monotonic_ns() + timeout_ns(connection->loop);
        }
        if (connection->closing || (connection->http2 != nullptr && http2_done(connection->http2))) {
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
        .http = http1_create(loop->server->config.limits,
                             loop->server->config.stream_handler != nullptr ? receive_body : nullptr, connection),
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

static void respond(HttpExchange *exchange)
{
    Connection *connection = exchange->connection;
    Loop *loop = connection->loop;
    const HttpServerResponse *response = exchange->failed ? nullptr : &exchange->response;
    connection->in_flight--;
    // The exchange lives in the stream's arena, which answering may free.
    if (exchange->stream != nullptr) {
        http2_respond(connection->http2, exchange->stream, response);
    } else {
        bool open = http1_respond(connection->http, response, loop->stopping);
        connection->output = http1_take_output(connection->http);
        connection->closing = !open;
    }
    if (connection->closed) {
        return;
    }
    if (connection->in_flight == 0) {
        connection->deadline_ns = clock_monotonic_ns() + timeout_ns(loop);
    }
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
        if (connection->http2 != nullptr) {
            // Requests already started finish; no new ones are read.
            http2_shutdown(connection->http2);
            connection->closing = true;
            connection_advance(connection);
        } else if (connection->in_flight == 0 && connection->output.len == 0 && connection->pending.len == 0 &&
                   http1_idle(connection->http)) {
            connection_close(connection);
        }
    }
}

static void take_done(Loop *loop)
{
    void *item = nullptr;
    while (queue_pop_timeout(loop->done, 0, &item)) {
        HttpExchange *exchange = item;
        if (item == loop) {
            stop(loop);
        } else if (exchange->stream_handler == nullptr) {
            respond(exchange);
        } else {
            Connection *connection = exchange->connection;
            mutex_lock(&exchange->mutex);
            exchange->queued = false;
            mutex_unlock(&exchange->mutex);
            pump(exchange);
            if (!connection->closed) {
                connection_advance(connection);
            }
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
    while (!loop->stopping || loop->connection_count > 0 || loop->closed != nullptr) {
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
    assert(config->handler != nullptr || config->stream_handler != nullptr);
    HttpServerConfig resolved = *config;
    resolved.address = str_copy(arena, config->address);
    HttpServerLimits *limits = &resolved.limits;
    limits->max_header_bytes = limits->max_header_bytes != 0 ? limits->max_header_bytes : DEFAULT_LIMITS.max_header_bytes;
    limits->max_body_bytes = limits->max_body_bytes != 0 ? limits->max_body_bytes : DEFAULT_LIMITS.max_body_bytes;
    limits->max_connections = limits->max_connections != 0 ? limits->max_connections : DEFAULT_LIMITS.max_connections;
    limits->timeout_ms = limits->timeout_ms > 0 ? limits->timeout_ms : DEFAULT_LIMITS.timeout_ms;
    resolved.loop_count = resolved.loop_count != 0 ? resolved.loop_count : 1;
    if (config->compression != nullptr) {
        HttpCompression *compression = arena_push(arena, sizeof *compression);
        *compression = *config->compression;
        resolved.compression = compression;
    }
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
    assert(config->handler != nullptr);
    HttpServerConfig resolved = config_with_defaults(scratch, config);
    Http1 *http = http1_create(resolved.limits, nullptr, nullptr);
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
                if (e == ERR_OK && resolved.compression != nullptr) {
                    http_compress(http1_arena(http), http1_request(http), &response, resolved.compression);
                }
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

const HttpServerRequest *http_exchange_request(const HttpExchange *exchange)
{
    return &exchange->request;
}

Arena *http_exchange_arena(HttpExchange *exchange)
{
    return exchange->arena;
}

Error http_exchange_read(HttpExchange *exchange, char *buffer, size_t capacity, size_t *got, Err *err)
{
    assert(capacity > 0);
    mutex_lock(&exchange->mutex);
    if (!exchange->continue_wanted) {
        exchange->continue_wanted = true;
        notify(exchange);
    }
    while (exchange->in_len == 0 && !exchange->in_ended && exchange->in_error == ERR_OK && !exchange->gone) {
        cond_wait(&exchange->changed, &exchange->mutex);
    }
    size_t count = exchange->in_len < capacity ? exchange->in_len : capacity;
    memcpy(buffer, exchange->in, count);
    memmove(exchange->in, exchange->in + count, exchange->in_len - count);
    exchange->in_len -= count;
    Error e = ERR_OK;
    if (count > 0) {
        notify(exchange);
    } else if (!exchange->in_ended) {
        e = exchange->in_error != ERR_OK ? exchange->in_error : ERR_NETWORK;
    }
    mutex_unlock(&exchange->mutex);
    *got = count;
    if (e == ERR_PARSE) {
        return err_set(err, e, "the request body is malformed");
    }
    return e == ERR_OK ? ERR_OK : err_set(err, e, "the client went away");
}

Error http_exchange_start(HttpExchange *exchange, unsigned status, const HttpHeader *headers, size_t header_count,
                          int64_t content_length, Err *err)
{
    assert(!exchange->head_ready);
    if (!http1_fields_valid(status, headers, header_count)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "a response field cannot go out");
    }
    // The loop reads the copy while the handler goes on.
    HttpHeader *copy = arena_push(exchange->arena, header_count * sizeof *copy);
    for (size_t i = 0; i < header_count; i++) {
        copy[i] = (HttpHeader){ str_copy(exchange->arena, headers[i].name), str_copy(exchange->arena, headers[i].value) };
    }
    exchange->bodyless = str_equal(exchange->request.method, S("HEAD")) || status == 204 || status == 304;
    mutex_lock(&exchange->mutex);
    exchange->status = status;
    exchange->headers = copy;
    exchange->header_count = header_count;
    exchange->content_length = content_length;
    exchange->head_ready = true;
    notify(exchange);
    mutex_unlock(&exchange->mutex);
    return ERR_OK;
}

Error http_exchange_write(HttpExchange *exchange, String data, Err *err)
{
    assert(exchange->head_ready);
    if (exchange->bodyless) {
        return ERR_OK;
    }
    if (exchange->content_length >= 0 && (int64_t)data.len > exchange->content_length - exchange->written) {
        return err_set(err, ERR_INVALID_ARGUMENT, "more body than the %lld bytes announced",
                       (long long)exchange->content_length);
    }
    exchange->written += (int64_t)data.len;
    mutex_lock(&exchange->mutex);
    while (data.len > 0 && !exchange->gone) {
        if (exchange->out_len == PIPE_BYTES) {
            cond_wait(&exchange->changed, &exchange->mutex);
            continue;
        }
        size_t room = PIPE_BYTES - exchange->out_len;
        size_t count = data.len < room ? data.len : room;
        memcpy(exchange->out + exchange->out_len, data.data, count);
        exchange->out_len += count;
        data = str_slice(data, count, data.len);
        notify(exchange);
    }
    bool gone = exchange->gone;
    mutex_unlock(&exchange->mutex);
    return gone ? err_set(err, ERR_NETWORK, "the client went away") : ERR_OK;
}
