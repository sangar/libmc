#include "mc/net/s3.h"

#include "mc/container/sort.h"
#include "mc/crypto/sigv4.h"
#include "mc/encoding/xml.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"

enum {
    DEFAULT_ATTEMPTS = 3,
    DEFAULT_STALL_TIMEOUT_MS = 60 * 1000,
    INITIAL_POOL_CAPACITY = 4,
    INITIAL_LIST_CAPACITY = 64,
    MAX_PUT_HEADERS = 3,
};

static const int64_t DEFAULT_PART_SIZE = 64LL << 20;
static const int64_t BACKOFF_MS[] = { 200, 1000, 3000 };
static const char META_PREFIX[] = "x-amz-meta-";
static const char UNSIGNED_PAYLOAD[] = "UNSIGNED-PAYLOAD";

struct S3Client {
    // arena holds the copy of the configuration and the pool.
    Arena *arena;
    S3Config config;
    String scheme;
    // host is the Host header: the bucket's host name when virtual-hosted, with the port when it is not the default.
    String host;
    // host_name is host without the port, the one host requests may go to.
    String host_name;
    HttpClientConfig http;
    // mutex guards the idle connections, which requests borrow one at a time.
    Mutex mutex;
    HttpClient **idle;
    size_t idle_count;
    size_t idle_capacity;
};

// ---- credentials ----

// read_profile reads the profile's keys from a shared credentials file and reports whether it had a pair.
static bool read_profile(Arena *arena, String path, String profile, S3Credentials *credentials)
{
    String raw = { 0 };
    if (file_read_all(arena, path, &raw, nullptr) != ERR_OK) {
        return false;
    }
    StringList lines = strlist_from_lines(arena, raw);
    bool in_profile = false;
    S3Credentials found = { 0 };
    for (size_t i = 0; i < lines.count; i++) {
        String line = str_trim(lines.items[i]);
        String key = { 0 };
        String value = { 0 };
        if (str_starts_with(line, S("["))) {
            in_profile = str_ends_with(line, S("]")) && str_equal(str_trim(str_slice(line, 1, line.len - 1)), profile);
        } else if (in_profile && !str_starts_with(line, S("#")) && !str_starts_with(line, S(";")) &&
                   str_cut(line, '=', &key, &value)) {
            key = str_trim(key);
            value = str_trim(value);
            if (str_equal(key, S("aws_access_key_id"))) {
                found.access_key = value;
            } else if (str_equal(key, S("aws_secret_access_key"))) {
                found.secret_key = value;
            } else if (str_equal(key, S("aws_session_token"))) {
                found.session_token = value;
            }
        }
    }
    if (found.access_key.len == 0 || found.secret_key.len == 0) {
        return false;
    }
    *credentials = found;
    return true;
}

Error s3_credentials_from_environment(Arena *arena, S3Credentials *credentials, Err *err)
{
    S3Credentials found = { 0 };
    if (env_get(arena, S("AWS_ACCESS_KEY_ID"), &found.access_key) && found.access_key.len > 0 &&
        env_get(arena, S("AWS_SECRET_ACCESS_KEY"), &found.secret_key) && found.secret_key.len > 0) {
        if (!env_get(arena, S("AWS_SESSION_TOKEN"), &found.session_token)) {
            found.session_token = S("");
        }
        *credentials = found;
        return ERR_OK;
    }
    String profile = { 0 };
    if (!env_get(arena, S("AWS_PROFILE"), &profile) || profile.len == 0) {
        profile = S("default");
    }
    String path = { 0 };
    if (!env_get(arena, S("AWS_SHARED_CREDENTIALS_FILE"), &path) || path.len == 0) {
        path = path_join(arena, env_home(arena), S(".aws/credentials"));
    }
    if (read_profile(arena, path, profile, credentials)) {
        return ERR_OK;
    }
    return err_set(err, ERR_NOT_FOUND,
                   "s3: no credentials in AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, nor for profile %.*s in %.*s",
                   (int)profile.len, profile.data, (int)path.len, path.data);
}

// ---- the client ----

