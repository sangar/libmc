#pragma once

#include <stdint.h>

#include "mc/net/http_server.h"

// Http1 is one HTTP/1.1 connection without the socket: bytes from the client
// go in, requests and the bytes to send come out. It parses with llhttp.
typedef struct Http1 Http1;

typedef enum Http1Event {
    HTTP1_MORE,     // every byte was used; feed more
    HTTP1_REQUEST,  // a request is ready: complete, or its head when the body streams
    HTTP1_BODY_END, // a streamed body ended; respond before feeding more
    HTTP1_CLOSE,    // send the output, then close
} Http1Event;

// Http1BodySink takes a streamed request's body as it is parsed.
typedef void (*Http1BodySink)(void *context, String piece);

// http1_create takes limits with every default already applied. Given a
// sink, it streams request bodies: a request comes at the end of its head,
// its body goes to sink as it is parsed, with no limit on its size, and the
// body's end is HTTP1_BODY_END. The caller owns it.
Http1 *http1_create(HttpServerLimits limits, Http1BodySink sink, void *sink_context);
void http1_destroy(Http1 *http);

// http1_feed parses data, empty when the client closed, and stores in *used
// how much of it it took; the rest is the next request's. While a streamed
// body is being read it may be fed before the response is done.
Http1Event http1_feed(Http1 *http, String data, size_t *used);

// http1_request is the request after HTTP1_REQUEST, in http1_arena; a
// streamed one has an empty body.
const HttpServerRequest *http1_request(const Http1 *http);
// http1_arena holds the request and the response until the next request starts.
Arena *http1_arena(Http1 *http);

// http1_body_complete says whether the request's body has been read to its end.
bool http1_body_complete(const Http1 *http);

// http1_continue tells a client that sent Expect: 100-continue to send the
// body, unless it was told or the response has started.
void http1_continue(Http1 *http);

// http1_respond answers the request with response, or an empty 500 when it is
// nullptr, and reports whether the connection stays open. close asks for it
// to close after this response.
bool http1_respond(Http1 *http, const HttpServerResponse *response, bool close);

// http1_fields_valid says whether a response's fields can go out: a status
// from 200 to 599, token names and values without CR, LF or NUL. Fields the
// server sets itself are a programmer error.
bool http1_fields_valid(unsigned status, const HttpHeader *headers, size_t header_count);

// http1_start begins a response, with valid fields, whose body comes in
// pieces: content_length bytes, or chunked when it is negative, or until the
// connection closes on HTTP/1.0. Begun before the request's body was read to
// its end, it closes the connection after.
void http1_start(Http1 *http, unsigned status, const HttpHeader *headers, size_t header_count, int64_t content_length,
                 bool close);
// http1_write adds data to the body, unless the response has none, as for HEAD.
void http1_write(Http1 *http, String data);
// http1_end ends the response and reports whether the connection stays open.
// Unless complete, as when the body fell short of its length, it closes.
bool http1_end(Http1 *http, bool complete);

// http1_take_output is what to send since the last take. It stays valid until
// it is sent and output is added, which the caller does only after.
String http1_take_output(Http1 *http);

// http1_idle says whether the connection is between requests with no part of
// the next one received.
bool http1_idle(const Http1 *http);
