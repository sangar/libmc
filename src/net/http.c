#include "mc/net/http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include "mc/platform/platform.h"
#include "roots.h"

enum {
    DEFAULT_CONNECT_TIMEOUT_MS = 10 * 1000,
    DEFAULT_TIMEOUT_MS = 30 * 1000,
    DEFAULT_MAX_RESPONSE_BYTES = 8 * 1024 * 1024,
    DEFAULT_MAX_REDIRECTS = 5,
    INITIAL_BODY_CAPACITY = 4096,
    INITIAL_HEADER_CAPACITY = 16,
};

struct HttpClient {
    CURL *curl;
    // arena holds the copy of the configuration.
    Arena *arena;
    HttpClientConfig config;
    // ca_file is null where the system has no bundle; Mozilla's roots, built in, are used instead.
    const char *ca_file;
    const char *user_agent;
};

static HttpClientConfig with_defaults(HttpClientConfig config)
{
    config.connect_timeout_ms = config.connect_timeout_ms > 0 ? config.connect_timeout_ms : DEFAULT_CONNECT_TIMEOUT_MS;
    config.timeout_ms = config.timeout_ms > 0 ? config.timeout_ms : DEFAULT_TIMEOUT_MS;
    config.max_response_bytes = config.max_response_bytes > 0 ? config.max_response_bytes : DEFAULT_MAX_RESPONSE_BYTES;
    config.max_redirects = config.max_redirects > 0 ? config.max_redirects : DEFAULT_MAX_REDIRECTS;
    return config;
}

Error http_client_create(const HttpClientConfig *config, HttpClient **client, Err *err)
{
    String ca_file = roots_file(config->ca_file);
    if (ca_file.len > 0 && !file_exists(ca_file)) {
        return err_set(err, ERR_NOT_FOUND, "http: no bundle of trusted roots at %.*s",
                       (int)ca_file.len, ca_file.data);
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return err_set(err, ERR_PLATFORM, "http: curl_global_init failed");
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        curl_global_cleanup();
        return err_set(err, ERR_OUT_OF_MEMORY, "http: curl_easy_init failed");
    }
    Arena *arena = arena_create(0);
    HttpClient *created = arena_push(arena, sizeof *created);
    *created = (HttpClient){
        .curl = curl,
        .arena = arena,
        .config = with_defaults(*config),
        .ca_file = ca_file.len > 0 ? str_cstr(arena, ca_file) : nullptr,
        .user_agent = str_cstr(arena, config->user_agent),
    };
    String *hosts = arena_push(arena, sizeof(String) * (config->allowed_host_count + 1));
    for (size_t i = 0; i < config->allowed_host_count; i++) {
        hosts[i] = str_copy(arena, config->allowed_hosts[i]);
    }
    created->config.allowed_hosts = hosts;
    *client = created;
    return ERR_OK;
}

void http_client_destroy(HttpClient *client)
{
    curl_easy_cleanup(client->curl);
    curl_global_cleanup();
    arena_destroy(client->arena);
}

bool http_response_header(const HttpResponse *response, String name, String *value)
{
    for (size_t i = 0; i < response->header_count; i++) {
        if (str_equal_ignore_case(response->headers[i].name, name)) {
            *value = response->headers[i].value;
            return true;
        }
    }
    return false;
}

// Hop collects one response: the body and headers curl hands over, in the
// caller's arena, or a 2xx body in the download file.
typedef struct Hop {
    CURL *curl;
    Arena *arena;
    StringBuilder body;
    size_t max_body;
    bool too_large;
    const HttpDownload *download;
    int64_t downloaded;
    bool write_failed;
    HttpHeader *headers;
    size_t header_count;
    size_t header_capacity;
} Hop;

static size_t receive_body(char *data, size_t size, size_t count, void *argument)
{
    Hop *hop = argument;
    size_t length = size * count;
    long status = 0;
    curl_easy_getinfo(hop->curl, CURLINFO_RESPONSE_CODE, &status);
    if (hop->download != nullptr && status >= 200 && status < 300) {
        if (file_write(hop->download->fd, (String){ data, length }, nullptr) != ERR_OK) {
            hop->write_failed = true;
            return 0;
        }
        hop->downloaded += (int64_t)length;
        return length;
    }
    if (hop->body.len + length > hop->max_body) {
        hop->too_large = true;
        return 0;
    }
    str_builder_append(&hop->body, (String){ data, length });
    return length;
}