// parse_endpoint splits scheme://host[:port][/...] into the scheme, the Host header and the host name.
[[nodiscard]] static Error parse_endpoint(Arena *arena, String endpoint, S3Client *client, Err *err)
{
    int n = (int)endpoint.len;
    size_t separator = 0;
    if (!str_find(endpoint, S("://"), &separator)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "s3: endpoint %.*s: want scheme://host[:port]", n, endpoint.data);
    }
    String scheme = str_slice(endpoint, 0, separator);
    bool https = str_equal(scheme, S("https"));
    String host = { 0 };
    String path = { 0 };
    str_cut(str_slice(endpoint, separator + 3, endpoint.len), '/', &host, &path);
    if (!https && !str_equal(scheme, S("http"))) {
        return err_set(err, ERR_INVALID_ARGUMENT, "s3: endpoint %.*s: want http:// or https://", n, endpoint.data);
    }
    if ((https && str_ends_with(host, S(":443"))) || (!https && str_ends_with(host, S(":80")))) {
        host = str_slice(host, 0, host.len - (https ? 4 : 3));
    }
    // The host name ends at the port's colon, or after the bracket of an IPv6 address.
    String name = host;
    size_t colon = 0;
    size_t bracket = 0;
    if (str_starts_with(host, S("["))) {
        name = str_find_char(host, ']', &bracket) ? str_slice(host, 0, bracket + 1) : host;
    } else if (str_find_last_char(host, ':', &colon)) {
        name = str_slice(host, 0, colon);
    }
    if (name.len == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "s3: endpoint %.*s has no host", n, endpoint.data);
    }
    client->scheme = str_copy(arena, scheme);
    client->host = str_copy(arena, host);
    client->host_name = str_copy(arena, name);
    return ERR_OK;
}

Error s3_client_create(const S3Config *config, S3Client **client, Err *err)
{
    if (config->bucket.len == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "s3: no bucket");
    }
    if (config->credentials.access_key.len == 0 || config->credentials.secret_key.len == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "s3: no access key and secret key");
    }
    if (config->part_size < 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "s3: negative part_size");
    }
    Arena *arena = arena_create(0);
    S3Client *created = arena_push(arena, sizeof *created);
    String region = config->region.len > 0 ? config->region : S("us-east-1");
    created->arena = arena;
    created->config = (S3Config){
        .region = str_copy(arena, region),
        .bucket = str_copy(arena, config->bucket),
        .path_style = config->path_style,
        .credentials = {
            .access_key = str_copy(arena, config->credentials.access_key),
            .secret_key = str_copy(arena, config->credentials.secret_key),
            .session_token = str_copy(arena, config->credentials.session_token),
        },
        .part_size = config->part_size > 0 ? config->part_size : DEFAULT_PART_SIZE,
        .max_attempts = config->max_attempts > 0 ? config->max_attempts : DEFAULT_ATTEMPTS,
        .stall_timeout_ms = config->stall_timeout_ms > 0 ? config->stall_timeout_ms : DEFAULT_STALL_TIMEOUT_MS,
    };
    String endpoint = config->endpoint.len > 0
                          ? config->endpoint
                          : str_format(arena, "https://s3.%.*s.amazonaws.com", (int)region.len, region.data);
    Error e = parse_endpoint(arena, endpoint, created, err);
    if (e != ERR_OK) {
        arena_destroy(arena);
        return e;
    }
    if (!config->path_style) {
        created->host = str_format(arena, "%.*s.%.*s", (int)config->bucket.len, config->bucket.data,
                                   (int)created->host.len, created->host.data);
        created->host_name = str_format(arena, "%.*s.%.*s", (int)config->bucket.len, config->bucket.data,
                                        (int)created->host_name.len, created->host_name.data);
    }
    created->http = (HttpClientConfig){
        .ca_file = str_copy(arena, config->ca_file),
        .user_agent = str_copy(arena, config->user_agent),
        .allowed_hosts = &created->host_name,
        .allowed_host_count = 1,
        .stall_timeout_ms = created->config.stall_timeout_ms,
        .allow_plain_http = str_equal(created->scheme, S("http")),
    };
    // Creating one connection up front reports a missing CA bundle here rather than on the first request.
    HttpClient *first = nullptr;
    e = http_client_create(&created->http, &first, err);
    if (e != ERR_OK) {
        arena_destroy(arena);
        return e;
    }
    mutex_init(&created->mutex);
    created->idle_capacity = INITIAL_POOL_CAPACITY;
    created->idle = arena_push(arena, sizeof(HttpClient *) * created->idle_capacity);
    created->idle[created->idle_count++] = first;
    *client = created;
    return ERR_OK;
}

void s3_client_destroy(S3Client *client)
{
    for (size_t i = 0; i < client->idle_count; i++) {
        http_client_destroy(client->idle[i]);
    }
    mutex_destroy(&client->mutex);
    arena_destroy(client->arena);
}

