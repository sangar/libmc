#pragma once

#include "mc/crypto/sha256.h"
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

// SigV4BodyRead reads up to capacity bytes of the body as sent into buffer;
// *got is 0 at its end. http_exchange_read has this shape.
typedef Error (*SigV4BodyRead)(void *context, char *buffer, size_t capacity, size_t *got, Err *err);

typedef enum SigV4BodyProblem {
    SIGV4_BODY_OK,
    SIGV4_BODY_HASH_MISMATCH,      // the body is not what x-amz-content-sha256 promised
    SIGV4_BODY_SIGNATURE_MISMATCH, // a chunk's or the trailer's signature is wrong
    SIGV4_BODY_MALFORMED,          // aws-chunked framing that cannot be read, or a body cut short
} SigV4BodyProblem;

enum { SIGV4_BODY_MAX_TRAILERS = 8 };

// SigV4Body reads a verified request's body as the client meant it: the
// aws-chunked framing taken off and each chunk's signature checked, or the
// whole body's hash checked at its end. It holds 4 KiB of framing.
typedef struct SigV4Body {
    Arena *arena;
    const SigV4Verified *verified;
    SigV4BodyRead read;
    void *context;
    Sha256 body_hash;
    Sha256 chunk_hash;
    String previous_signature;
    String chunk_signature;
    uint64_t chunk_remaining;
    bool chunked;
    bool done;
    char buffer[4096];
    size_t start;
    size_t end;
    HttpHeader trailers[SIGV4_BODY_MAX_TRAILERS];
    size_t trailer_count;
    SigV4BodyProblem problem;
} SigV4Body;

// sigv4_body_init reads the body through read(context, ...); verified, from
// sigv4_verify, must outlive body.
void sigv4_body_init(SigV4Body *body, Arena *arena, const SigV4Verified *verified, SigV4BodyRead read, void *context);
// sigv4_body_read stores up to capacity bytes of the decoded body in buffer;
// *got is 0 at its end, which comes only once every check has passed. A body
// that fails one is ERR_PARSE, with body->problem saying which; an error of
// read is passed on.
[[nodiscard]] Error sigv4_body_read(SigV4Body *body, char *buffer, size_t capacity, size_t *got, Err *err);
// sigv4_body_trailer finds a trailer the client sent after an aws-chunked
// body, such as x-amz-checksum-crc32, once the body has ended.
bool sigv4_body_trailer(const SigV4Body *body, String name, String *value);
