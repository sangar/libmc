#pragma once

#include <stdint.h>

#include "mc/net/http_server.h"

// Http2 is one HTTP/2 connection without the socket, as Http1 is for
// HTTP/1.1: bytes from the client go in, complete requests and the bytes to
// send come out. It parses with nghttp2. Several requests may be open at
// once, each on a stream of its own.
typedef struct Http2 Http2;
typedef struct Http2Stream Http2Stream;

// Http2Request is a complete request. Its arena holds it, and the response
// until it is sent.
typedef struct Http2Request {
    Http2Stream *stream;
    Arena *arena;
    const HttpServerRequest *request;
} Http2Request;

// http2_create takes limits with every default already applied, and queues
// the server's settings to send. When streaming, a request comes at the end
// of its head, its body is read with http2_body, with no limit on its size,
// and it is answered with http2_start, http2_write and http2_end. The caller
// owns it, and destroys it only once every request it gave out has been
// answered.
Http2 *http2_create(HttpServerLimits limits, bool streaming);
void http2_destroy(Http2 *http);

// http2_feed parses all of data, empty when the client closed, and reports
// whether the connection stays open.
bool http2_feed(Http2 *http, String data);

// http2_next_request gives the next request that came complete, if any.
bool http2_next_request(Http2 *http, Http2Request *request);

// http2_respond answers stream with response, or an empty 500 when it is
// nullptr. A stream the client has reset meanwhile is only released.
void http2_respond(Http2 *http, Http2Stream *stream, const HttpServerResponse *response);

// http2_body is the part of a streamed body received and not yet taken.
String http2_body(const Http2Stream *stream);
// http2_body_take takes count bytes of it, which lets the client send as many more.
void http2_body_take(Http2 *http, Http2Stream *stream, size_t count);
// http2_body_ended says whether the body was taken to its end.
bool http2_body_ended(const Http2Stream *stream);
// http2_stream_closed says whether the stream ended early, as when the client reset it.
bool http2_stream_closed(const Http2Stream *stream);

// http2_start begins a streamed response, with fields http1_fields_valid
// takes, of content_length bytes, or unknown when it is negative.
void http2_start(Http2 *http, Http2Stream *stream, unsigned status, const HttpHeader *headers, size_t header_count,
                 int64_t content_length);
// http2_writable says whether the response's last piece went out, so the next may come.
bool http2_writable(const Http2Stream *stream);
// http2_write adds data to the response's body.
void http2_write(Http2 *http, Http2Stream *stream, String data);
// http2_end ends the response, or resets the stream unless complete, and
// releases the stream as http2_respond does.
void http2_end(Http2 *http, Http2Stream *stream, bool complete);

// http2_take_output is what to send since the last take; it stays valid until the next.
String http2_take_output(Http2 *http);

// http2_shutdown tells the client no new streams will be served, and lets
// the open ones finish.
void http2_shutdown(Http2 *http);

// http2_done says whether the connection has nothing more to read or send.
bool http2_done(Http2 *http);