// receive_header keeps "Name: value" lines; a status line starts a new
// response, such as after 100 Continue, so it drops those before it.
static size_t receive_header(char *data, size_t size, size_t count, void *argument)
{
    Hop *hop = argument;
    size_t length = size * count;
    String line = str_trim((String){ data, length });
    String name = { 0 };
    String value = { 0 };
    if (str_starts_with(line, S("HTTP/"))) {
        hop->header_count = 0;
    } else if (str_cut(line, ':', &name, &value) && name.len > 0) {
        if (hop->headers == nullptr) {
            hop->header_capacity = INITIAL_HEADER_CAPACITY;
            hop->headers = arena_push(hop->arena, sizeof(HttpHeader) * hop->header_capacity);
        } else {
            hop->headers = arena_grow(hop->arena, hop->headers, &hop->header_capacity, hop->header_count,
                                      sizeof(HttpHeader));
        }
        hop->headers[hop->header_count++] = (HttpHeader){
            .name = str_copy(hop->arena, str_trim(name)),
            .value = str_copy(hop->arena, str_trim(value)),
        };
    }
    return length;
}

// BodySource feeds body_file to curl, from its offset again on each hop and on each rewind curl asks for.
typedef struct BodySource {
    const HttpFileBody *file;
    int64_t position;
    bool read_failed;
} BodySource;

static size_t send_body(char *buffer, size_t size, size_t count, void *argument)
{
    BodySource *source = argument;
    int64_t remaining = source->file->length - source->position;
    if (remaining <= 0) {
        return 0;
    }
    size_t got = 0;
    if (file_pread(source->file->fd, buffer, min_size(size * count, (size_t)remaining),
                   source->file->offset + source->position, &got, nullptr) != ERR_OK ||
        got == 0) {
        source->read_failed = true;
        return CURL_READFUNC_ABORT;
    }
    source->position += (int64_t)got;
    return got;
}

static int rewind_body(void *argument, curl_off_t offset, int origin)
{
    BodySource *source = argument;
    if (origin != SEEK_SET || offset < 0 || offset > source->file->length) {
        return CURL_SEEKFUNC_CANTSEEK;
    }
    source->position = offset;
    return CURL_SEEKFUNC_OK;
}

static int check_cancel(void *argument, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total,
                        curl_off_t uploaded)
{
    return cancel_requested(argument) ? 1 : 0;
}

static const char *const METHODS[] = { "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE" };

static bool is_method(String method)
{
    for (size_t i = 0; i < countof(METHODS); i++) {
        if (str_equal(method, S(METHODS[i]))) {
            return true;
        }
    }
    return false;
}

// check_url reports whether url may be requested: http or https as allowed, to an allowed host.
[[nodiscard]] static Error check_url(const HttpClient *client, const char *url, Err *err)
{
    CURLU *parsed = curl_url();
    char *scheme = nullptr;
    char *host = nullptr;
    Error e = ERR_OK;
    if (parsed == nullptr || curl_url_set(parsed, CURLUPART_URL, url, 0) != CURLUE_OK ||
        curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK ||
        curl_url_get(parsed, CURLUPART_HOST, &host, 0) != CURLUE_OK) {
        e = err_set(err, ERR_INVALID_ARGUMENT, "http: malformed URL %s", url);
    } else if (!str_equal(S(scheme), S("https")) && !(client->config.allow_plain_http && str_equal(S(scheme), S("http")))) {
        e = err_set(err, ERR_INVALID_ARGUMENT, "http: scheme %s is not allowed in %s", scheme, url);
    } else if (client->config.allowed_host_count > 0) {
        bool allowed = false;
        for (size_t i = 0; i < client->config.allowed_host_count && !allowed; i++) {
            allowed = str_equal_ignore_case(S(host), client->config.allowed_hosts[i]);
        }
        if (!allowed) {
            e = err_set(err, ERR_INVALID_ARGUMENT, "http: host %s is not allowed", host);
        }
    }
    curl_free(scheme);
    curl_free(host);
    curl_url_cleanup(parsed);
    return e;
}

