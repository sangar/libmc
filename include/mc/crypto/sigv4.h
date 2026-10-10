#pragma once

#include "mc/text/str.h"

// SigV4Request is the part of a request that AWS Signature Version 4 signs.
// path and query must already be canonical: URI-encoded, with the query
// parameters sorted by name.
typedef struct SigV4Request {
    String method;
    String path;
    String query;
    String host;     // the Host header, with the port when it is not the default
    String amz_date; // the X-Amz-Date header, from sigv4_date
    // headers are further signed headers as "Name: value", besides Host and X-Amz-Date.
    StringList headers;
    String payload_hash; // sigv4_payload_hash of the body
} SigV4Request;

typedef struct SigV4Credentials {
    String access_key;
    String secret_key;
    String region;
    String service;
} SigV4Credentials;

// sigv4_authorization returns the value of the Authorization header.
String sigv4_authorization(Arena *arena, SigV4Request request, SigV4Credentials credentials);
String sigv4_payload_hash(Arena *arena, String payload);
// sigv4_date writes a Unix time the way X-Amz-Date wants it: 20150830T123600Z.
String sigv4_date(Arena *arena, int64_t unix_seconds);
