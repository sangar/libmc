#include "http_compress.h"

#include "mc/encoding/gzip.h"
#include "mc/platform/platform.h"

// weight_zero says whether a coding's ;q= parameter refuses it.
static bool weight_zero(String parameters)
{
    String name = { 0 };
    String weight = { 0 };
    if (!str_cut(str_trim(parameters), '=', &name, &weight) || !str_equal_ignore_case(str_trim(name), S("q"))) {
        return false;
    }
    weight = str_trim(weight);
    for (size_t i = 0; i < weight.len; i++) {
        if (weight.data[i] != '0' && weight.data[i] != '.') {
            return false;
        }
    }
    return weight.len > 0;
}

bool http_accepts_gzip(Arena *arena, const HttpServerRequest *request)
{
    int gzip = -1;
    int any = -1;
    for (size_t i = 0; i < request->header_count; i++) {
        if (!str_equal_ignore_case(request->headers[i].name, S("Accept-Encoding"))) {
            continue;
        }
        StringList codings = str_split(arena, request->headers[i].value, ',');
        for (size_t j = 0; j < codings.count; j++) {
            String coding = { 0 };
            String parameters = { 0 };
            if (!str_cut(codings.items[j], ';', &coding, &parameters)) {
                coding = codings.items[j];
            }
            coding = str_trim(coding);
            int accepted = weight_zero(parameters) ? 0 : 1;
            if (str_equal_ignore_case(coding, S("gzip"))) {
                gzip = accepted;
            } else if (str_equal(coding, S("*"))) {
                any = accepted;
            }
        }
    }
    return gzip >= 0 ? gzip == 1 : any == 1;
}

bool http_compressible(String content_type)
{
    static const char *const PREFIXES[] = { "text/", "application/json", "application/javascript",
                                            "application/xml", "application/wasm", "image/svg+xml" };
    static const char *const SUFFIXES[] = { "+json", "+xml" };
    String media = { 0 };
    String parameters = { 0 };
    if (!str_cut(content_type, ';', &media, &parameters)) {
        media = content_type;
    }
    media = str_trim(media);
    for (size_t i = 0; i < countof(PREFIXES); i++) {
        if (media.len >= S(PREFIXES[i]).len &&
            str_equal_ignore_case(str_slice(media, 0, S(PREFIXES[i]).len), S(PREFIXES[i]))) {
            return true;
        }
    }
    for (size_t i = 0; i < countof(SUFFIXES); i++) {
        String suffix = S(SUFFIXES[i]);
        if (media.len >= suffix.len && str_equal_ignore_case(str_slice(media, media.len - suffix.len, media.len), suffix)) {
            return true;
        }
    }
    return false;
}

static bool carries_credentials(const HttpServerRequest *request)
{
    String value = { 0 };
    return http_header_find(request->headers, request->header_count, S("Cookie"), &value) ||
           http_header_find(request->headers, request->header_count, S("Authorization"), &value) ||
           http_header_find(request->headers, request->header_count, S("X-Csrf-Token"), &value);
}

// jitter is 1 to max_bytes random letters, or nothing when max_bytes is 0.
static String jitter(Arena *arena, size_t max_bytes)
{
    uint8_t random[256];
    if (max_bytes == 0 || random_bytes(random, sizeof random, nullptr) != ERR_OK) {
        return S("");
    }
    size_t len = 1 + ((size_t)random[0] << 8 | random[1]) % (max_bytes < sizeof random - 2 ? max_bytes : sizeof random - 2);
    char *letters = arena_push(arena, len);
    for (size_t i = 0; i < len; i++) {
        letters[i] = (char)('a' + random[2 + i] % 26);
    }
    return (String){ letters, len };
}

// add_headers says the body is gzip, and adds Accept-Encoding to Vary, as a
// cache must keep the encodings apart.
static void add_headers(Arena *arena, HttpServerResponse *response)
{
    String vary = { 0 };
    bool has_vary = http_header_find(response->headers, response->header_count, S("Vary"), &vary);
    HttpHeader *headers = arena_push(arena, (response->header_count + 2) * sizeof *headers);
    size_t count = 0;
    for (size_t i = 0; i < response->header_count; i++) {
        headers[count] = response->headers[i];
        if (str_equal_ignore_case(headers[count].name, S("Vary")) && !str_equal(str_trim(vary), S("*"))) {
            headers[count].value = str_format(arena, "%.*s, Accept-Encoding", (int)vary.len, vary.data);
        }
        count++;
    }
    headers[count++] = (HttpHeader){ S("Content-Encoding"), S("gzip") };
    if (!has_vary) {
        headers[count++] = (HttpHeader){ S("Vary"), S("Accept-Encoding") };
    }
    response->headers = headers;
    response->header_count = count;
}

void http_compress(Arena *arena, const HttpServerRequest *request, HttpServerResponse *response,
                   const HttpCompression *compression)
{
    String content_type = { 0 };
    String encoding = { 0 };
    bool eligible = response->status >= 200 && response->status != 204 && response->status != 304 &&
                    response->body.len >= compression->min_bytes &&
                    !http_header_find(response->headers, response->header_count, S("Content-Encoding"), &encoding) &&
                    http_header_find(response->headers, response->header_count, S("Content-Type"), &content_type) &&
                    http_compressible(content_type) && http_accepts_gzip(arena, request) &&
                    !(compression->skip_credentials && carries_credentials(request));
    if (!eligible) {
        return;
    }
    GzipOptions options = { .comment = jitter(arena, compression->jitter_bytes) };
    String compressed = { 0 };
    if (gzip_compress(arena, response->body, &options, &compressed, nullptr) != ERR_OK ||
        compressed.len >= response->body.len) {
        return;
    }
    response->body = compressed;
    add_headers(arena, response);
}
