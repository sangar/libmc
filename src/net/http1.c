#include "http1.h"

#include <assert.h>
#include <llhttp.h>

#include "mc/platform/platform.h"

// MAX_LINE_END_BYTES before a request are one empty line, all RFC 9112 2.2 asks
// a server to skip; llhttp skips any number.
enum { MAX_LINE_END_BYTES = 2 };
enum { URL_CAPACITY = 256, FIELD_CAPACITY = 64, OUTPUT_CAPACITY = 1024, BODY_CAPACITY = 256 };

struct Http1 {
    Arena *arena;
    Arena *request_arena; // reset at the start of each request
    HttpServerLimits limits;
    llhttp_t parser;
    StringBuilder output;
    size_t taken; // output bytes already handed out
    StringBuilder url;
    StringBuilder field;
    StringBuilder value;
    StringBuilder body;
    String field_name; // the name of the field whose value is being parsed
    HttpHeader *headers;
    size_t header_count;
    size_t header_capacity;
    HttpServerRequest request;
    size_t host_count;
    size_t transfer_encoding_count;
    bool chunked; // the last Transfer-Encoding is exactly "chunked"
    bool expects_continue;
    size_t header_bytes;
    size_t body_bytes;
    unsigned reject_status; // set by a callback that refuses the request, 0 otherwise
    bool between_messages;
    size_t line_end_bytes; // CR and LF bytes since the last message
    bool closing;          // the last request closes the connection
    bool in_trailer;
    size_t extension_name_bytes;
    bool awaiting_response;
    Http1BodySink sink; // streams bodies when set
    void *sink_context;
    bool has_body;
    bool body_ended;
    bool continue_sent;
    bool responding;       // a response has started
    bool response_body;    // it has a body: not for HEAD, 204 or 304
    bool chunked_response; // its body goes in chunks
    bool keep_alive;
    Http1Event pause_event; // what the parser paused for
};

