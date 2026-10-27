#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/net/http.h"
#include "mc/net/tls.h"
#include "mc/text/str.h"

// An HTTP/1.1 server for web applications, over TLS when given a TlsServer,
// and HTTP/2 when the TLS handshake chooses it, with nghttp2.
// Event loops own the connections and parse requests with llhttp; a pool of
// worker threads runs the handler,
// so a handler may block, as on a database query, without holding up other
// connections. Requests are refused unless they are unambiguous: one Host,
// no Content-Length beside Transfer-Encoding, single spaces, CRLF line ends.

// HttpServerRequest is one complete request. Its views live in the arena the
// handler gets.
typedef struct HttpServerRequest {
    String method;
    String target; // as sent: the origin form, or "*"
    String path;   // target before any '?'
    String query;  // target after the first '?', without it
    // headers are the fields as sent, in order; trailer fields are dropped.
    const HttpHeader *headers;
    size_t header_count;
    String body; // decoded when chunked
    unsigned version_major; // 1, or 2 for HTTP/2
    unsigned version_minor; // HTTP/1.0 or HTTP/1.1; 0 for HTTP/2
} HttpServerRequest;

// HttpServerResponse comes to the handler as 200 with no headers or body. The
// server adds Date, Content-Length and Connection, and leaves the body out
// for HEAD, 204 and 304; a handler setting any of those three fields is a
// programmer error. A field name that is not a token, or a value holding CR,
// LF or NUL, makes the response a 500.
typedef struct HttpServerResponse {
    unsigned status;
    HttpHeader *headers;
    size_t header_count;
    String body;
} HttpServerResponse;

// HttpHandler answers request in response, allocating only from arena, which
// lives until the response is sent. Several run at once on worker threads, so
// context must be safe to share. An error is answered with an empty 500.
typedef Error (*HttpHandler)(void *context, Arena *arena, const HttpServerRequest *request,
                             HttpServerResponse *response, Err *err);

// HttpServerLimits bound what a client can make the server hold. Zero takes
// the default beside each field.
typedef struct HttpServerLimits {
    size_t max_header_bytes; // request line and fields: 8 KiB, else 431 or 414
    size_t max_body_bytes;   // 1 MiB, else 413
    size_t max_connections;  // 1024; more wait in the listen backlog
    // timeout_ms bounds each wait on the client: from the connection or the
    // last response to a complete request, and between bytes of a response
    // the client is slow to read: 30 s. A handler has no limit.
    int64_t timeout_ms;
} HttpServerLimits;

typedef struct HttpServerConfig {
    String address; // host:port as net_listen takes it; port 0 picks a free one
    HttpServerLimits limits;
    size_t loop_count;   // event loop threads: 1
    size_t worker_count; // handler threads: one per CPU
    HttpHandler handler;
    void *context;
    // tls, when set, serves every connection over TLS with it; the server
    // borrows it until http_server_destroy. http_server_exchange ignores it.
    TlsServer *tls;
} HttpServerConfig;

typedef struct HttpServer HttpServer;

// http_server_create listens on config->address and serves until
// http_server_destroy. The caller owns the server.
[[nodiscard]] Error http_server_create(const HttpServerConfig *config, HttpServer **server, Err *err);
uint16_t http_server_port(const HttpServer *server);
// http_server_destroy stops accepting, closes idle connections, lets requests
// already started finish within the timeout, and waits for their handlers.
void http_server_destroy(HttpServer *server);

// http_server_exchange runs config's handler on this thread, with no socket,
// for one connection on which a client sends each piece in turn and then
// closes. *answer is every byte the server answered, in arena.
[[nodiscard]] Error http_server_exchange(Arena *arena, const HttpServerConfig *config, StringList pieces,
                                         String *answer, Err *err);
