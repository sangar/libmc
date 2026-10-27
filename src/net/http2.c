#include "http2.h"

#include <stdlib.h>

#include <nghttp2/nghttp2.h>

#include "mc/platform/platform.h"
#include "mc/text/fmt.h"

enum { MAX_CONCURRENT_STREAMS = 100, OUTPUT_CAPACITY = 16384, MAX_RESPONSE_HEADERS = 64, SECONDS_PER_DAY = 86400 };

struct Http2Stream {
    Http2 *http;
    int32_t id;
    Arena *arena;
    HttpServerRequest request;
    HttpHeader *headers;
    size_t header_count;
    size_t header_capacity;
    size_t header_bytes;
    String authority;
    StringBuilder body;
    bool body_too_large;
    bool with_handler; // a handler holds the request, so the stream outlives a reset
    bool closed;       // nghttp2 is done with it
    String response_body;
    size_t response_sent;
    Http2Stream *next_ready;
    Http2Stream *previous; // in the connection's list of live streams
    Http2Stream *next;
};

struct Http2 {
    nghttp2_session *session;
    HttpServerLimits limits;
    Http2Stream *ready;      // complete requests not yet taken, oldest first
    Http2Stream *ready_last;
    Http2Stream *streams; // every stream not yet freed
    StringBuilder output;
    Arena *arena;
};

static void stream_free(Http2Stream *stream)
{
    Http2 *http = stream->http;
    if (stream->previous != nullptr) {
        stream->previous->next = stream->next;
    } else {
        http->streams = stream->next;
    }
    if (stream->next != nullptr) {
        stream->next->previous = stream->previous;
    }
    arena_destroy(stream->arena);
}

// release frees stream once neither nghttp2 nor a handler holds it.
static void release(Http2Stream *stream)
{
    if (stream->closed && !stream->with_handler) {
        stream_free(stream);
    }
}

static int begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    Arena *arena = arena_create(0);
    Http2Stream *stream = arena_push(arena, sizeof *stream);
    Http2 *http = user_data;
    *stream = (Http2Stream){
        .http = http,
        .id = frame->hd.stream_id,
        .arena = arena,
        .body = str_builder_create(arena, 256),
        .next = http->streams,
    };
    if (http->streams != nullptr) {
        http->streams->previous = stream;
    }
    http->streams = stream;
    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, stream);
    return 0;
}

static int header(nghttp2_session *session, const nghttp2_frame *frame, const uint8_t *name, size_t name_len,
                  const uint8_t *value, size_t value_len, uint8_t flags, void *user_data)
{
    Http2 *http = user_data;
    Http2Stream *stream = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
    if (stream == nullptr || frame->hd.type != NGHTTP2_HEADERS) {
        return 0;
    }
    stream->header_bytes += name_len + value_len;
    if (stream->header_bytes > http->limits.max_header_bytes) {
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE; // resets the stream
    }
    String key = str_copy(stream->arena, (String){ (const char *)name, name_len });
    String text = str_copy(stream->arena, (String){ (const char *)value, value_len });
    if (str_equal(key, S(":method"))) {
        stream->request.method = text;
    } else if (str_equal(key, S(":path"))) {
        stream->request.target = text;
    } else if (str_equal(key, S(":authority"))) {
        stream->authority = text;
    } else if (key.len > 0 && key.data[0] != ':') {
        stream->headers = arena_grow(stream->arena, stream->headers, &stream->header_capacity, stream->header_count,
                                     sizeof *stream->headers);
        stream->headers[stream->header_count++] = (HttpHeader){ key, text };
    }
    return 0;
}

static int data_chunk(nghttp2_session *session, uint8_t flags, int32_t stream_id, const uint8_t *data, size_t len,
                      void *user_data)
{
    Http2 *http = user_data;
    Http2Stream *stream = nghttp2_session_get_stream_user_data(session, stream_id);
    if (stream == nullptr || stream->body_too_large) {
        return 0;
    }
    stream->body_too_large = stream->body.len + len > http->limits.max_body_bytes;
    if (!stream->body_too_large) {
        str_builder_append(&stream->body, (String){ (const char *)data, len });
    }
    return 0;
}

static bool has_host(const Http2Stream *stream)
{
    for (size_t i = 0; i < stream->header_count; i++) {
        if (str_equal(stream->headers[i].name, S("host"))) {
            return true;
        }
    }
    return false;
}