[[nodiscard]] static Error map_curl_error(CURLcode code, const Hop *hop, const BodySource *source, Cancel *cancel,
                                         const char *detail, Err *err)
{
    if (hop->write_failed) {
        return err_set(err, ERR_IO, "http: cannot write the download");
    }
    if (source->read_failed) {
        return err_set(err, ERR_IO, "http: cannot read the body file");
    }
    if (cancel != nullptr && cancel_requested(cancel)) {
        return err_set(err, ERR_CANCELLED, "http: cancelled");
    }
    // curl refuses a declared Content-Length over the limit itself; the write callback catches the rest.
    if (hop->too_large || code == CURLE_FILESIZE_EXCEEDED) {
        return err_set(err, ERR_UNSUPPORTED, "http: response body larger than %zu bytes", hop->max_body);
    }
    const char *message = detail[0] != '\0' ? detail : curl_easy_strerror(code);
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
        return err_set(err, ERR_TIMEOUT, "http: %s", message);
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return err_set(err, ERR_INVALID_ARGUMENT, "http: %s", message);
    case CURLE_OUT_OF_MEMORY:
        return err_set(err, ERR_OUT_OF_MEMORY, "http: %s", message);
    default:
        return err_set(err, ERR_NETWORK, "http: %s", message);
    }
}

// HopRequest is what one hop sends; redirects change it.
typedef struct HopRequest {
    const char *method;
    const char *url;
    String body;
    const HttpFileBody *body_file;
    bool has_body;
    // decode is whether the client asked for gzip and deflate, and so decodes them.
    bool decode;
    Cancel *cancel;
    int64_t timeout_ms;
} HopRequest;

static void set_options(HttpClient *client, const HopRequest *hop_request, struct curl_slist *headers, Hop *hop,
                        BodySource *source, char *error_buffer)
{
    CURL *curl = client->curl;
    curl_easy_reset(curl);
    const char *protocols = client->config.allow_plain_http ? "http,https" : "https";
    curl_easy_setopt(curl, CURLOPT_URL, hop_request->url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
    roots_set(curl, client->ca_file);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)client->config.connect_timeout_ms);
    int64_t timeout_ms = hop_request->timeout_ms != 0 ? hop_request->timeout_ms : client->config.timeout_ms;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? (long)timeout_ms : 0L);
    if (client->config.stall_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)((client->config.stall_timeout_ms + 999) / 1000));
    }
    // A download's size is not the memory limit's business, nor is the Content-Length of a HEAD, which has
    // no body; receive_body applies the limit to bodies it keeps.
    bool head = strcmp(hop_request->method, "HEAD") == 0;
    if (hop->download == nullptr && !head) {
        curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)client->config.max_response_bytes);
    }
    // An empty list asks for every encoding curl decodes, gzip and deflate; receive_body counts decoded bytes.
    if (hop_request->decode) {
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    }
    if (hop_request->cancel != nullptr) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, hop_request->cancel);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, hop);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, hop);
    if (client->user_agent[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, client->user_agent);
    }
    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (head) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (strcmp(hop_request->method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, hop_request->method);
    }
    if (hop_request->has_body && hop_request->body_file != nullptr) {
        // An upload is a PUT to curl; CUSTOMREQUEST above names any other method.
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, send_body);
        curl_easy_setopt(curl, CURLOPT_READDATA, source);
        curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, rewind_body);
        curl_easy_setopt(curl, CURLOPT_SEEKDATA, source);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)hop_request->body_file->length);
    } else if (hop_request->has_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, hop_request->body.len > 0 ? hop_request->body.data : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)hop_request->body.len);
    }
}

