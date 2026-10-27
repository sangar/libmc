#pragma once

#include "mc/net/http_server.h"

// Http1 is one HTTP/1.1 connection without the socket: bytes from the client
// go in, requests and the bytes to send come out. It parses with llhttp.
typedef struct Http1 Http1;

typedef enum Http1Event {
    HTTP1_MORE,    // every byte was used; feed more
    HTTP1_REQUEST, // a request is complete; respond before feeding more
    HTTP1_CLOSE,   // send the output, then close
} Http1Event;

// http1_create takes limits with every default already applied. The caller owns it.
Http1 *http1_create(HttpServerLimits limits);
void http1_destroy(Http1 *http);

// http1_feed parses data, empty when the client closed, and stores in *used
// how much of it it took; the rest is the next request's.
Http1Event http1_feed(Http1 *http, String data, size_t *used);

// http1_request is the complete request after HTTP1_REQUEST, in http1_arena.
const HttpServerRequest *http1_request(const Http1 *http);
// http1_arena holds the request and the response until the next request starts.
Arena *http1_arena(Http1 *http);

// http1_respond answers the request with response, or an empty 500 when it is
// nullptr, and reports whether the connection stays open. close asks for it
// to close after this response.
bool http1_respond(Http1 *http, const HttpServerResponse *response, bool close);

// http1_take_output is what to send since the last take; it stays valid until
// the next request starts, which feeding after it is sent may do.
String http1_take_output(Http1 *http);

// http1_idle says whether the connection is between requests with no part of
// the next one received.
bool http1_idle(const Http1 *http);