[[nodiscard]] static Error borrow(S3Client *client, HttpClient **http, Err *err)
{
    mutex_lock(&client->mutex);
    *http = client->idle_count > 0 ? client->idle[--client->idle_count] : nullptr;
    mutex_unlock(&client->mutex);
    return *http != nullptr ? ERR_OK : http_client_create(&client->http, http, err);
}

static void give_back(S3Client *client, HttpClient *http)
{
    mutex_lock(&client->mutex);
    client->idle = arena_grow(client->arena, client->idle, &client->idle_capacity, client->idle_count,
                              sizeof(HttpClient *));
    client->idle[client->idle_count++] = http;
    mutex_unlock(&client->mutex);
}

// ---- dates ----

static int64_t digits(String s, size_t start, size_t end)
{
    uint64_t value = 0;
    return str_parse_u64(str_slice(s, start, end), &value) ? (int64_t)value : 0;
}

static int64_t unix_ns(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second)
{
    int64_t days = civil_to_days((CivilDate){ .year = year, .month = (unsigned)month, .day = (unsigned)day });
    return (((days * 24 + hour) * 60 + minute) * 60 + second) * NS_PER_SECOND;
}

// parse_iso8601 reads 2024-03-13T10:00:00.123Z, as ListObjectsV2 writes LastModified; 0 when it is not that.
static int64_t parse_iso8601(String s)
{
    if (s.len < 19 || s.data[4] != '-' || s.data[7] != '-' || s.data[10] != 'T') {
        return 0;
    }
    int64_t ns = unix_ns(digits(s, 0, 4), digits(s, 5, 7), digits(s, 8, 10), digits(s, 11, 13), digits(s, 14, 16),
                         digits(s, 17, 19));
    if (s.len > 20 && s.data[19] == '.') {
        int64_t scale = NS_PER_SECOND;
        for (size_t i = 20; i < s.len && s.data[i] >= '0' && s.data[i] <= '9' && scale > 1; i++) {
            scale /= 10;
            ns += (s.data[i] - '0') * scale;
        }
    }
    return ns;
}

// parse_http_date reads Wed, 13 Mar 2024 10:00:00 GMT, as Last-Modified carries it; 0 when it is not that.
static int64_t parse_http_date(Arena *arena, String s)
{
    static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    StringList fields = str_fields(arena, s);
    if (fields.count < 5 || fields.items[4].len != 8) {
        return 0;
    }
    int64_t month = 0;
    for (size_t i = 0; i < countof(MONTHS); i++) {
        if (str_equal(fields.items[2], S(MONTHS[i]))) {
            month = (int64_t)i + 1;
        }
    }
    if (month == 0) {
        return 0;
    }
    String clock = fields.items[4];
    return unix_ns(digits(fields.items[3], 0, 4), month, digits(fields.items[1], 0, 2), digits(clock, 0, 2),
                   digits(clock, 3, 5), digits(clock, 6, 8));
}

// ---- XML ----

// xml_text returns the text of the first element called name in events[from, to), or "" when it is absent or empty.
static String xml_text(const XmlEvents *events, size_t from, size_t to, String name)
{
    for (size_t i = from; i < to; i++) {
        if (events->items[i].kind == XML_EVENT_START && str_equal(events->items[i].name, name)) {
            bool has_text = i + 1 < to && events->items[i + 1].kind == XML_EVENT_TEXT;
            return has_text ? events->items[i + 1].text : S("");
        }
    }
    return S("");
}

// xml_find returns the text of the first element called name in body, or "" when the body is not XML or lacks it.
static String xml_find(Arena *arena, String body, String name)
{
    XmlEvents events = { 0 };
    if (body.len == 0 || xml_parse(arena, body, &events, nullptr) != ERR_OK) {
        return S("");
    }
    return xml_text(&events, 0, events.count, name);
}

static String strip_quotes(String s)
{
    if (s.len >= 2 && s.data[0] == '"' && s.data[s.len - 1] == '"') {
        return str_slice(s, 1, s.len - 1);
    }
    return s;
}

// ---- requests ----

typedef struct S3Request {
    String method;
    // key is empty for the bucket itself.
    String key;
    const HttpHeader *query;
    size_t query_count;
    // headers are further headers to sign, with lowercase names.
    const HttpHeader *headers;
    size_t header_count;
    String body;
    const HttpFileBody *body_file;
    const HttpDownload *download;
} S3Request;

