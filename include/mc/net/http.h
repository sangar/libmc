#pragma once

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

// An HTTP client over TLS, for calls to web APIs: libcurl on mbedTLS, nghttp2
// and zlib, pinned in deps.lock. It speaks HTTP/2 where the server offers it
// and HTTP/1.1 otherwise. Redirects are followed by the client itself, so
// every hop is checked against the allowed hosts and schemes.

typedef struct HttpHeader {
    String name;
    String value;
} HttpHeader;

// HttpClientConfig holds what applies to every request. Zero values take the
// default named beside each field.
typedef struct HttpClientConfig {
    // ca_file is a PEM bundle of trusted roots; empty uses the system's,
    // http_default_ca_file, or Mozilla's roots built into libmc where the
    // system has none, as in minimal containers.
    String ca_file;
    // user_agent is sent with every request; many APIs refuse a generic one.
    String user_agent;
    // allowed_hosts are the host names, compared ignoring case, that requests
    // and redirects may go to; none allows every host.
    const String *allowed_hosts;
    size_t allowed_host_count;
    // connect_timeout_ms bounds connecting and the TLS handshake: 10 s.
    int64_t connect_timeout_ms;
    // timeout_ms bounds each request from start to the last byte, per hop: 30 s.
    int64_t timeout_ms;
    // max_response_bytes bounds a response body: 8 MB.
    size_t max_response_bytes;
    // max_redirects bounds the hops followed: 5.
    size_t max_redirects;
    // allow_plain_http admits http:// URLs, for tests against a local server.
    bool allow_plain_http;
} HttpClientConfig;

typedef struct HttpRequest {
    // method is GET, HEAD, POST, PUT, PATCH or DELETE; empty is GET.
    String method;
    String url;
    const HttpHeader *headers;
    size_t header_count;
    String body;
} HttpRequest;

// HttpResponse is the final response, after any redirects, in the arena
// given to http_send.
typedef struct HttpResponse {
    int status;
    HttpHeader *headers;
    size_t header_count;
    String body;
    // url is where the final response came from.
    String url;
} HttpResponse;

// HttpClient keeps connections open between requests. One thread uses it at
// a time; give each thread its own.
typedef struct HttpClient HttpClient;

// http_default_ca_file is the system's bundle of trusted roots, such as
// /etc/ssl/certs/ca-certificates.crt on Debian; empty when none is found.
String http_default_ca_file(void);

// http_client_create copies config. A ca_file that does not exist is ERR_NOT_FOUND.
[[nodiscard]] Error http_client_create(const HttpClientConfig *config, HttpClient **client, Err *err);
void http_client_destroy(HttpClient *client);

// http_send performs request and fills response in arena. Any status is an
// answer, so a 404 or 503 is ERR_OK with that status. A malformed URL, an
// unsupported method or scheme, or a host that is not allowed, also on a
// redirect, is ERR_INVALID_ARGUMENT; a timeout ERR_TIMEOUT; a body over
// max_response_bytes ERR_UNSUPPORTED; anything else that prevents an answer,
// such as DNS, connection or certificate failures and too many redirects,
// ERR_NETWORK. 301, 302 and 303 redirects continue as GET without the body;
// 307 and 308 repeat the method and body. The client asks for gzip and
// deflate unless the request sets Accept-Encoding, and decodes them: the body
// and its limit are the decoded bytes, while the headers are as sent.
[[nodiscard]] Error http_send(HttpClient *client, Arena *arena, const HttpRequest *request, HttpResponse *response,
                              Err *err);

// http_response_header finds the first response header called name, ignoring ASCII case.
bool http_response_header(const HttpResponse *response, String name, String *value);