static bool method_served(String method)
{
    static const char *const SERVED[] = { "GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "TRACE", "PATCH" };
    for (size_t i = 0; i < countof(SERVED); i++) {
        if (str_equal(method, S(SERVED[i]))) {
            return true;
        }
    }
    return false;
}

static bool visible_ascii(String text)
{
    for (size_t i = 0; i < text.len; i++) {
        unsigned char byte = (unsigned char)text.data[i];
        if (byte < 0x21 || byte >= 0x7f) {
            return false;
        }
    }
    return true;
}

static bool token_byte(unsigned char byte)
{
    static const char SEPARATORS[] = "\"(),/:;<=>?@[\\]{}";
    for (size_t i = 0; i < sizeof SEPARATORS - 1; i++) {
        if (byte == (unsigned char)SEPARATORS[i]) {
            return false;
        }
    }
    return byte > 0x20 && byte < 0x7f;
}

static bool field_name_valid(String name)
{
    for (size_t i = 0; i < name.len; i++) {
        if (!token_byte((unsigned char)name.data[i])) {
            return false;
        }
    }
    return name.len > 0;
}

static bool field_value_valid(String value)
{
    for (size_t i = 0; i < value.len; i++) {
        char byte = value.data[i];
        if (byte == '\r' || byte == '\n' || byte == '\0') {
            return false;
        }
    }
    return true;
}

static String reason_phrase(unsigned status)
{
    static const struct {
        unsigned status;
        const char *reason;
    } REASONS[] = {
        { 100, "Continue" },
        { 200, "OK" },
        { 201, "Created" },
        { 202, "Accepted" },
        { 204, "No Content" },
        { 206, "Partial Content" },
        { 301, "Moved Permanently" },
        { 302, "Found" },
        { 303, "See Other" },
        { 304, "Not Modified" },
        { 307, "Temporary Redirect" },
        { 308, "Permanent Redirect" },
        { 400, "Bad Request" },
        { 401, "Unauthorized" },
        { 403, "Forbidden" },
        { 404, "Not Found" },
        { 405, "Method Not Allowed" },
        { 409, "Conflict" },
        { 410, "Gone" },
        { 412, "Precondition Failed" },
        { 413, "Content Too Large" },
        { 414, "URI Too Long" },
        { 415, "Unsupported Media Type" },
        { 422, "Unprocessable Content" },
        { 429, "Too Many Requests" },
        { 431, "Request Header Fields Too Large" },
        { 500, "Internal Server Error" },
        { 501, "Not Implemented" },
        { 502, "Bad Gateway" },
        { 503, "Service Unavailable" },
        { 505, "HTTP Version Not Supported" },
    };
    for (size_t i = 0; i < countof(REASONS); i++) {
        if (REASONS[i].status == status) {
            return S(REASONS[i].reason);
        }
    }
    return S("");
}

static bool status_has_body(unsigned status)
{
    return status != 204 && status != 304;
}

// output_ready starts the output afresh once all of it was taken, so a
// streamed response does not grow it without end.
static void output_ready(Http1 *http)
{
    if (http->taken == http->output.len) {
        http->output.len = 0;
        http->taken = 0;
    }
}

static void append_head(Http1 *http, unsigned status, const HttpHeader *headers, size_t header_count,
                        int64_t content_length, bool keep_alive)
{
    String reason = reason_phrase(status);
    str_builder_append_format(&http->output, "HTTP/1.1 %u %.*s\r\n", status, (int)reason.len, reason.data);
    String date = http_date(http->request_arena, clock_wall_ns());
    str_builder_append_format(&http->output, "Date: %.*s\r\n", (int)date.len, date.data);
    for (size_t i = 0; i < header_count; i++) {
        HttpHeader header = headers[i];
        str_builder_append_format(&http->output, "%.*s: %.*s\r\n", (int)header.name.len, header.name.data,
                                  (int)header.value.len, header.value.data);
    }
    if (status_has_body(status) && content_length >= 0) {
        str_builder_append_format(&http->output, "Content-Length: %lld\r\n", (long long)content_length);
    } else if (status_has_body(status) && http->chunked_response) {
        str_builder_append(&http->output, S("Transfer-Encoding: chunked\r\n"));
    }
    str_builder_append_format(&http->output, "Connection: %s\r\n\r\n", keep_alive ? "keep-alive" : "close");
}

static void append_response(Http1 *http, const HttpServerResponse *response, bool keep_alive)
{
    http->chunked_response = false;
    append_head(http, response->status, response->headers, response->header_count, (int64_t)response->body.len,
                keep_alive);
    if (llhttp_get_method(&http->parser) != HTTP_HEAD && status_has_body(response->status)) {
        str_builder_append(&http->output, response->body);
    }
}

static void append_rejection(Http1 *http, String reason)
{
    output_ready(http);
    unsigned status = http->reject_status != 0 ? http->reject_status : 400;
    HttpHeader content_type = { S("Content-Type"), S("text/plain") };
    HttpServerResponse response = {
        .status = status,
        .headers = &content_type,
        .header_count = 1,
        .body = str_format(http->request_arena, "%.*s\n", (int)reason.len, reason.data),
    };
    append_response(http, &response, false);
    http->closing = true;
}

static int reject(Http1 *http, unsigned status)
{
    http->reject_status = status;
    return -1;
}

static int count_header_bytes(Http1 *http, size_t length)
{
    http->header_bytes += length;
    return http->header_bytes > http->limits.max_header_bytes ? reject(http, 431) : 0;
}

static int on_message_begin(llhttp_t *parser)
{
    Http1 *http = parser->data;
    arena_reset(http->request_arena);
    Arena *arena = http->request_arena;
    http->between_messages = false;
    http->in_trailer = false;
    http->output = str_builder_create(arena, OUTPUT_CAPACITY);
    http->taken = 0;
    http->url = str_builder_create(arena, URL_CAPACITY);
    http->field = str_builder_create(arena, FIELD_CAPACITY);
    http->value = str_builder_create(arena, FIELD_CAPACITY);
    http->body = str_builder_create(arena, BODY_CAPACITY);
    http->headers = nullptr;
    http->header_count = 0;
    http->header_capacity = 0;
    http->host_count = 0;
    http->transfer_encoding_count = 0;
    http->chunked = false;
    http->expects_continue = false;
    http->header_bytes = 0;
    http->body_bytes = 0;
    http->has_body = false;
    http->body_ended = false;
    http->continue_sent = false;
    return 0;
}

static int on_url(llhttp_t *parser, const char *at, size_t length)
{
    Http1 *http = parser->data;
    str_builder_append(&http->url, (String){ at, length });
    return count_header_bytes(http, length);
}

// on_url_complete accepts the origin form and "*"; absolute and authority
// forms are for proxies.
static int on_url_complete(llhttp_t *parser)
{
    Http1 *http = parser->data;
    String target = str_builder_finish(&http->url);
    bool valid = (str_starts_with(target, S("/")) || str_equal(target, S("*"))) && visible_ascii(target);
    return valid ? 0 : reject(http, 400);
}

static int on_header_field(llhttp_t *parser, const char *at, size_t length)
{
    Http1 *http = parser->data;
    str_builder_append(&http->field, (String){ at, length });
    return count_header_bytes(http, length);
}

static int on_header_field_complete(llhttp_t *parser)
{
    Http1 *http = parser->data;
    String name = str_copy(http->request_arena, str_builder_finish(&http->field));
    http->field.len = 0;
    if (http->in_trailer && str_equal_ignore_case(name, S("Connection"))) {
        return reject(http, 400);
    }
    http->host_count += str_equal_ignore_case(name, S("Host")) ? 1 : 0;
    http->transfer_encoding_count += str_equal_ignore_case(name, S("Transfer-Encoding")) ? 1 : 0;
    http->field_name = name;
    return 0;
}

static int on_header_value(llhttp_t *parser, const char *at, size_t length)
{
    Http1 *http = parser->data;
    str_builder_append(&http->value, (String){ at, length });
    return count_header_bytes(http, length);
}

static int on_header_value_complete(llhttp_t *parser)
{
    Http1 *http = parser->data;
    String value = str_trim(str_copy(http->request_arena, str_builder_finish(&http->value)));
    http->value.len = 0;
    if (http->in_trailer) {
        return 0;
    }
    String name = http->field_name;
    if (str_equal_ignore_case(name, S("Transfer-Encoding"))) {
        http->chunked = str_equal_ignore_case(value, S("chunked"));
    }
    if (str_equal_ignore_case(name, S("Expect"))) {
        http->expects_continue = str_equal_ignore_case(value, S("100-continue"));
    }
    http->headers =
        arena_grow(http->request_arena, http->headers, &http->header_capacity, http->header_count, sizeof *http->headers);
    http->headers[http->header_count++] = (HttpHeader){ name, value };
    return 0;
}

static void fill_request(Http1 *http, String body)
{
    llhttp_t *parser = &http->parser;
    String target = str_builder_finish(&http->url);
    String path = target;
    String query = { 0 };
    str_cut(target, '?', &path, &query);
    http->request = (HttpServerRequest){
        .method = S(llhttp_method_name((llhttp_method_t)llhttp_get_method(parser))),
        .target = target,
        .path = path,
        .query = query,
        .headers = http->headers,
        .header_count = http->header_count,
        .body = body,
        .version_major = parser->http_major,
        .version_minor = parser->http_minor,
    };
}

// on_headers_complete requires a served method, HTTP/1, one Host, which HTTP/1.1
// makes mandatory (RFC 9112 3.2), and no transfer coding but a single chunked;
// llhttp joins repeated fields into one list. HTTP/1.0 has no transfer
// codings, so one there is faulty framing (RFC 9112 6.1).
static int on_headers_complete(llhttp_t *parser)
{
    Http1 *http = parser->data;
    http->in_trailer = true;
    if (!method_served(S(llhttp_method_name((llhttp_method_t)llhttp_get_method(parser))))) {
        return reject(http, 501);
    }
    if (parser->http_major != 1) {
        return reject(http, 505);
    }
    bool host_required = parser->http_minor >= 1;
    if (http->host_count > 1 || (host_required && http->host_count == 0)) {
        return reject(http, 400);
    }
    if (http->transfer_encoding_count > 0 && !host_required) {
        return reject(http, 400);
    }
    if (http->transfer_encoding_count > 0 && !(http->transfer_encoding_count == 1 && http->chunked)) {
        return reject(http, 501);
    }
    http->has_body = http->chunked || parser->content_length > 0;
    if (http->sink != nullptr && http->has_body) {
        // The request goes to the handler now, and its body as it comes.
        fill_request(http, (String){ 0 });
        http->awaiting_response = true;
        http->pause_event = HTTP1_REQUEST;
        return HPE_PAUSED;
    }
    if (parser->content_length > http->limits.max_body_bytes) {
        return reject(http, 413);
    }
    http1_continue(http);
    return 0;
}

static int on_chunk_extension_name(llhttp_t *parser, const char *at, size_t length)
{
    Http1 *http = parser->data;
    http->extension_name_bytes += length;
    return 0;
}

// on_chunk_extension_name_complete refuses an empty name, which llhttp lets through.
static int on_chunk_extension_name_complete(llhttp_t *parser)
{
    Http1 *http = parser->data;
    bool empty = http->extension_name_bytes == 0;
    http->extension_name_bytes = 0;
    return empty ? reject(http, 400) : 0;
}

static int on_body(llhttp_t *parser, const char *at, size_t length)
{
    Http1 *http = parser->data;
    if (http->sink != nullptr) {
        http->sink(http->sink_context, (String){ at, length });
        return 0;
    }
    http->body_bytes += length;
    if (http->body_bytes > http->limits.max_body_bytes) {
        return reject(http, 413);
    }
    str_builder_append(&http->body, (String){ at, length });
    return 0;
}

// on_message_complete holds the request, or ends a streamed body, and pauses
// the parser, so http1_feed sees where the next request starts.
static int on_message_complete(llhttp_t *parser)
{
    Http1 *http = parser->data;
    bool streamed = http->awaiting_response;
    if (!streamed) {
        fill_request(http, str_builder_finish(&http->body));
    }
    http->closing = http->closing || llhttp_should_keep_alive(parser) != 1;
    http->awaiting_response = true;
    http->body_ended = true;
    http->between_messages = true;
    http->line_end_bytes = 0;
    http->pause_event = streamed ? HTTP1_BODY_END : HTTP1_REQUEST;
    return HPE_PAUSED;
}

static const llhttp_settings_t SETTINGS = {
    .on_message_begin = on_message_begin,
    .on_url = on_url,
    .on_url_complete = on_url_complete,
    .on_header_field = on_header_field,
    .on_header_field_complete = on_header_field_complete,
    .on_header_value = on_header_value,
    .on_header_value_complete = on_header_value_complete,
    .on_headers_complete = on_headers_complete,
    .on_body = on_body,
    .on_chunk_extension_name = on_chunk_extension_name,
    .on_chunk_extension_name_complete = on_chunk_extension_name_complete,
    .on_message_complete = on_message_complete,
};

Http1 *http1_create(HttpServerLimits limits, Http1BodySink sink, void *sink_context)
{
    Arena *arena = arena_create(0);
    Http1 *http = arena_push(arena, sizeof *http);
    *http = (Http1){
        .arena = arena,
        .request_arena = arena_create(0),
        .limits = limits,
        .between_messages = true,
        .sink = sink,
        .sink_context = sink_context,
    };
    http->output = str_builder_create(http->request_arena, OUTPUT_CAPACITY);
    llhttp_init(&http->parser, HTTP_REQUEST, &SETTINGS);
    http->parser.data = http;
    return http;
}

void http1_destroy(Http1 *http)
{
    arena_destroy(http->request_arena);
    arena_destroy(http->arena);
}

// skip_empty_line follows the line ends that start data, which may only make up
// one CRLF, and says whether they do so far.
static bool skip_empty_line(Http1 *http, String data)
{
    static const char CRLF[] = "\r\n";
    size_t i = 0;
    for (; i < data.len && (data.data[i] == '\r' || data.data[i] == '\n'); i++) {
        if (http->line_end_bytes == MAX_LINE_END_BYTES || data.data[i] != CRLF[http->line_end_bytes]) {
            return false;
        }
        http->line_end_bytes++;
    }
    return i == data.len || http->line_end_bytes != 1;
}

// rejectable says whether a refusal can still be the answer: not once the
// request went to a handler, whose response it would cut into.
static bool rejectable(const Http1 *http)
{
    return !http->awaiting_response;
}

Http1Event http1_feed(Http1 *http, String data, size_t *used)
{
    assert((!http->awaiting_response || !http->body_ended) && !http->closing);
    *used = data.len;
    // Between requests llhttp is still paused at the last one, where a close is no error.
    if (data.len == 0) {
        llhttp_errno_t result = http->between_messages ? HPE_OK : llhttp_finish(&http->parser);
        if (result != HPE_OK && rejectable(http)) {
            append_rejection(http, S(llhttp_errno_name(result)));
        }
        http->closing = true;
        return HTTP1_CLOSE;
    }
    if (http->between_messages && !skip_empty_line(http, data)) {
        append_rejection(http, S("more than one empty line"));
        return HTTP1_CLOSE;
    }
    llhttp_errno_t result = llhttp_execute(&http->parser, data.data, data.len);
    if (result == HPE_OK) {
        return HTTP1_MORE;
    }
    if (result == HPE_PAUSED) {
        *used = (size_t)(llhttp_get_error_pos(&http->parser) - data.data);
        llhttp_resume(&http->parser);
        return http->pause_event;
    }
    if (result != HPE_PAUSED_UPGRADE && rejectable(http)) {
        append_rejection(http, S(llhttp_errno_name(result)));
    }
    http->closing = true;
    return HTTP1_CLOSE;
}

bool http1_body_complete(const Http1 *http)
{
    return http->body_ended;
}

void http1_continue(Http1 *http)
{
    bool asked = http->expects_continue && http->has_body && http->parser.http_minor >= 1;
    if (asked && !http->continue_sent && !http->responding) {
        output_ready(http);
        str_builder_append(&http->output, S("HTTP/1.1 100 Continue\r\n\r\n"));
        http->continue_sent = true;
    }
}

const HttpServerRequest *http1_request(const Http1 *http)
{
    assert(http->awaiting_response);
    return &http->request;
}

Arena *http1_arena(Http1 *http)
{
    return http->request_arena;
}

bool http1_fields_valid(unsigned status, const HttpHeader *headers, size_t header_count)
{
    assert(status >= 200 && status <= 599);
    for (size_t i = 0; i < header_count; i++) {
        String name = headers[i].name;
        assert(!str_equal_ignore_case(name, S("Content-Length")) && !str_equal_ignore_case(name, S("Connection")) &&
               !str_equal_ignore_case(name, S("Transfer-Encoding")) && !str_equal_ignore_case(name, S("Date")));
        if (!field_name_valid(name) || !field_value_valid(headers[i].value)) {
            return false;
        }
    }
    return true;
}

bool http1_respond(Http1 *http, const HttpServerResponse *response, bool close)
{
    assert(http->awaiting_response && !http->responding);
    http->awaiting_response = false;
    static const HttpServerResponse FAILED = { .status = 500 };
    if (response == nullptr || !http1_fields_valid(response->status, response->headers, response->header_count)) {
        response = &FAILED;
    }
    bool keep_alive = !close && !http->closing;
    output_ready(http);
    append_response(http, response, keep_alive);
    http->closing = !keep_alive;
    return keep_alive;
}

void http1_start(Http1 *http, unsigned status, const HttpHeader *headers, size_t header_count, int64_t content_length,
                 bool close)
{
    assert(http->awaiting_response && !http->responding && http1_fields_valid(status, headers, header_count));
    bool http_1_1 = http->parser.http_minor >= 1;
    http->responding = true;
    http->response_body = llhttp_get_method(&http->parser) != HTTP_HEAD && status_has_body(status);
    http->chunked_response = content_length < 0 && http_1_1;
    // HTTP/1.0 has no chunks, so a body of unknown length ends with the
    // connection, as does a response begun before the request's body was read.
    http->keep_alive = !close && !http->closing && http->body_ended &&
                       (content_length >= 0 || http_1_1 || !status_has_body(status));
    output_ready(http);
    append_head(http, status, headers, header_count, content_length, http->keep_alive);
}

void http1_write(Http1 *http, String data)
{
    assert(http->responding);
    if (!http->response_body || data.len == 0) {
        return;
    }
    output_ready(http);
    if (http->chunked_response) {
        str_builder_append_format(&http->output, "%zx\r\n", data.len);
    }
    str_builder_append(&http->output, data);
    if (http->chunked_response) {
        str_builder_append(&http->output, S("\r\n"));
    }
}

bool http1_end(Http1 *http, bool complete)
{
    assert(http->responding);
    if (http->response_body && http->chunked_response && complete) {
        output_ready(http);
        str_builder_append(&http->output, S("0\r\n\r\n"));
    }
    http->responding = false;
    http->awaiting_response = false;
    bool keep_alive = http->keep_alive && complete;
    http->closing = !keep_alive;
    return keep_alive;
}

String http1_take_output(Http1 *http)
{
    String output = { http->output.data + http->taken, http->output.len - http->taken };
    http->taken = http->output.len;
    return output;
}

bool http1_idle(const Http1 *http)
{
    return http->between_messages && http->line_end_bytes == 0 && !http->awaiting_response;
}