static String canonical_path(Arena *arena, const S3Client *client, String key)
{
    StringBuilder path = str_builder_create(arena, 128);
    str_builder_append_char(&path, '/');
    if (client->config.path_style) {
        str_builder_append(&path, client->config.bucket);
        if (key.len > 0) {
            str_builder_append_char(&path, '/');
        }
    }
    str_builder_append(&path, str_percent_encode(arena, key, true));
    return str_builder_finish(&path);
}

static int compare_parameters(const void *context, const void *a, const void *b)
{
    const HttpHeader *x = a;
    const HttpHeader *y = b;
    int order = str_compare(x->name, y->name);
    return order != 0 ? order : str_compare(x->value, y->value);
}

static String canonical_query(Arena *arena, const S3Request *request)
{
    HttpHeader *sorted = arena_push(arena, arena_size_mul(request->query_count + 1, sizeof *sorted));
    for (size_t i = 0; i < request->query_count; i++) {
        sorted[i] = (HttpHeader){ str_percent_encode(arena, request->query[i].name, false),
                                  str_percent_encode(arena, request->query[i].value, false) };
    }
    sort_stable(arena, sorted, request->query_count, sizeof *sorted, compare_parameters, nullptr);
    StringBuilder query = str_builder_create(arena, 128);
    for (size_t i = 0; i < request->query_count; i++) {
        str_builder_append_format(&query, "%s%.*s=%.*s", i > 0 ? "&" : "", (int)sorted[i].name.len,
                                  sorted[i].name.data, (int)sorted[i].value.len, sorted[i].value.data);
    }
    return str_builder_finish(&query);
}

static String header_line(Arena *arena, String name, String value)
{
    return str_format(arena, "%.*s: %.*s", (int)name.len, name.data, (int)value.len, value.data);
}

// build_http signs request and fills http with the URL and headers, in arena.
static void build_http(Arena *arena, const S3Client *client, const S3Request *request, HttpRequest *http)
{
    String path = canonical_path(arena, client, request->key);
    String query = canonical_query(arena, request);
    String payload_hash = request->body_file != nullptr ? S(UNSIGNED_PAYLOAD) : sigv4_payload_hash(arena, request->body);
    String amz_date = sigv4_date(arena, clock_wall_ns() / NS_PER_SECOND);
    String token = client->config.credentials.session_token;

    size_t capacity = request->header_count + 5;
    HttpHeader *headers = arena_push(arena, arena_size_mul(capacity, sizeof *headers));
    size_t count = 0;
    headers[count++] = (HttpHeader){ S("x-amz-content-sha256"), payload_hash };
    if (token.len > 0) {
        headers[count++] = (HttpHeader){ S("x-amz-security-token"), token };
    }
    for (size_t i = 0; i < request->header_count; i++) {
        headers[count++] = request->headers[i];
    }
    StringList signed_lines = { 0 };
    for (size_t i = 0; i < count; i++) {
        strlist_push(arena, &signed_lines, header_line(arena, headers[i].name, headers[i].value));
    }
    SigV4Request signing = {
        .method = request->method,
        .path = path,
        .query = query,
        .host = client->host,
        .amz_date = amz_date,
        .headers = signed_lines,
        .payload_hash = payload_hash,
    };
    SigV4Credentials credentials = {
        .access_key = client->config.credentials.access_key,
        .secret_key = client->config.credentials.secret_key,
        .region = client->config.region,
        .service = S("s3"),
    };
    headers[count++] = (HttpHeader){ S("x-amz-date"), amz_date };
    headers[count++] = (HttpHeader){ S("Authorization"), sigv4_authorization(arena, signing, credentials) };
    // Stored bytes come back as stored: the client must not decode an object saved with a Content-Encoding.
    headers[count++] = (HttpHeader){ S("Accept-Encoding"), S("identity") };
    *http = (HttpRequest){
        .method = request->method,
        .url = str_format(arena, "%.*s://%.*s%.*s%s%.*s", (int)client->scheme.len, client->scheme.data,
                          (int)client->host.len, client->host.data, (int)path.len, path.data, query.len > 0 ? "?" : "",
                          (int)query.len, query.data),
        .headers = headers,
        .header_count = count,
        .body = request->body,
        .body_file = request->body_file,
        .download = request->download,
        .timeout_ms = -1,
    };
}

static bool is_success(int status)
{
    return status >= 200 && status < 300;
}

