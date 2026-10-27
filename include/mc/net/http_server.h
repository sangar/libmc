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

// http_query_param finds the first parameter called name in query, a request's
// query without its '?', and decodes its value as a form does: '+' is a
// space and %XX a byte, while a '%' that begins no escape stays as it is.
// "?uploads" has the parameter uploads with an empty value.
bool http_query_param(Arena *arena, String query, String name, String *value);

typedef enum HttpRangeResult {
    HTTP_RANGE_NONE,          // no range to honour: answer the whole body with 200
    HTTP_RANGE_SATISFIABLE,   // answer [first, last] with 206 and Content-Range
    HTTP_RANGE_UNSATISFIABLE, // answer 416
} HttpRangeResult;

// http_parse_range reads a Range field of a single bytes range, such as
// bytes=0-499, bytes=500- or bytes=-500, against a body of size bytes into
// the inclusive [*first, *last], cut to the body. Several ranges, another
// unit or a malformed field are HTTP_RANGE_NONE, which RFC 9110 14.2 lets a
// server answer with the whole body.
HttpRangeResult http_parse_range(String field, int64_t size, int64_t *first, int64_t *last);

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

// HttpExchange is one request whose body and response stream, for a handler
// that reads an upload as it arrives or sends a body it never holds whole.
// Its functions are for the handler's thread, until the handler returns.
typedef struct HttpExchange HttpExchange;

// HttpStreamHandler answers the request in exchange: it may read the body
// with http_exchange_read, then starts the response with http_exchange_start
// and sends its body with http_exchange_write. Returning without starting is
// answered with an empty 500, as is an error before starting, or 400 when
// the client framed the body wrongly; an error after
// it, or a body shorter than announced, ends the connection on HTTP/1.1 or
// resets the stream on HTTP/2, so the client sees the response was cut short.
typedef Error (*HttpStreamHandler)(void *context, HttpExchange *exchange, Err *err);

// http_exchange_request is the request with an empty body.
const HttpServerRequest *http_exchange_request(const HttpExchange *exchange);
// http_exchange_arena is the handler's to allocate from until it returns.
Arena *http_exchange_arena(HttpExchange *exchange);

// http_exchange_read waits for the next piece of the body, stores up to
// capacity bytes of it in buffer and their count in *got, which is 0 once the
// body has ended. The first read tells a client that sent Expect:
// 100-continue to send the body, so a handler can refuse an upload unread.
// Fails with ERR_NETWORK when the client went away and ERR_PARSE when it
// framed the body wrongly. Until it is read, the client is held back.
[[nodiscard]] Error http_exchange_read(HttpExchange *exchange, char *buffer, size_t capacity, size_t *got, Err *err);

// http_exchange_start sends the response's head: status and headers as
// HttpServerResponse takes them, with a body of content_length bytes, or of a
// length not known yet when it is negative, which goes chunked on HTTP/1.1 and
// until the connection closes on HTTP/1.0. HEAD, 204 and 304 get the head
// alone, with Content-Length still as given. A field that cannot go out fails
// with ERR_INVALID_ARGUMENT and starts nothing.
[[nodiscard]] Error http_exchange_start(HttpExchange *exchange, unsigned status, const HttpHeader *headers,
                                        size_t header_count, int64_t content_length, Err *err);
// http_exchange_write sends data as the next piece of the body, waiting
// while the client is slow to read. More than content_length fails with
// ERR_INVALID_ARGUMENT, and a client gone with ERR_NETWORK; a response
// without a body drops it.
[[nodiscard]] Error http_exchange_write(HttpExchange *exchange, String data, Err *err);

// HttpServerLimits bound what a client can make the server hold. Zero takes
// the default beside each field.
typedef struct HttpServerLimits {
    size_t max_header_bytes; // request line and fields: 8 KiB, else 431 or 414
    size_t max_body_bytes;   // 1 MiB, else 413; streamed bodies have no limit
    size_t max_connections;  // 1024; more wait in the listen backlog
    // timeout_ms bounds each wait on the client: from the connection or the
    // last response to a complete request, between bytes of a streamed body
    // on HTTP/1.1, and between bytes of a response the client is slow to
    // read: 30 s. A handler has no limit.
    int64_t timeout_ms;
} HttpServerLimits;

// HttpCompression is how a server gzips responses, after Basecamp's
// Thruster: when the client accepts gzip, the response is not encoded
// already, its Content-Type is text or another kind that compresses well
// (JSON, JavaScript, XML, SVG, WebAssembly), its body is at least min_bytes
// and its status carries one, and compressing makes it smaller. It then adds
// Content-Encoding and Vary. The worker compresses after the handler.
typedef struct HttpCompression {
    size_t min_bytes; // smaller bodies go as they are
    // jitter_bytes pads each gzipped response with 1 to this many random
    // bytes, at most 254, in the gzip header, against BREACH, which reads
    // secrets from compressed sizes; 0 pads nothing.
    size_t jitter_bytes;
    // skip_credentials leaves responses to requests carrying Cookie,
    // Authorization or X-Csrf-Token uncompressed, as such responses are the
    // ones that hold secrets.
    bool skip_credentials;
} HttpCompression;

typedef struct HttpServerConfig {
    String address; // host:port as net_listen takes it; port 0 picks a free one
    HttpServerLimits limits;
    size_t loop_count;   // event loop threads: 1
    size_t worker_count; // handler threads: one per CPU
    HttpHandler handler;
    // stream_handler, when set, answers every request instead of handler.
    // Responses it sends are not compressed.
    HttpStreamHandler stream_handler;
    void *context;
    // tls, when set, serves every connection over TLS with it; the server
    // borrows it until http_server_destroy. http_server_exchange ignores it.
    TlsServer *tls;
    const HttpCompression *compression; // copied; nullptr compresses nothing
} HttpServerConfig;

typedef struct HttpServer HttpServer;

// http_server_create listens on config->address and serves until
// http_server_destroy. The caller owns the server.
[[nodiscard]] Error http_server_create(const HttpServerConfig *config, HttpServer **server, Err *err);
uint16_t http_server_port(const HttpServer *server);
// http_server_destroy stops accepting, closes idle connections, lets requests
// already started finish within the timeout, and waits for their handlers.
void http_server_destroy(HttpServer *server);

// http_server_exchange runs config's handler, not a stream_handler, on this thread, with no socket,
// for one connection on which a client sends each piece in turn and then
// closes. Pieces that begin with HTTP/2's connection preface are spoken to
// as HTTP/2 is with prior knowledge, which a server on a socket does only
// after ALPN chose it. *answer is every byte the server answered, in arena.
[[nodiscard]] Error http_server_exchange(Arena *arena, const HttpServerConfig *config, StringList pieces,
                                         String *answer, Err *err);
