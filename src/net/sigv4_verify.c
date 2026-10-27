#include "mc/net/sigv4_verify.h"

#include "mc/container/sort.h"
#include "mc/crypto/sigv4.h"
#include "mc/text/fmt.h"

#include <string.h>

enum {
    MAX_SKEW_SECONDS = 15 * 60,
    MAX_PRESIGN_SECONDS = 7 * 24 * 60 * 60,
    SECONDS_PER_DAY = 86400,
};

static const char ALGORITHM[] = "AWS4-HMAC-SHA256";

static String hex_sha256(Arena *arena, String data)
{
    uint8_t digest[SHA256_LEN];
    sha256(data, digest);
    return hex_encode(arena, digest, SHA256_LEN);
}

static String hex_hmac(Arena *arena, String key, String data)
{
    uint8_t digest[SHA256_LEN];
    hmac_sha256(key, data, digest);
    return hex_encode(arena, digest, SHA256_LEN);
}

// query_value finds the first parameter called name in query, decoded.
static bool query_value(Arena *arena, String query, String name, String *value)
{
    String rest = query;
    while (rest.len > 0) {
        String pair;
        str_cut(rest, '&', &pair, &rest);
        String key;
        String encoded;
        str_cut(pair, '=', &key, &encoded);
        String decoded;
        if (str_percent_decode(arena, key, &decoded) && str_equal(decoded, name)) {
            return str_percent_decode(arena, encoded, value);
        }
    }
    return false;
}

static bool header_value(const SigV4Received *request, String name, String *value)
{
    for (size_t i = 0; i < request->header_count; i++) {
        if (str_equal_ignore_case(request->headers[i].name, name)) {
            *value = request->headers[i].value;
            return true;
        }
    }
    return false;
}

static bool read_credential(String credential, SigV4Signature *signature)
{
    String rest = credential;
    String terminal;
    return str_cut(rest, '/', &signature->access_key, &rest) && str_cut(rest, '/', &signature->date, &rest) &&
           str_cut(rest, '/', &signature->region, &rest) && str_cut(rest, '/', &signature->service, &terminal) &&
           str_equal(terminal, S("aws4_request")) && signature->date.len == 8;
}

static SigV4Problem read_header(Arena *arena, String authorization, SigV4Signature *signature)
{
    String algorithm;
    String fields;
    if (!str_cut(authorization, ' ', &algorithm, &fields) || !str_equal(algorithm, S(ALGORITHM))) {
        return SIGV4_MALFORMED;
    }
    String credential = S("");
    StringList parts = str_split(arena, fields, ',');
    for (size_t i = 0; i < parts.count; i++) {
        String name;
        String value;
        if (!str_cut(str_trim(parts.items[i]), '=', &name, &value)) {
            return SIGV4_MALFORMED;
        }
        if (str_equal(name, S("Credential"))) {
            credential = value;
        } else if (str_equal(name, S("SignedHeaders"))) {
            signature->signed_headers = value;
        } else if (str_equal(name, S("Signature"))) {
            signature->signature = value;
        }
    }
    bool complete = read_credential(credential, signature) && signature->signed_headers.len > 0 &&
                    signature->signature.len > 0;
    return complete ? SIGV4_OK : SIGV4_MALFORMED;
}

static SigV4Problem read_query(Arena *arena, String query, SigV4Signature *signature)
{
    String algorithm = S("");
    String credential = S("");
    String expires = S("");
    bool complete = query_value(arena, query, S("X-Amz-Algorithm"), &algorithm) &&
                    str_equal(algorithm, S(ALGORITHM)) &&
                    query_value(arena, query, S("X-Amz-Credential"), &credential) &&
                    read_credential(credential, signature) &&
                    query_value(arena, query, S("X-Amz-Date"), &signature->amz_date) &&
                    query_value(arena, query, S("X-Amz-SignedHeaders"), &signature->signed_headers) &&
                    query_value(arena, query, S("X-Amz-Signature"), &signature->signature) &&
                    query_value(arena, query, S("X-Amz-Expires"), &expires) &&
                    str_parse_i64(expires, &signature->expires_seconds) && signature->expires_seconds >= 1 &&
                    signature->expires_seconds <= MAX_PRESIGN_SECONDS && signature->signature.len > 0;
    signature->presigned = true;
    return complete ? SIGV4_OK : SIGV4_MALFORMED;
}