// retryable says whether a failed attempt is worth another: a failure of the network or a throttling or server status.
static bool retryable(Error e, int status)
{
    if (e != ERR_OK) {
        return e == ERR_NETWORK || e == ERR_TIMEOUT;
    }
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

// send sends request, trying again as retryable allows, and leaves the last status in response.
[[nodiscard]] static Error send(S3Client *client, Cancel *cancel, Arena *arena, const S3Request *request,
                                HttpResponse *response, Err *err)
{
    HttpClient *http = nullptr;
    Error e = borrow(client, &http, err);
    if (e != ERR_OK) {
        return e;
    }
    for (int attempt = 0;; attempt++) {
        if (attempt > 0) {
            int64_t backoff = BACKOFF_MS[min_size((size_t)attempt - 1, countof(BACKOFF_MS) - 1)];
            if (cancel == nullptr) {
                clock_sleep_ms(backoff);
            } else if (!cancel_sleep(cancel, backoff)) {
                e = err_set(err, ERR_CANCELLED, "s3: cancelled");
                break;
            }
        }
        if (request->download != nullptr) {
            e = file_truncate(request->download->fd, 0, err);
            if (e == ERR_OK) {
                e = file_seek(request->download->fd, 0, err);
            }
            if (e != ERR_OK) {
                break;
            }
        }
        HttpRequest signed_request = { 0 };
        build_http(arena, client, request, &signed_request);
        signed_request.cancel = cancel;
        *response = (HttpResponse){ 0 };
        e = http_send(http, arena, &signed_request, response, err);
        if (!retryable(e, response->status) || attempt + 1 >= client->config.max_attempts) {
            break;
        }
    }
    give_back(client, http);
    return e;
}

// fail_status describes a response whose status is an error, with S3's code and message when the body has them.
[[nodiscard]] static Error fail_status(Arena *arena, const S3Request *request, const HttpResponse *response, Err *err)
{
    String key = request->key.len > 0 ? request->key : S("/");
    String code = xml_find(arena, response->body, S("Code"));
    String message = xml_find(arena, response->body, S("Message"));
    Error e = response->status == 404 && !str_equal(code, S("NoSuchBucket")) && !str_equal(code, S("NoSuchUpload"))
                  ? ERR_NOT_FOUND
                  : ERR_NETWORK;
    return err_set(err, e, "s3: %.*s %.*s: HTTP %d%s%.*s%s%.*s", (int)request->method.len, request->method.data,
                   (int)key.len, key.data, response->status, code.len > 0 ? " " : "", (int)code.len, code.data,
                   message.len > 0 ? ": " : "", (int)message.len, message.data);
}

// call sends request and turns a status that is not 2xx into the error.
[[nodiscard]] static Error call(S3Client *client, Cancel *cancel, Arena *arena, const S3Request *request,
                                HttpResponse *response, Err *err)
{
    Error e = send(client, cancel, arena, request, response, err);
    if (e == ERR_OK && !is_success(response->status)) {
        e = fail_status(arena, request, response, err);
    }
    return e;
}

// describe reads an object's description from the headers of a HEAD or GET.
static S3Object describe(Arena *arena, String key, const HttpResponse *response)
{
    S3Object object = { .key = str_copy(arena, key), .etag = S(""), .content_type = S("") };
    size_t prefix = sizeof META_PREFIX - 1;
    size_t metadata_count = 0;
    for (size_t i = 0; i < response->header_count; i++) {
        metadata_count += str_equal_ignore_case(str_slice(response->headers[i].name, 0, prefix), S(META_PREFIX));
    }
    HttpHeader *metadata = arena_push(arena, arena_size_mul(metadata_count + 1, sizeof *metadata));
    for (size_t i = 0; i < response->header_count; i++) {
        String name = str_lower_ascii(arena, response->headers[i].name); // modern-c: allow ascii-text
        String value = response->headers[i].value;
        int64_t number = 0;
        if (str_starts_with(name, S(META_PREFIX))) {
            metadata[object.metadata.count++] = (HttpHeader){ str_slice(name, prefix, name.len), value };
        } else if (str_equal(name, S("etag"))) {
            object.etag = strip_quotes(value);
        } else if (str_equal(name, S("content-length")) && str_parse_i64(value, &number)) {
            object.size = number;
        } else if (str_equal(name, S("last-modified"))) {
            object.last_modified_ns = parse_http_date(arena, value);
        } else if (str_equal(name, S("content-type"))) {
            object.content_type = value;
        }
    }
    object.metadata.items = metadata;
    return object;
}

bool s3_metadata_get(const S3Object *object, String name, String *value)
{
    for (size_t i = 0; i < object->metadata.count; i++) {
        if (str_equal_ignore_case(object->metadata.items[i].name, name)) {
            *value = object->metadata.items[i].value;
            return true;
        }
    }
    return false;
}

// ---- operations ----

// put_headers turns the options into headers: content type, storage class and metadata.
static size_t put_headers(Arena *arena, const S3PutOptions *options, HttpHeader **headers)
{
    S3PutOptions none = { 0 };
    options = options != nullptr ? options : &none;
    *headers = arena_push(arena, arena_size_mul(options->metadata.count + 2, sizeof **headers));
    size_t count = 0;
    if (options->content_type.len > 0) {
        (*headers)[count++] = (HttpHeader){ S("content-type"), options->content_type };
    }
    if (options->storage_class.len > 0) {
        (*headers)[count++] = (HttpHeader){ S("x-amz-storage-class"), options->storage_class };
    }
    for (size_t i = 0; i < options->metadata.count; i++) {
        const HttpHeader *item = &options->metadata.items[i];
        String name = str_lower_ascii(arena, item->name); // modern-c: allow ascii-text
        (*headers)[count++] = (HttpHeader){ str_concat(arena, S(META_PREFIX), name), item->value };
    }
    return count;
}

[[nodiscard]] static Error put_once(S3Client *client, Cancel *cancel, Arena *arena, String key, String body,
                                    const HttpFileBody *body_file, const S3PutOptions *options, String *etag, Err *err)
{
    HttpHeader *headers = nullptr;
    size_t header_count = put_headers(arena, options, &headers);
    S3Request request = {
        .method = S("PUT"),
        .key = key,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_file = body_file,
    };
    HttpResponse response = { 0 };
    Error e = call(client, cancel, arena, &request, &response, err);
    if (e == ERR_OK) {
        String value = { 0 };
        *etag = http_response_header(&response, S("ETag"), &value) ? strip_quotes(value) : S("");
    }
    return e;
}

Error s3_put(S3Client *client, Cancel *cancel, Arena *arena, String key, String body, const S3PutOptions *options,
             String *etag, Err *err)
{
    return put_once(client, cancel, arena, key, body, nullptr, options, etag, err);
}

static void abort_upload(S3Client *client, Arena *arena, String key, String upload_id)
{
    const HttpHeader query[] = { { S("uploadId"), upload_id } };
    S3Request request = { .method = S("DELETE"), .key = key, .query = query, .query_count = countof(query) };
    HttpResponse response = { 0 };
    // It goes out even when the upload was cancelled, so the store does not keep the parts.
    (void)send(client, nullptr, arena, &request, &response, nullptr);
}

[[nodiscard]] static Error put_parts(S3Client *client, Cancel *cancel, Arena *arena, String key, String upload_id,
                                     int fd, int64_t size, StringBuilder *complete, Err *err)
{
    int64_t part_size = client->config.part_size;
    for (int64_t offset = 0, number = 1; offset < size; offset += part_size, number++) {
        String part_number = str_format(arena, "%lld", (long long)number);
        const HttpHeader query[] = { { S("partNumber"), part_number }, { S("uploadId"), upload_id } };
        HttpFileBody part = { .fd = fd, .offset = offset, .length = part_size < size - offset ? part_size : size - offset };
        S3Request request = {
            .method = S("PUT"),
            .key = key,
            .query = query,
            .query_count = countof(query),
            .body_file = &part,
        };
        HttpResponse response = { 0 };
        Error e = call(client, cancel, arena, &request, &response, err);
        if (e != ERR_OK) {
            return e;
        }
        String etag = { 0 };
        if (!http_response_header(&response, S("ETag"), &etag)) {
            return err_set(err, ERR_NETWORK, "s3: PUT %.*s part %.*s: no ETag in the response", (int)key.len,
                           key.data, (int)part_number.len, part_number.data);
        }
        etag = strip_quotes(etag);
        str_builder_append_format(complete, "<Part><PartNumber>%.*s</PartNumber><ETag>\"%.*s\"</ETag></Part>",
                                  (int)part_number.len, part_number.data, (int)etag.len, etag.data);
    }
    return ERR_OK;
}

[[nodiscard]] static Error put_multipart(S3Client *client, Cancel *cancel, Arena *arena, String key, int fd,
                                         int64_t size, const S3PutOptions *options, String *etag, Err *err)
{
    HttpHeader *headers = nullptr;
    size_t header_count = put_headers(arena, options, &headers);
    const HttpHeader start_query[] = { { S("uploads"), S("") } };
    S3Request start = {
        .method = S("POST"),
        .key = key,
        .query = start_query,
        .query_count = countof(start_query),
        .headers = headers,
        .header_count = header_count,
    };
    HttpResponse response = { 0 };
    Error e = call(client, cancel, arena, &start, &response, err);
    if (e != ERR_OK) {
        return e;
    }
    String upload_id = xml_find(arena, response.body, S("UploadId"));
    if (upload_id.len == 0) {
        return err_set(err, ERR_NETWORK, "s3: POST %.*s: no UploadId in the response", (int)key.len, key.data);
    }
    StringBuilder complete = str_builder_create(arena, 1024);
    str_builder_append(&complete, S("<CompleteMultipartUpload>"));
    e = put_parts(client, cancel, arena, key, upload_id, fd, size, &complete, err);
    str_builder_append(&complete, S("</CompleteMultipartUpload>"));
    if (e == ERR_OK) {
        const HttpHeader query[] = { { S("uploadId"), upload_id } };
        S3Request finish = {
            .method = S("POST"),
            .key = key,
            .query = query,
            .query_count = countof(query),
            .body = str_builder_finish(&complete),
        };
        e = call(client, cancel, arena, &finish, &response, err);
        // S3 can answer 200 and still fail the upload, with an Error document for the body.
        String code = e == ERR_OK ? xml_find(arena, response.body, S("Code")) : S("");
        if (code.len > 0) {
            String message = xml_find(arena, response.body, S("Message"));
            e = err_set(err, ERR_NETWORK, "s3: POST %.*s: %.*s: %.*s", (int)key.len, key.data, (int)code.len,
                        code.data, (int)message.len, message.data);
        } else if (e == ERR_OK) {
            *etag = strip_quotes(xml_find(arena, response.body, S("ETag")));
        }
    }
    if (e != ERR_OK) {
        abort_upload(client, arena, key, upload_id);
    }
    return e;
}

Error s3_put_file(S3Client *client, Cancel *cancel, Arena *arena, String key, int fd, int64_t size,
                  const S3PutOptions *options, String *etag, Err *err)
{
    if (size < 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "s3: PUT %.*s: negative size", (int)key.len, key.data);
    }
    if (size >= client->config.part_size) {
        return put_multipart(client, cancel, arena, key, fd, size, options, etag, err);
    }
    HttpFileBody body = { .fd = fd, .offset = 0, .length = size };
    return put_once(client, cancel, arena, key, S(""), &body, options, etag, err);
}

