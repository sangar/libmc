#include "mc/crypto/sigv4.h"

#include "mc/container/sort.h"
#include "mc/crypto/sha256.h"
#include "mc/text/fmt.h"

typedef struct {
    String name;
    String value;
} Header;

static int compare_names(const void *context, const void *a, const void *b)
{
    return str_compare(((const Header *)a)->name, ((const Header *)b)->name);
}

// canonical_value trims the value and collapses each run of spaces to one, as the signature wants.
static String canonical_value(Arena *arena, String value)
{
    return str_join(arena, str_fields(arena, value), S(" "));
}

// merge_repeated_names joins the values of a repeated name with commas, in the
// order given, since the signature covers each name once.
static Header *merge_repeated_names(Arena *arena, Header *sorted, size_t *count)
{
    size_t merged = 0;
    for (size_t i = 0; i < *count; i++) {
        Header *last = merged > 0 ? &sorted[merged - 1] : nullptr;
        if (last != nullptr && str_equal(last->name, sorted[i].name)) {
            last->value = str_format(arena, "%.*s,%.*s", (int)last->value.len, last->value.data,
                                     (int)sorted[i].value.len, sorted[i].value.data);
        } else {
            sorted[merged++] = sorted[i];
        }
    }
    *count = merged;
    return sorted;
}

static Header *signed_headers(Arena *arena, SigV4Request request, size_t *count)
{
    *count = request.headers.count + 2;
    Header *headers = arena_push(arena, arena_size_mul(*count, sizeof *headers));
    headers[0] = (Header){ S("host"), request.host };
    headers[1] = (Header){ S("x-amz-date"), request.amz_date };
    for (size_t i = 0; i < request.headers.count; i++) {
        String name;
        String value;
        str_cut(request.headers.items[i], ':', &name, &value);
        headers[i + 2] = (Header){ str_lower_ascii(arena, str_trim(name)), canonical_value(arena, value) };
    }
    sort_stable(arena, headers, *count, sizeof *headers, compare_names, nullptr);
    return merge_repeated_names(arena, headers, count);
}

static String hex_sha256(Arena *arena, String data)
{
    uint8_t digest[SHA256_LEN];
    sha256(data, digest);
    return hex_encode(arena, digest, sizeof digest);
}

static String hmac(Arena *arena, String key, String data)
{
    uint8_t *digest = arena_push(arena, SHA256_LEN);
    hmac_sha256(key, data, digest);
    return (String){ (const char *)digest, SHA256_LEN };
}

String sigv4_authorization(Arena *arena, SigV4Request request, SigV4Credentials credentials)
{
    size_t count;
    Header *headers = signed_headers(arena, request, &count);
    StringBuilder canonical = str_builder_create(arena, 512);
    StringBuilder names = str_builder_create(arena, 64);
    str_builder_append_format(&canonical, "%.*s\n%.*s\n%.*s\n", (int)request.method.len, request.method.data,
                              (int)request.path.len, request.path.data, (int)request.query.len, request.query.data);
    for (size_t i = 0; i < count; i++) {
        str_builder_append_format(&canonical, "%.*s:%.*s\n", (int)headers[i].name.len, headers[i].name.data,
                                  (int)headers[i].value.len, headers[i].value.data);
        if (i > 0) {
            str_builder_append_char(&names, ';');
        }
        str_builder_append(&names, headers[i].name);
    }
    String signed_names = str_builder_finish(&names);
    str_builder_append_format(&canonical, "\n%.*s\n%.*s", (int)signed_names.len, signed_names.data,
                              (int)request.payload_hash.len, request.payload_hash.data);

    String date = str_slice(request.amz_date, 0, 8);
    String scope = str_format(arena, "%.*s/%.*s/%.*s/aws4_request", (int)date.len, date.data,
                              (int)credentials.region.len, credentials.region.data, (int)credentials.service.len,
                              credentials.service.data);
    String request_hash = hex_sha256(arena, str_builder_finish(&canonical));
    String to_sign = str_format(arena, "AWS4-HMAC-SHA256\n%.*s\n%.*s\n%.*s", (int)request.amz_date.len,
                                request.amz_date.data, (int)scope.len, scope.data, (int)request_hash.len,
                                request_hash.data);

    String key = hmac(arena, str_concat(arena, S("AWS4"), credentials.secret_key), date);
    key = hmac(arena, key, credentials.region);
    key = hmac(arena, key, credentials.service);
    key = hmac(arena, key, S("aws4_request"));
    String signature = hmac(arena, key, to_sign);
    String signature_hex = hex_encode(arena, (const uint8_t *)signature.data, signature.len);
    return str_format(arena, "AWS4-HMAC-SHA256 Credential=%.*s/%.*s, SignedHeaders=%.*s, Signature=%.*s",
                      (int)credentials.access_key.len, credentials.access_key.data, (int)scope.len, scope.data,
                      (int)signed_names.len, signed_names.data, (int)signature_hex.len, signature_hex.data);
}

String sigv4_payload_hash(Arena *arena, String payload)
{
    return hex_sha256(arena, payload);
}

String sigv4_date(Arena *arena, int64_t unix_seconds)
{
    String rfc3339 = fmt_rfc3339(arena, unix_seconds * NS_PER_SECOND, 0, false);
    return str_replace_all(arena, str_replace_all(arena, rfc3339, S("-"), S("")), S(":"), S(""));
}
