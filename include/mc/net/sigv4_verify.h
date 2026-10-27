#pragma once

#include "mc/net/http.h"

// Checking requests signed with AWS Signature Version 4, as an S3-compatible
// server does: in the Authorization header, or in a presigned URL's query.
// The path is made canonical as S3 does, decoded and encoded once; services
// that encode it twice are not covered.

// SigV4Received is a request as a server received it.
typedef struct SigV4Received {
    String method;
    String path;  // as sent, percent-encoded
    String query; // as sent, without the '?'
    const HttpHeader *headers; // names in any case
    size_t header_count;
} SigV4Received;

typedef enum SigV4Problem {
    SIGV4_OK,
    SIGV4_MISSING,   // neither an Authorization header nor a presigned query
    SIGV4_MALFORMED, // a signature that cannot be read, or a signed header missing
    SIGV4_MISMATCH,  // a signature the secret did not make
    SIGV4_EXPIRED,   // a presigned URL past its X-Amz-Expires
    SIGV4_SKEWED,    // a header signature more than 15 minutes from the clock
} SigV4Problem;

// SigV4Payload is what x-amz-content-sha256 says of the body.
typedef enum SigV4Payload {
    SIGV4_PAYLOAD_SHA256, // the body's hash, for the caller to compare once it has the body
    SIGV4_PAYLOAD_UNSIGNED,
    SIGV4_PAYLOAD_STREAMING_SIGNED, // aws-chunked, each chunk signed
    SIGV4_PAYLOAD_STREAMING_SIGNED_TRAILER,
    SIGV4_PAYLOAD_STREAMING_UNSIGNED_TRAILER,
} SigV4Payload;

// SigV4Signature is what a request claims. The caller looks up the secret of
// access_key and checks that region and service are its own.
typedef struct SigV4Signature {
    String access_key;
    String date; // YYYYMMDD
    String region;
    String service;
    String signed_headers; // lowercase names joined by ';'
    String signature;
    String amz_date; // 20130524T000000Z
    int64_t expires_seconds; // presigned only, at most seven days
    bool presigned;
} SigV4Signature;

// sigv4_read finds the signature in request's Authorization header or query.
SigV4Problem sigv4_read(Arena *arena, const SigV4Received *request, SigV4Signature *signature);

// SigV4Verified is what checking the body needs.
typedef struct SigV4Verified {
    SigV4Payload payload;
    String payload_hash;   // lowercase hex when payload is SIGV4_PAYLOAD_SHA256
    String seed_signature; // the request's signature, which the first chunk's signs on from
    String signing_key;
    String amz_date;
    String scope;
} SigV4Verified;

// sigv4_verify checks signature, from sigv4_read, against secret_key at
// now_unix_seconds, the server's clock.
SigV4Problem sigv4_verify(Arena *arena, const SigV4Received *request, const SigV4Signature *signature,
                          String secret_key, int64_t now_unix_seconds, SigV4Verified *verified);

// sigv4_chunk_signature is the hex signature of one aws-chunked chunk, whose
// SHA-256 is chunk_sha256 in lowercase hex, following previous_signature.
String sigv4_chunk_signature(Arena *arena, const SigV4Verified *verified, String previous_signature,
                             String chunk_sha256);