Error s3_get(S3Client *client, Cancel *cancel, Arena *arena, String key, String *body, S3Object *object, Err *err)
{
    S3Request request = { .method = S("GET"), .key = key };
    HttpResponse response = { 0 };
    Error e = call(client, cancel, arena, &request, &response, err);
    if (e == ERR_OK) {
        *body = response.body;
        *object = describe(arena, key, &response);
        object->size = (int64_t)response.body.len;
    }
    return e;
}

Error s3_get_file(S3Client *client, Cancel *cancel, Arena *arena, String key, int fd, S3Object *object, Err *err)
{
    HttpDownload download = { .fd = fd };
    S3Request request = { .method = S("GET"), .key = key, .download = &download };
    HttpResponse response = { 0 };
    Error e = call(client, cancel, arena, &request, &response, err);
    if (e == ERR_OK) {
        *object = describe(arena, key, &response);
        object->size = response.downloaded;
    }
    return e;
}

Error s3_head(S3Client *client, Cancel *cancel, Arena *arena, String key, S3Object *object, Err *err)
{
    S3Request request = { .method = S("HEAD"), .key = key };
    HttpResponse response = { 0 };
    Error e = call(client, cancel, arena, &request, &response, err);
    if (e == ERR_OK) {
        *object = describe(arena, key, &response);
    }
    return e;
}

