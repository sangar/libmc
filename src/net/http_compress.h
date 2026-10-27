#pragma once

#include "mc/net/http_server.h"

// http_compress gzips response's body, in arena, as compression says.
void http_compress(Arena *arena, const HttpServerRequest *request, HttpServerResponse *response,
                   const HttpCompression *compression);

// http_accepts_gzip reads Accept-Encoding (RFC 9110 12.5.3): gzip named and
// not refused with q=0, or *, when gzip is not named, and not refused.
bool http_accepts_gzip(Arena *arena, const HttpServerRequest *request);

// http_compressible says whether a Content-Type is text or another kind
// that gzip shrinks: JSON, JavaScript, XML, SVG, WebAssembly.
bool http_compressible(String content_type);