// complete finishes the request as an HTTP/1.1 handler sees one: :authority
// becomes Host, and the target splits into path and query.
static void complete(Http2 *http, Http2Stream *stream)
{
    if (stream->authority.len > 0 && !has_host(stream)) {
        stream->headers = arena_grow(stream->arena, stream->headers, &stream->header_capacity, stream->header_count,
                                     sizeof *stream->headers);
        stream->headers[stream->header_count++] = (HttpHeader){ S("host"), stream->authority };
    }
    HttpServerRequest *request = &stream->request;
    String path = request->target;
    String query = { 0 };
    str_cut(request->target, '?', &path, &query);
    request->path = path;
    request->query = query;
    request->headers = stream->headers;
    request->header_count = stream->header_count;
    request->body = str_builder_finish(&stream->body);
    request->version_major = 2;
    if (http->ready_last != nullptr) {
        http->ready_last->next_ready = stream;
    } else {
        http->ready = stream;
    }
    http->ready_last = stream;
}

static void respond_now(Http2 *http, Http2Stream *stream, unsigned status);

static int frame_received(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
    Http2 *http = user_data;
    bool ends_stream = (frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
                       (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
    Http2Stream *stream = ends_stream ? nghttp2_session_get_stream_user_data(session, frame->hd.stream_id) : nullptr;
    if (stream == nullptr) {
        return 0;
    }
    if (stream->body_too_large) {
        respond_now(http, stream, 413);
    } else {
        complete(http, stream);
    }
    return 0;
}

static int stream_closed(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *user_data)
{
    Http2Stream *stream = nghttp2_session_get_stream_user_data(session, stream_id);
    if (stream != nullptr) {
        stream->closed = true;
        release(stream);
    }
    return 0;
}

static nghttp2_ssize read_body(nghttp2_session *session, int32_t stream_id, uint8_t *buffer, size_t length,
                               uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
    Http2Stream *stream = source->ptr;
    size_t left = stream->response_body.len - stream->response_sent;
    size_t count = left < length ? left : length;
    for (size_t i = 0; i < count; i++) {
        buffer[i] = (uint8_t)stream->response_body.data[stream->response_sent + i];
    }
    stream->response_sent += count;
    if (stream->response_sent == stream->response_body.len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (nghttp2_ssize)count;
}

static nghttp2_nv field(String name, String value)
{
    return (nghttp2_nv){ (uint8_t *)name.data, (uint8_t *)value.data, name.len, value.len, NGHTTP2_NV_FLAG_NONE };
}

// date is now as an HTTP Date field writes it (RFC 9110 5.6.7).
static String date(Arena *arena)
{
    static const char *const DAYS[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
    static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int64_t now = clock_wall_ns() / 1000000000;
    int64_t days = now / SECONDS_PER_DAY;
    int64_t seconds = now % SECONDS_PER_DAY;
    CivilDate civil = civil_from_days(days);
    return str_format(arena, "%s, %02u %s %04lld %02lld:%02lld:%02lld GMT", DAYS[days % 7], civil.day,
                      MONTHS[civil.month - 1], (long long)civil.year, (long long)(seconds / 3600),
                      (long long)(seconds / 60 % 60), (long long)(seconds % 60));
}

// field_allowed says whether a handler's field may go out in HTTP/2, which
// has no connection-specific fields and lowercase names (RFC 9113 8.2).
static bool field_allowed(String name)
{
    static const char *const CONNECTION_SPECIFIC[] = { "connection", "keep-alive", "proxy-connection",
                                                       "transfer-encoding", "upgrade", "content-length", "date" };
    for (size_t i = 0; i < countof(CONNECTION_SPECIFIC); i++) {
        if (str_equal_ignore_case(name, S(CONNECTION_SPECIFIC[i]))) {
            return false;
        }
    }
    return true;
}

static bool field_valid(String name, String value)
{
    if (name.len == 0) {
        return false;
    }
    for (size_t i = 0; i < name.len; i++) {
        char c = name.data[i];
        bool token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                     c == '_' || c == '.' || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ||
                     c == '*' || c == '+' || c == '^' || c == '`' || c == '|' || c == '~';
        if (!token) {
            return false;
        }
    }
    for (size_t i = 0; i < value.len; i++) {
        if (value.data[i] == '\r' || value.data[i] == '\n' || value.data[i] == '\0') {
            return false;
        }
    }
    return true;
}

static String lowercase(Arena *arena, String text)
{
    char *lower = arena_push(arena, text.len + 1);
    for (size_t i = 0; i < text.len; i++) {
        char c = text.data[i];
        lower[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    return (String){ lower, text.len };
}

// submit sends response on stream: a body unless the request was HEAD or
// the status has none, and a 500 in its place when a field cannot go out.
static void submit(Http2 *http, Http2Stream *stream, const HttpServerResponse *response)
{
    Arena *arena = stream->arena;
    HttpServerResponse failure = { .status = 500 };
    for (size_t i = 0; response != nullptr && i < response->header_count; i++) {
        if (!field_valid(response->headers[i].name, response->headers[i].value)) {
            response = &failure;
        }
    }
    response = response != nullptr ? response : &failure;
    bool bodyless = str_equal(stream->request.method, S("HEAD")) || response->status == 204 || response->status == 304;
    nghttp2_nv fields[MAX_RESPONSE_HEADERS + 3];
    size_t count = 0;
    fields[count++] = field(S(":status"), str_format(arena, "%u", response->status));
    fields[count++] = field(S("date"), date(arena));
    if (response->status != 204 && response->status != 304) {
        fields[count++] = field(S("content-length"), str_format(arena, "%zu", response->body.len));
    }
    for (size_t i = 0; i < response->header_count && count < countof(fields); i++) {
        if (field_allowed(response->headers[i].name)) {
            fields[count++] = field(lowercase(arena, response->headers[i].name), response->headers[i].value);
        }
    }
    stream->response_body = bodyless ? S("") : str_copy(arena, response->body);
    nghttp2_data_provider2 body = { .source.ptr = stream, .read_callback = read_body };
    unused(nghttp2_submit_response2(http->session, stream->id, fields, count,
                                    stream->response_body.len > 0 ? &body : nullptr));
}

// respond_now answers stream with an empty status without a handler.
static void respond_now(Http2 *http, Http2Stream *stream, unsigned status)
{
    HttpServerResponse response = { .status = status };
    submit(http, stream, &response);
}

Http2 *http2_create(HttpServerLimits limits)
{
    Arena *arena = arena_create(0);
    Http2 *http = arena_push(arena, sizeof *http);
    *http = (Http2){ .arena = arena, .limits = limits, .output = str_builder_create(arena, OUTPUT_CAPACITY) };
    nghttp2_session_callbacks *callbacks = nullptr;
    // nghttp2 fails these only when out of memory, which aborts everywhere else in libmc.
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        abort();
    }
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, data_chunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frame_received);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, stream_closed);
    int status = nghttp2_session_server_new(&http->session, callbacks, http);
    nghttp2_session_callbacks_del(callbacks);
    nghttp2_settings_entry settings[] = { { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, MAX_CONCURRENT_STREAMS } };
    if (status != 0 || nghttp2_submit_settings(http->session, NGHTTP2_FLAG_NONE, settings, countof(settings)) != 0) {
        abort();
    }
    return http;
}

// http2_destroy frees every stream; the caller waits for the handlers first.
void http2_destroy(Http2 *http)
{
    nghttp2_session_del(http->session);
    while (http->streams != nullptr) {
        stream_free(http->streams);
    }
    arena_destroy(http->arena);
}

bool http2_feed(Http2 *http, String data)
{
    if (data.len == 0) {
        return false;
    }
    return nghttp2_session_mem_recv2(http->session, (const uint8_t *)data.data, data.len) == (nghttp2_ssize)data.len;
}

bool http2_next_request(Http2 *http, Http2Request *request)
{
    Http2Stream *stream = http->ready;
    if (stream == nullptr) {
        return false;
    }
    http->ready = stream->next_ready;
    if (http->ready == nullptr) {
        http->ready_last = nullptr;
    }
    stream->with_handler = true;
    *request = (Http2Request){ .stream = stream, .arena = stream->arena, .request = &stream->request };
    return true;
}

void http2_respond(Http2 *http, Http2Stream *stream, const HttpServerResponse *response)
{
    stream->with_handler = false;
    if (stream->closed) {
        stream_free(stream);
        return;
    }
    submit(http, stream, response);
}

String http2_take_output(Http2 *http)
{
    http->output.len = 0;
    const uint8_t *data = nullptr;
    nghttp2_ssize len = 0;
    while ((len = nghttp2_session_mem_send2(http->session, &data)) > 0) {
        str_builder_append(&http->output, (String){ (const char *)data, (size_t)len });
    }
    return (String){ http->output.data, http->output.len };
}

void http2_shutdown(Http2 *http)
{
    unused(nghttp2_submit_shutdown_notice(http->session));
}

bool http2_done(Http2 *http)
{
    return !nghttp2_session_want_read(http->session) && !nghttp2_session_want_write(http->session);
}