Error s3_delete(S3Client *client, Cancel *cancel, Arena *arena, String key, Err *err)
{
    S3Request request = { .method = S("DELETE"), .key = key };
    HttpResponse response = { 0 };
    Error e = call(client, cancel, arena, &request, &response, err);
    return e == ERR_NOT_FOUND ? ERR_OK : e;
}

static void push_object(Arena *arena, S3ObjectList *objects, S3Object object)
{
    if (objects->items == nullptr) {
        objects->capacity = INITIAL_LIST_CAPACITY;
        objects->items = arena_push(arena, arena_size_mul(objects->capacity, sizeof *objects->items));
    } else {
        objects->items = arena_grow(arena, objects->items, &objects->capacity, objects->count, sizeof *objects->items);
    }
    objects->items[objects->count++] = object;
}

// list_page adds the objects of one ListObjectsV2 page to objects, in arena, and returns the token of the next
// page, or "" after the last; the token lives in page.
[[nodiscard]] static Error list_page(Arena *arena, Arena *page, String body, S3ObjectList *objects, String *token,
                                     Err *err)
{
    XmlEvents events = { 0 };
    Error e = xml_parse(page, body, &events, err);
    if (e != ERR_OK) {
        return err_wrap(err, e, "s3: list");
    }
    for (size_t i = 0; i < events.count; i++) {
        if (events.items[i].kind != XML_EVENT_START || !str_equal(events.items[i].name, S("Contents"))) {
            continue;
        }
        size_t end = i + 1;
        while (end < events.count &&
               !(events.items[end].kind == XML_EVENT_END && str_equal(events.items[end].name, S("Contents")))) {
            end++;
        }
        int64_t size = 0;
        (void)str_parse_i64(xml_text(&events, i, end, S("Size")), &size);
        push_object(arena, objects,
                    (S3Object){
                        .key = str_copy(arena, xml_text(&events, i, end, S("Key"))),
                        .size = size,
                        .etag = str_copy(arena, strip_quotes(xml_text(&events, i, end, S("ETag")))),
                        .last_modified_ns = parse_iso8601(xml_text(&events, i, end, S("LastModified"))),
                        .content_type = S(""),
                    });
        i = end;
    }
    bool truncated = str_equal(xml_text(&events, 0, events.count, S("IsTruncated")), S("true"));
    *token = truncated ? xml_text(&events, 0, events.count, S("NextContinuationToken")) : S("");
    if (truncated && token->len == 0) {
        return err_set(err, ERR_NETWORK, "s3: list: a truncated page without NextContinuationToken");
    }
    return ERR_OK;
}