[[nodiscard]] static Error header_list(const HttpRequest *request, Arena *scratch, struct curl_slist **list, Err *err)
{
    *list = nullptr;
    for (size_t i = 0; i < request->header_count; i++) {
        const HttpHeader *header = &request->headers[i];
        if (str_contains_any(header->name, S("\r\n:")) || str_contains_any(header->value, S("\r\n"))) {
            curl_slist_free_all(*list);
            *list = nullptr;
            return err_set(err, ERR_INVALID_ARGUMENT, "http: header %.*s holds a line break or colon",
                           (int)header->name.len, header->name.data);
        }
        const char *line = str_cstr(scratch, str_format(scratch, "%.*s: %.*s", (int)header->name.len,
                                                         header->name.data, (int)header->value.len,
                                                         header->value.data));
        struct curl_slist *appended = curl_slist_append(*list, line);
        if (appended == nullptr) {
            curl_slist_free_all(*list);
            *list = nullptr;
            return err_set(err, ERR_OUT_OF_MEMORY, "http: cannot list the headers");
        }
        *list = appended;
    }
    return ERR_OK;
}

static bool sets_header(const HttpRequest *request, String name)
{
    for (size_t i = 0; i < request->header_count; i++) {
        if (str_equal_ignore_case(request->headers[i].name, name)) {
            return true;
        }
    }
    return false;
}

// follows reports whether status redirects, and turns the request into the next hop's.
static bool follows(long status, HopRequest *hop_request)
{
    if (status == 301 || status == 302 || status == 303) {
        if (strcmp(hop_request->method, "HEAD") != 0) {
            hop_request->method = "GET";
        }
        hop_request->has_body = false;
        hop_request->body_file = nullptr;
        return true;
    }
    return status == 307 || status == 308;
}

Error http_send(HttpClient *client, Arena *arena, const HttpRequest *request, HttpResponse *response, Err *err)
{
    String method = request->method.len > 0 ? request->method : S("GET");
    if (!is_method(method)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "http: unsupported method %.*s", (int)min_size(method.len, 16),
                       method.data);
    }
    if (request->body_file != nullptr && (request->body_file->offset < 0 || request->body_file->length < 0)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "http: body_file has a negative offset or length");
    }
    Arena *scratch = arena_create(0);
    HopRequest hop_request = {
        .method = str_cstr(scratch, method),
        .url = str_cstr(scratch, request->url),
        .body = request->body,
        .body_file = request->body_file,
        .has_body = request->body.len > 0 || request->body_file != nullptr || str_equal(method, S("POST")) ||
                    str_equal(method, S("PUT")) || str_equal(method, S("PATCH")),
        .decode = !sets_header(request, S("Accept-Encoding")),
        .cancel = request->cancel,
        .timeout_ms = request->timeout_ms,
    };
    struct curl_slist *headers = nullptr;
    Error e = header_list(request, scratch, &headers, err);
    for (size_t hops = 0; e == ERR_OK; hops++) {
        e = check_url(client, hop_request.url, err);
        if (e) {
            break;
        }
        if (request->cancel != nullptr && cancel_requested(request->cancel)) {
            e = err_set(err, ERR_CANCELLED, "http: cancelled");
            break;
        }
        Hop hop = {
            .curl = client->curl,
            .arena = arena,
            .body = str_builder_create(arena, INITIAL_BODY_CAPACITY),
            .max_body = client->config.max_response_bytes,
            .download = request->download,
        };
        BodySource source = { .file = hop_request.body_file };
        char error_buffer[CURL_ERROR_SIZE] = "";
        set_options(client, &hop_request, headers, &hop, &source, error_buffer);
        CURLcode code = curl_easy_perform(client->curl);
        if (code != CURLE_OK) {
            e = map_curl_error(code, &hop, &source, request->cancel, error_buffer, err);
            break;
        }
        long status = 0;
        char *location = nullptr;
        curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(client->curl, CURLINFO_REDIRECT_URL, &location);
        if (location != nullptr && follows(status, &hop_request)) {
            if (hops == client->config.max_redirects) {
                e = err_set(err, ERR_NETWORK, "http: more than %zu redirects from %.*s",
                            client->config.max_redirects, (int)request->url.len, request->url.data);
                break;
            }
            // location belongs to the handle and changes on the next perform.
            hop_request.url = str_cstr(scratch, S(location));
            continue;
        }
        *response = (HttpResponse){
            .status = (int)status,
            .headers = hop.headers,
            .header_count = hop.header_count,
            .body = str_builder_finish(&hop.body),
            .url = str_copy(arena, S(hop_request.url)),
            .downloaded = hop.downloaded,
        };
        break;
    }
    curl_slist_free_all(headers);
    arena_destroy(scratch);
    return e;
}