SigV4Problem sigv4_read(Arena *arena, const SigV4Received *request, SigV4Signature *signature)
{
    *signature = (SigV4Signature){ 0 };
    String authorization;
    if (header_value(request, S("Authorization"), &authorization)) {
        header_value(request, S("X-Amz-Date"), &signature->amz_date);
        return read_header(arena, authorization, signature);
    }
    String query_signature;
    if (query_value(arena, request->query, S("X-Amz-Signature"), &query_signature)) {
        return read_query(arena, request->query, signature);
    }
    return SIGV4_MISSING;
}

static bool read_amz_date(String text, int64_t *unix_seconds)
{
    int64_t year;
    int64_t month;
    int64_t day;
    int64_t hour;
    int64_t minute;
    int64_t second;
    if (text.len != 16 || text.data[8] != 'T' || text.data[15] != 'Z' ||
        !str_parse_i64(str_slice(text, 0, 4), &year) || !str_parse_i64(str_slice(text, 4, 6), &month) ||
        !str_parse_i64(str_slice(text, 6, 8), &day) || !str_parse_i64(str_slice(text, 9, 11), &hour) ||
        !str_parse_i64(str_slice(text, 11, 13), &minute) || !str_parse_i64(str_slice(text, 13, 15), &second) ||
        month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    CivilDate date = { .year = year, .month = (unsigned)month, .day = (unsigned)day };
    *unix_seconds = civil_to_days(date) * SECONDS_PER_DAY + hour * 3600 + minute * 60 + second;
    return true;
}

static int compare_strings(const void *context, const void *a, const void *b)
{
    unused(context);
    return str_compare(*(const String *)a, *(const String *)b);
}

// canonical_query encodes each parameter's name and value once and sorts them,
// leaving out a presigned URL's own signature.
static bool canonical_query(Arena *arena, String query, bool presigned, String *canonical)
{
    StringList pairs = { 0 };
    StringList raw = str_split(arena, query, '&');
    for (size_t i = 0; i < raw.count; i++) {
        if (raw.items[i].len == 0) {
            continue;
        }
        String key;
        String value;
        str_cut(raw.items[i], '=', &key, &value);
        if (!str_percent_decode(arena, key, &key) || !str_percent_decode(arena, value, &value)) {
            return false;
        }
        if (presigned && str_equal(key, S("X-Amz-Signature"))) {
            continue;
        }
        String encoded_key = str_percent_encode(arena, key, false);
        String encoded_value = str_percent_encode(arena, value, false);
        strlist_push(arena, &pairs, str_format(arena, "%.*s=%.*s", (int)encoded_key.len, encoded_key.data,
                                               (int)encoded_value.len, encoded_value.data));
    }
    sort_stable(arena, pairs.items, pairs.count, sizeof(String), compare_strings, nullptr);
    *canonical = str_join(arena, pairs, S("&"));
    return true;
}

// canonical_headers writes each signed header as name:value, the values of a
// repeated name joined by commas, each trimmed with its runs of spaces made one.
static bool canonical_headers(Arena *arena, const SigV4Received *request, String signed_headers, String *canonical)
{
    StringBuilder out = str_builder_create(arena, 512);
    StringList names = str_split(arena, signed_headers, ';');
    for (size_t n = 0; n < names.count; n++) {
        StringList values = { 0 };
        for (size_t i = 0; i < request->header_count; i++) {
            if (str_equal_ignore_case(request->headers[i].name, names.items[n])) {
                strlist_push(arena, &values, str_join(arena, str_fields(arena, request->headers[i].value), S(" ")));
            }
        }
        if (values.count == 0) {
            return false;
        }
        String joined = str_join(arena, values, S(","));
        str_builder_append_format(&out, "%.*s:%.*s\n", (int)names.items[n].len, names.items[n].data, (int)joined.len,
                                  joined.data);
    }
    *canonical = str_builder_finish(&out);
    return true;
}

static bool is_sha256_hex(String s)
{
    if (s.len != 2 * SHA256_LEN) {
        return false;
    }
    for (size_t i = 0; i < s.len; i++) {
        char c = s.data[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool read_payload(String hash, SigV4Payload *payload)
{
    static const struct {
        const char *name;
        SigV4Payload payload;
    } PAYLOADS[] = {
        { "UNSIGNED-PAYLOAD", SIGV4_PAYLOAD_UNSIGNED },
        { "STREAMING-AWS4-HMAC-SHA256-PAYLOAD", SIGV4_PAYLOAD_STREAMING_SIGNED },
        { "STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER", SIGV4_PAYLOAD_STREAMING_SIGNED_TRAILER },
        { "STREAMING-UNSIGNED-PAYLOAD-TRAILER", SIGV4_PAYLOAD_STREAMING_UNSIGNED_TRAILER },
    };
    for (size_t i = 0; i < countof(PAYLOADS); i++) {
        if (str_equal(hash, S(PAYLOADS[i].name))) {
            *payload = PAYLOADS[i].payload;
            return true;
        }
    }
    *payload = SIGV4_PAYLOAD_SHA256;
    return is_sha256_hex(hash);
}

static bool equal_constant_time(String a, String b)
{
    if (a.len != b.len) {
        return false;
    }
    unsigned char difference = 0;
    for (size_t i = 0; i < a.len; i++) {
        difference |= (unsigned char)(a.data[i] ^ b.data[i]);
    }
    return difference == 0;
}

static SigV4Problem check_time(const SigV4Signature *signature, int64_t now)
{
    int64_t signed_at;
    if (!read_amz_date(signature->amz_date, &signed_at) || !str_starts_with(signature->amz_date, signature->date)) {
        return SIGV4_MALFORMED;
    }
    if (signature->presigned) {
        return now > signed_at + signature->expires_seconds ? SIGV4_EXPIRED : SIGV4_OK;
    }
    return now - signed_at > MAX_SKEW_SECONDS || signed_at - now > MAX_SKEW_SECONDS ? SIGV4_SKEWED : SIGV4_OK;
}

SigV4Problem sigv4_verify(Arena *arena, const SigV4Received *request, const SigV4Signature *signature,
                          String secret_key, int64_t now_unix_seconds, SigV4Verified *verified)
{
    SigV4Problem problem = check_time(signature, now_unix_seconds);
    if (problem != SIGV4_OK) {
        return problem;
    }
    String payload_hash = S("UNSIGNED-PAYLOAD");
    if (!signature->presigned && !header_value(request, S("X-Amz-Content-Sha256"), &payload_hash)) {
        return SIGV4_MALFORMED;
    }
    SigV4Payload payload;
    String path;
    String query;
    String headers;
    if (!read_payload(payload_hash, &payload) || !str_percent_decode(arena, request->path, &path) ||
        !canonical_query(arena, request->query, signature->presigned, &query) ||
        !canonical_headers(arena, request, signature->signed_headers, &headers)) {
        return SIGV4_MALFORMED;
    }
    path = str_percent_encode(arena, path, true);
    String canonical = str_format(arena, "%.*s\n%.*s\n%.*s\n%.*s\n%.*s\n%.*s", (int)request->method.len,
                                  request->method.data, (int)path.len, path.data, (int)query.len, query.data,
                                  (int)headers.len, headers.data, (int)signature->signed_headers.len,
                                  signature->signed_headers.data, (int)payload_hash.len, payload_hash.data);
    String scope = str_format(arena, "%.*s/%.*s/%.*s/aws4_request", (int)signature->date.len, signature->date.data,
                              (int)signature->region.len, signature->region.data, (int)signature->service.len,
                              signature->service.data);
    String canonical_hash = hex_sha256(arena, canonical);
    String to_sign = str_format(arena, "%s\n%.*s\n%.*s\n%.*s", ALGORITHM, (int)signature->amz_date.len,
                                signature->amz_date.data, (int)scope.len, scope.data, (int)canonical_hash.len,
                                canonical_hash.data);
    String key = sigv4_signing_key(arena, secret_key, signature->date, signature->region, signature->service);
    String expected = hex_hmac(arena, key, to_sign);
    if (!equal_constant_time(expected, signature->signature)) {
        return SIGV4_MISMATCH;
    }
    *verified = (SigV4Verified){
        .payload = payload,
        .payload_hash = payload_hash,
        .seed_signature = expected,
        .signing_key = key,
        .amz_date = signature->amz_date,
        .scope = scope,
    };
    return SIGV4_OK;
}

String sigv4_chunk_signature(Arena *arena, const SigV4Verified *verified, String previous_signature,
                             String chunk_sha256)
{
    String empty_hash = hex_sha256(arena, S(""));
    String to_sign = str_format(arena, "AWS4-HMAC-SHA256-PAYLOAD\n%.*s\n%.*s\n%.*s\n%.*s\n%.*s",
                                (int)verified->amz_date.len, verified->amz_date.data, (int)verified->scope.len,
                                verified->scope.data, (int)previous_signature.len, previous_signature.data,
                                (int)empty_hash.len, empty_hash.data, (int)chunk_sha256.len, chunk_sha256.data);
    return hex_hmac(arena, verified->signing_key, to_sign);
}

static bool signs_chunks(const SigV4Verified *verified)
{
    return verified->payload == SIGV4_PAYLOAD_STREAMING_SIGNED ||
           verified->payload == SIGV4_PAYLOAD_STREAMING_SIGNED_TRAILER;
}

void sigv4_body_init(SigV4Body *body, Arena *arena, const SigV4Verified *verified, SigV4BodyRead read, void *context)
{
    *body = (SigV4Body){
        .arena = arena,
        .verified = verified,
        .read = read,
        .context = context,
        .previous_signature = verified->seed_signature,
        .chunked = signs_chunks(verified) || verified->payload == SIGV4_PAYLOAD_STREAMING_UNSIGNED_TRAILER,
    };
    sha256_init(&body->body_hash);
}

[[nodiscard]] static Error fail(SigV4Body *body, SigV4BodyProblem problem, Err *err)
{
    static const char *const WHY[] = {
        [SIGV4_BODY_HASH_MISMATCH] = "the body does not have the hash it was signed with",
        [SIGV4_BODY_SIGNATURE_MISMATCH] = "a chunk signature does not match",
        [SIGV4_BODY_MALFORMED] = "the aws-chunked body is malformed",
    };
    body->problem = problem;
    return err_set(err, ERR_PARSE, "sigv4: %s", WHY[problem]);
}

// read_more reads from the client into the framing buffer.
[[nodiscard]] static Error read_more(SigV4Body *body, Err *err)
{
    if (body->start > 0) {
        memmove(body->buffer, body->buffer + body->start, body->end - body->start);
        body->end -= body->start;
        body->start = 0;
    }
    if (body->end == sizeof body->buffer) {
        return fail(body, SIGV4_BODY_MALFORMED, err);
    }
    size_t got = 0;
    Error e = body->read(body->context, body->buffer + body->end, sizeof body->buffer - body->end, &got, err);
    if (e == ERR_OK && got == 0) {
        return fail(body, SIGV4_BODY_MALFORMED, err);
    }
    body->end += got;
    return e;
}

[[nodiscard]] static Error read_line(SigV4Body *body, String *line, Err *err)
{
    for (;;) {
        String pending = { body->buffer + body->start, body->end - body->start };
        size_t index = 0;
        if (str_find(pending, S("\r\n"), &index)) {
            *line = str_copy(body->arena, str_slice(pending, 0, index));
            body->start += index + 2;
            return ERR_OK;
        }
        Error e = read_more(body, err);
        if (e != ERR_OK) {
            return e;
        }
    }
}

[[nodiscard]] static Error check_chunk_signature(SigV4Body *body, Err *err)
{
    if (!signs_chunks(body->verified)) {
        return ERR_OK;
    }
    uint8_t digest[SHA256_LEN];
    sha256_final(&body->chunk_hash, digest);
    String expected = sigv4_chunk_signature(body->arena, body->verified, body->previous_signature,
                                            hex_encode(body->arena, digest, SHA256_LEN));
    if (!equal_constant_time(expected, body->chunk_signature)) {
        return fail(body, SIGV4_BODY_SIGNATURE_MISMATCH, err);
    }
    body->previous_signature = expected;
    return ERR_OK;
}

// check_trailer_signature checks x-amz-trailer-signature, which signs the
// other trailers as name:value lines following the last chunk's signature.
[[nodiscard]] static Error check_trailer_signature(SigV4Body *body, String signature, Err *err)
{
    StringBuilder lines = str_builder_create(body->arena, 128);
    for (size_t i = 0; i < body->trailer_count; i++) {
        str_builder_append_format(&lines, "%.*s:%.*s\n", (int)body->trailers[i].name.len, body->trailers[i].name.data,
                                  (int)body->trailers[i].value.len, body->trailers[i].value.data);
    }
    String trailers_hash = hex_sha256(body->arena, str_builder_finish(&lines));
    const SigV4Verified *verified = body->verified;
    String to_sign = str_format(body->arena, "AWS4-HMAC-SHA256-TRAILER\n%.*s\n%.*s\n%.*s\n%.*s",
                                (int)verified->amz_date.len, verified->amz_date.data, (int)verified->scope.len,
                                verified->scope.data, (int)body->previous_signature.len,
                                body->previous_signature.data, (int)trailers_hash.len, trailers_hash.data);
    if (!equal_constant_time(hex_hmac(body->arena, verified->signing_key, to_sign), signature)) {
        return fail(body, SIGV4_BODY_SIGNATURE_MISMATCH, err);
    }
    return ERR_OK;
}

[[nodiscard]] static Error read_trailers(SigV4Body *body, Err *err)
{
    String signature = S("");
    bool signed_trailer = false;
    String line;
    Error e;
    while ((e = read_line(body, &line, err)) == ERR_OK && line.len > 0) {
        String name;
        String value;
        if (!str_cut(line, ':', &name, &value)) {
            return fail(body, SIGV4_BODY_MALFORMED, err);
        }
        name = str_trim(name);
        value = str_trim(value);
        if (str_equal_ignore_case(name, S("x-amz-trailer-signature"))) {
            signature = value;
            signed_trailer = true;
        } else if (body->trailer_count < SIGV4_BODY_MAX_TRAILERS) {
            body->trailers[body->trailer_count++] = (HttpHeader){ name, value };
        } else {
            return fail(body, SIGV4_BODY_MALFORMED, err);
        }
    }
    if (e != ERR_OK || body->verified->payload != SIGV4_PAYLOAD_STREAMING_SIGNED_TRAILER) {
        return e;
    }
    return signed_trailer ? check_trailer_signature(body, signature, err) : fail(body, SIGV4_BODY_SIGNATURE_MISMATCH, err);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] static Error start_chunk(SigV4Body *body, Err *err)
{
    String line;
    Error e = read_line(body, &line, err);
    if (e != ERR_OK) {
        return e;
    }
    String size_text;
    String extension;
    str_cut(line, ';', &size_text, &extension);
    uint64_t size = 0;
    for (size_t i = 0; i < size_text.len; i++) {
        int digit = hex_digit(size_text.data[i]);
        if (digit < 0 || size > (UINT64_MAX >> 4)) {
            return fail(body, SIGV4_BODY_MALFORMED, err);
        }
        size = size << 4 | (uint64_t)digit;
    }
    if (size_text.len == 0) {
        return fail(body, SIGV4_BODY_MALFORMED, err);
    }
    body->chunk_signature = str_trim_prefix(extension, S("chunk-signature="));
    body->chunk_remaining = size;
    sha256_init(&body->chunk_hash);
    if (size > 0) {
        return ERR_OK;
    }
    e = check_chunk_signature(body, err);
    if (e == ERR_OK) {
        e = read_trailers(body, err);
    }
    body->done = e == ERR_OK;
    return e;
}

[[nodiscard]] static Error end_chunk(SigV4Body *body, Err *err)
{
    String crlf;
    Error e = read_line(body, &crlf, err);
    if (e == ERR_OK && crlf.len != 0) {
        return fail(body, SIGV4_BODY_MALFORMED, err);
    }
    return e != ERR_OK ? e : check_chunk_signature(body, err);
}

[[nodiscard]] static Error read_chunked(SigV4Body *body, char *buffer, size_t capacity, size_t *got, Err *err)
{
    if (body->chunk_remaining == 0) {
        Error e = start_chunk(body, err);
        if (e != ERR_OK || body->done) {
            return e;
        }
    }
    size_t want = body->chunk_remaining < capacity ? (size_t)body->chunk_remaining : capacity;
    if (body->start == body->end) {
        Error e = body->read(body->context, buffer, want, got, err);
        if (e == ERR_OK && *got == 0) {
            e = fail(body, SIGV4_BODY_MALFORMED, err);
        }
        if (e != ERR_OK) {
            return e;
        }
    } else {
        *got = min_size(want, body->end - body->start);
        memcpy(buffer, body->buffer + body->start, *got);
        body->start += *got;
    }
    body->chunk_remaining -= *got;
    sha256_update(&body->chunk_hash, buffer, *got);
    return body->chunk_remaining == 0 ? end_chunk(body, err) : ERR_OK;
}

Error sigv4_body_read(SigV4Body *body, char *buffer, size_t capacity, size_t *got, Err *err)
{
    *got = 0;
    if (body->done || capacity == 0) {
        return ERR_OK;
    }
    if (body->chunked) {
        return read_chunked(body, buffer, capacity, got, err);
    }
    Error e = body->read(body->context, buffer, capacity, got, err);
    if (e != ERR_OK) {
        return e;
    }
    sha256_update(&body->body_hash, buffer, *got);
    if (*got > 0) {
        return ERR_OK;
    }
    body->done = true;
    if (body->verified->payload != SIGV4_PAYLOAD_SHA256) {
        return ERR_OK;
    }
    uint8_t digest[SHA256_LEN];
    sha256_final(&body->body_hash, digest);
    if (!str_equal(hex_encode(body->arena, digest, SHA256_LEN), body->verified->payload_hash)) {
        return fail(body, SIGV4_BODY_HASH_MISMATCH, err);
    }
    return ERR_OK;
}

bool sigv4_body_trailer(const SigV4Body *body, String name, String *value)
{
    for (size_t i = 0; i < body->trailer_count; i++) {
        if (str_equal_ignore_case(body->trailers[i].name, name)) {
            *value = body->trailers[i].value;
            return true;
        }
    }
    return false;
}