Error s3_list(S3Client *client, Cancel *cancel, Arena *arena, String prefix, S3ObjectList *objects, Err *err)
{
    *objects = (S3ObjectList){ 0 };
    // Each page's response lives only until the next is fetched; the objects go to the caller's arena.
    Arena *pages[2] = { arena_create(0), arena_create(0) };
    String token = S("");
    Error e = ERR_OK;
    for (size_t n = 0; e == ERR_OK; n++) {
        // The token comes from the previous page, so that one's arena lives until this request is built.
        Arena *page = pages[n % 2];
        arena_reset(page);
        HttpHeader query[3] = { { S("list-type"), S("2") }, { S("prefix"), prefix } };
        size_t query_count = 2;
        if (token.len > 0) {
            query[query_count++] = (HttpHeader){ S("continuation-token"), token };
        }
        S3Request request = { .method = S("GET"), .query = query, .query_count = query_count };
        HttpResponse response = { 0 };
        e = call(client, cancel, page, &request, &response, err);
        if (e == ERR_OK) {
            e = list_page(arena, page, response.body, objects, &token, err);
        }
        if (token.len == 0) {
            break;
        }
    }
    arena_destroy(pages[0]);
    arena_destroy(pages[1]);
    return e;
}

Error s3_create_bucket(S3Client *client, Cancel *cancel, Arena *arena, Err *err)
{
    String region = client->config.region;
    // us-east-1 is the default location, and S3 refuses it named as a constraint.
    String body = str_equal(region, S("us-east-1"))
                      ? S("")
                      : str_format(arena,
                                   "<CreateBucketConfiguration><LocationConstraint>%.*s</LocationConstraint>"
                                   "</CreateBucketConfiguration>",
                                   (int)region.len, region.data);
    S3Request request = { .method = S("PUT"), .body = body };
    HttpResponse response = { 0 };
    Error e = send(client, cancel, arena, &request, &response, err);
    if (e != ERR_OK || is_success(response.status)) {
        return e;
    }
    if (str_equal(xml_find(arena, response.body, S("Code")), S("BucketAlreadyOwnedByYou"))) {
        return ERR_OK;
    }
    return fail_status(arena, &request, &response, err);
}
