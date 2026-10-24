#include "test.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mc/net/s3.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

// The tests run the client against a small S3 in a thread of this process,
// over plain HTTP: one bucket, path-style, objects and multipart uploads in
// memory, list pages of two keys. It checks that requests are signed, not the
// signatures; set LIBMC_S3_ENDPOINT and LIBMC_S3_BUCKET, with credentials as
// s3_credentials_from_environment finds them, to also run against a real store.

enum { ACCEPT_POLL_MS = 50, MAX_OBJECTS = 32, MAX_PARTS = 16, LIST_PAGE = 2 };

static const char BUCKET[] = "bucket";
static const char ACCESS_KEY[] = "AKIDTEST";

typedef struct FakeObject {
    String key;
    String body;
    String content_type;
    // headers are the object's x-amz-meta-* lines, "name: value\r\n" each, as they go back out.
    String headers;
} FakeObject;

typedef struct FakeUpload {
    String id;
    String key;
    String parts[MAX_PARTS];
    String headers;
} FakeUpload;

typedef struct FakeS3 {
    int listener;
    String address;
    atomic_bool stopping;
    Thread thread;
    // arena holds the objects; only the server thread touches it while it runs.
    Arena *arena;
    FakeObject objects[MAX_OBJECTS];
    size_t object_count;
    FakeUpload uploads[4];
    size_t upload_count;
    bool bucket_exists;
    // failures makes that many requests answer 503 before the next succeeds.
    atomic_int failures;
    // failing_parts makes every upload of a part answer 503.
    atomic_bool failing_parts;
    atomic_int requests;
    atomic_int unsigned_requests;
    atomic_int aborted_uploads;
} FakeS3;

typedef struct FakeRequest {
    String method;
    String path;
    String query;
    String head;
    String body;
} FakeRequest;

static bool read_exactly(int fd, char *buffer, size_t length)
{
    size_t done = 0;
    while (done < length) {
        size_t got = 0;
        if (net_receive(fd, buffer + done, length - done, &got, nullptr) != ERR_OK || got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

static bool header_value(String head, const char *name, String *value)
{
    String rest = head;
    while (rest.len > 0) {
        String line = str_trim(str_first_line(rest));
        rest = str_slice(rest, str_first_line(rest).len + 1, rest.len);
        String line_name = { 0 };
        String line_value = { 0 };
        if (str_cut(line, ':', &line_name, &line_value) && str_equal_ignore_case(str_trim(line_name), S(name))) {
            *value = str_trim(line_value);
            return true;
        }
    }
    return false;
}

// read_request reads the head, answers Expect: 100-continue, then reads a Content-Length body.
static bool read_request(Arena *arena, int fd, FakeRequest *request)
{
    StringBuilder head = str_builder_create(arena, 1024);
    while (!str_ends_with((String){ head.data, head.len }, S("\r\n\r\n"))) {
        char c = 0;
        if (!read_exactly(fd, &c, 1)) {
            return false;
        }
        str_builder_append_char(&head, c);
    }
    request->head = str_builder_finish(&head);
    String line = str_first_line(request->head);
    String target = { 0 };
    String rest = { 0 };
    str_cut(line, ' ', &request->method, &rest);
    str_cut(rest, ' ', &target, &rest);
    str_cut(target, '?', &request->path, &request->query);
    String value = { 0 };
    uint64_t length = 0;
    if (header_value(request->head, "Expect", &value) && str_equal_ignore_case(value, S("100-continue"))) {
        (void)net_send(fd, S("HTTP/1.1 100 Continue\r\n\r\n"), nullptr);
    }
    if (header_value(request->head, "Content-Length", &value) && str_parse_u64(value, &length) && length > 0) {
        char *body = arena_push(arena, (size_t)length + 1);
        if (!read_exactly(fd, body, (size_t)length)) {
            return false;
        }
        request->body = (String){ body, (size_t)length };
    } else {
        request->body = S("");
    }
    return true;
}

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static String percent_decode(Arena *arena, String s)
{
    StringBuilder out = str_builder_create(arena, s.len);
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] == '%' && i + 2 < s.len && hex_digit(s.data[i + 1]) >= 0 && hex_digit(s.data[i + 2]) >= 0) {
            str_builder_append_char(&out, (char)(hex_digit(s.data[i + 1]) * 16 + hex_digit(s.data[i + 2])));
            i += 2;
        } else {
            str_builder_append_char(&out, s.data[i]);
        }
    }
    return str_builder_finish(&out);
}

// parameter finds name in an encoded query and decodes its value.
static bool parameter(Arena *arena, String query, const char *name, String *value)
{
    StringList pairs = str_split(arena, query, '&');
    for (size_t i = 0; i < pairs.count; i++) {
        String key = { 0 };
        String encoded = { 0 };
        str_cut(pairs.items[i], '=', &key, &encoded);
        if (str_equal(key, S(name))) {
            *value = percent_decode(arena, encoded);
            return true;
        }
    }
    return false;
}

static String reply(Arena *arena, int status, String headers, String body)
{
    return str_format(arena, "HTTP/1.1 %d Fake\r\nContent-Length: %zu\r\nConnection: close\r\n%.*s\r\n%.*s", status,
                      body.len, (int)headers.len, headers.data, (int)body.len, body.data);
}

static String error_reply(Arena *arena, int status, const char *code)
{
    String body = str_format(arena, "<?xml version=\"1.0\"?><Error><Code>%s</Code><Message>%s happened</Message></Error>",
                             code, code);
    return reply(arena, status, S("Content-Type: application/xml\r\n"), body);
}

static FakeObject *find_object(FakeS3 *s3, String key)
{
    for (size_t i = 0; i < s3->object_count; i++) {
        if (str_equal(s3->objects[i].key, key)) {
            return &s3->objects[i];
        }
    }
    return nullptr;
}

static void store_object(FakeS3 *s3, String key, String body, String content_type, String headers)
{
    FakeObject *object = find_object(s3, key);
    if (object == nullptr && s3->object_count < MAX_OBJECTS) {
        object = &s3->objects[s3->object_count++];
    }
    if (object != nullptr) {
        *object = (FakeObject){
            .key = str_copy(s3->arena, key),
            .body = str_copy(s3->arena, body),
            .content_type = str_copy(s3->arena, content_type),
            .headers = str_copy(s3->arena, headers),
        };
    }
}

// metadata_lines picks the x-amz-meta-* headers of a request.
static String metadata_lines(Arena *arena, String head)
{
    StringBuilder out = str_builder_create(arena, 128);
    StringList lines = str_split(arena, head, '\n');
    for (size_t i = 0; i < lines.count; i++) {
        String line = str_trim(lines.items[i]);
        if (str_starts_with(line, S("x-amz-meta-"))) {
            str_builder_append_format(&out, "%.*s\r\n", (int)line.len, line.data);
        }
    }
    return str_builder_finish(&out);
}

static int compare_objects(const void *a, const void *b)
{
    return str_compare(((const FakeObject *)a)->key, ((const FakeObject *)b)->key);
}

static String list_objects(FakeS3 *s3, Arena *arena, String query)
{
    String prefix = S("");
    String token = S("0");
    (void)parameter(arena, query, "prefix", &prefix);
    (void)parameter(arena, query, "continuation-token", &token);
    qsort(s3->objects, s3->object_count, sizeof *s3->objects, compare_objects);
    uint64_t skip = 0;
    (void)str_parse_u64(token, &skip);
    StringBuilder body = str_builder_create(arena, 1024);
    str_builder_append(&body, S("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ListBucketResult>"));
    size_t matched = 0;
    size_t listed = 0;
    bool truncated = false;
    for (size_t i = 0; i < s3->object_count; i++) {
        FakeObject *object = &s3->objects[i];
        if (!str_starts_with(object->key, prefix) || matched++ < skip) {
            continue;
        }
        if (listed == LIST_PAGE) {
            truncated = true;
            break;
        }
        listed++;
        String key = str_replace_all(arena, object->key, S("&"), S("&amp;"));
        str_builder_append_format(&body,
                                  "<Contents><Key>%.*s</Key><LastModified>2024-03-13T10:00:00.250Z</LastModified>"
                                  "<ETag>&quot;etag-%.*s&quot;</ETag><Size>%zu</Size></Contents>",
                                  (int)key.len, key.data, (int)key.len, key.data, object->body.len);
    }
    str_builder_append_format(&body, "<IsTruncated>%s</IsTruncated>", truncated ? "true" : "false");
    if (truncated) {
        str_builder_append_format(&body, "<NextContinuationToken>%zu</NextContinuationToken>", (size_t)skip + LIST_PAGE);
    }
    str_builder_append(&body, S("</ListBucketResult>"));
    return reply(arena, 200, S("Content-Type: application/xml\r\n"), str_builder_finish(&body));
}

static FakeUpload *find_upload(FakeS3 *s3, Arena *arena, String query)
{
    String id = { 0 };
    if (!parameter(arena, query, "uploadId", &id)) {
        return nullptr;
    }
    for (size_t i = 0; i < s3->upload_count; i++) {
        if (str_equal(s3->uploads[i].id, id)) {
            return &s3->uploads[i];
        }
    }
    return nullptr;
}

static String handle_upload(FakeS3 *s3, Arena *arena, const FakeRequest *request, String key)
{
    String ignored = { 0 };
    if (str_equal(request->method, S("POST")) && parameter(arena, request->query, "uploads", &ignored)) {
        if (s3->upload_count == countof(s3->uploads)) {
            return error_reply(arena, 500, "TooManyUploads");
        }
        FakeUpload *upload = &s3->uploads[s3->upload_count];
        *upload = (FakeUpload){
            .id = str_format(s3->arena, "upload-%zu", s3->upload_count),
            .key = str_copy(s3->arena, key),
            .headers = str_copy(s3->arena, metadata_lines(arena, request->head)),
        };
        s3->upload_count++;
        return reply(arena, 200, S(""),
                     str_format(arena, "<InitiateMultipartUploadResult><UploadId>%.*s</UploadId>"
                                       "</InitiateMultipartUploadResult>",
                                (int)upload->id.len, upload->id.data));
    }
    FakeUpload *upload = find_upload(s3, arena, request->query);
    if (upload == nullptr) {
        return error_reply(arena, 404, "NoSuchUpload");
    }
    String number_text = { 0 };
    uint64_t number = 0;
    if (str_equal(request->method, S("PUT")) && parameter(arena, request->query, "partNumber", &number_text) &&
        str_parse_u64(number_text, &number) && number >= 1 && number <= MAX_PARTS) {
        if (atomic_load(&s3->failing_parts)) {
            return error_reply(arena, 503, "SlowDown");
        }
        upload->parts[number - 1] = str_copy(s3->arena, request->body);
        return reply(arena, 200, str_format(arena, "ETag: \"part-%llu\"\r\n", (unsigned long long)number), S(""));
    }
    if (str_equal(request->method, S("DELETE"))) {
        atomic_fetch_add(&s3->aborted_uploads, 1);
        return reply(arena, 204, S(""), S(""));
    }
    // Completing joins the parts the request names, in order.
    StringBuilder body = str_builder_create(arena, 1024);
    for (size_t i = 0; i < MAX_PARTS; i++) {
        String tag = str_format(arena, "<PartNumber>%zu</PartNumber><ETag>\"part-%zu\"</ETag>", i + 1, i + 1);
        if (str_contains(request->body, tag)) {
            str_builder_append(&body, upload->parts[i]);
        }
    }
    store_object(s3, upload->key, str_builder_finish(&body), S("binary/octet-stream"), upload->headers);
    return reply(arena, 200, S(""),
                 S("<CompleteMultipartUploadResult><ETag>&quot;whole-3&quot;</ETag></CompleteMultipartUploadResult>"));
}

static String handle(FakeS3 *s3, Arena *arena, const FakeRequest *request)
{
    atomic_fetch_add(&s3->requests, 1);
    String authorization = { 0 };
    String date = { 0 };
    if (!header_value(request->head, "Authorization", &authorization) ||
        !str_starts_with(authorization, str_format(arena, "AWS4-HMAC-SHA256 Credential=%s/", ACCESS_KEY)) ||
        !header_value(request->head, "x-amz-date", &date)) {
        atomic_fetch_add(&s3->unsigned_requests, 1);
        return error_reply(arena, 403, "AccessDenied");
    }
    if (atomic_load(&s3->failures) > 0) {
        atomic_fetch_sub(&s3->failures, 1);
        return error_reply(arena, 503, "SlowDown");
    }
    String bucket_path = str_format(arena, "/%s", BUCKET);
    if (str_equal(request->path, bucket_path)) {
        if (str_equal(request->method, S("PUT"))) {
            bool existed = s3->bucket_exists;
            s3->bucket_exists = true;
            return existed ? error_reply(arena, 409, "BucketAlreadyOwnedByYou") : reply(arena, 200, S(""), S(""));
        }
        return list_objects(s3, arena, request->query);
    }
    if (!str_starts_with(request->path, str_concat(arena, bucket_path, S("/")))) {
        return error_reply(arena, 404, "NoSuchBucket");
    }
    String key = percent_decode(arena, str_slice(request->path, bucket_path.len + 1, request->path.len));
    if (str_equal(key, S("forbidden"))) {
        return error_reply(arena, 403, "AccessDenied");
    }
    if (str_contains(request->query, S("upload"))) {
        return handle_upload(s3, arena, request, key);
    }
    FakeObject *object = find_object(s3, key);
    if (str_equal(request->method, S("PUT"))) {
        String content_type = S("binary/octet-stream");
        (void)header_value(request->head, "Content-Type", &content_type);
        store_object(s3, key, request->body, content_type, metadata_lines(arena, request->head));
        return reply(arena, 200, str_format(arena, "ETag: \"etag-%.*s\"\r\n", (int)key.len, key.data), S(""));
    }
    if (str_equal(request->method, S("DELETE"))) {
        if (object != nullptr) {
            *object = s3->objects[--s3->object_count];
        }
        return reply(arena, 204, S(""), S(""));
    }
    if (object == nullptr) {
        return str_equal(request->method, S("HEAD")) ? reply(arena, 404, S(""), S("")) : error_reply(arena, 404, "NoSuchKey");
    }
    String headers = str_format(arena, "ETag: \"etag-%.*s\"\r\nContent-Type: %.*s\r\nLast-Modified: Wed, 13 Mar 2024 "
                                       "10:00:00 GMT\r\n%.*s",
                                (int)key.len, key.data, (int)object->content_type.len, object->content_type.data,
                                (int)object->headers.len, object->headers.data);
    if (str_equal(request->method, S("HEAD"))) {
        return str_format(arena, "HTTP/1.1 200 Fake\r\nContent-Length: %zu\r\nConnection: close\r\n%.*s\r\n",
                          object->body.len, (int)headers.len, headers.data);
    }
    return reply(arena, 200, headers, object->body);
}

static void *serve(void *argument)
{
    FakeS3 *s3 = argument;
    while (!atomic_load(&s3->stopping)) {
        int client = -1;
        if (!net_accept(s3->listener, ACCEPT_POLL_MS, &client)) {
            continue;
        }
        (void)net_set_timeouts(client, 5000, nullptr);
        Arena *arena = arena_create(0);
        FakeRequest request = { 0 };
        if (read_request(arena, client, &request)) {
            (void)net_send(client, handle(s3, arena, &request), nullptr);
        }
        net_close(client);
        arena_destroy(arena);
    }
    return nullptr;
}

static bool fake_start(Test *test, FakeS3 *s3)
{
    *s3 = (FakeS3){ .arena = arena_create(0) };
    atomic_init(&s3->stopping, false);
    atomic_init(&s3->failures, 0);
    atomic_init(&s3->failing_parts, false);
    atomic_init(&s3->requests, 0);
    atomic_init(&s3->unsigned_requests, 0);
    atomic_init(&s3->aborted_uploads, 0);
    return net_listen(S("127.0.0.1:0"), &s3->listener, nullptr) == ERR_OK &&
           net_local_address(test->arena, s3->listener, &s3->address, nullptr) == ERR_OK &&
           thread_start(&s3->thread, serve, s3, nullptr) == ERR_OK;
}

static void fake_stop(FakeS3 *s3)
{
    atomic_store(&s3->stopping, true);
    thread_join(&s3->thread);
    net_close(s3->listener);
    arena_destroy(s3->arena);
}

static S3Client *fake_client(Test *test, const FakeS3 *s3, int64_t part_size)
{
    S3Config config = {
        .endpoint = str_format(test->arena, "http://%.*s", (int)s3->address.len, s3->address.data),
        .region = S("eu-north-1"),
        .bucket = S(BUCKET),
        .path_style = true,
        .credentials = { .access_key = S(ACCESS_KEY), .secret_key = S("secret") },
        .part_size = part_size,
    };
    S3Client *client = nullptr;
    Err err = { 0 };
    test_check(test, s3_client_create(&config, &client, &err) == ERR_OK, err.msg);
    return client;
}

static void s3_stores_reads_and_removes_objects(Test *test)
{
    FakeS3 s3;
    test_check(test, fake_start(test, &s3), "starts the fake S3");
    S3Client *client = fake_client(test, &s3, 0);
    Err err = { 0 };
    test_check(test, s3_create_bucket(client, nullptr, test->arena, &err) == ERR_OK, err.msg);
    test_check(test, s3_create_bucket(client, nullptr, test->arena, &err) == ERR_OK, "a bucket already owned is fine");

    const HttpHeader metadata[] = { { S("SHA256"), S("abc") }, { S("mtime"), S("42") } };
    S3PutOptions options = { .metadata = { metadata, countof(metadata) }, .content_type = S("text/plain") };
    // Keys are bytes to S3: æ, ø and å in both cases, and å decomposed as macOS file names give it, stay distinct.
    String key = S("dir/a file+ÆØÅ æøå.txt");
    String decomposed = S("dir/a\xcc\x8a.txt");
    String composed = S("dir/\xc3\xa5.txt");
    String etag = { 0 };
    test_check(test, s3_put(client, nullptr, test->arena, key, S("hello"), &options, &etag, &err) == ERR_OK, err.msg);
    test_check_str(test, etag, S("etag-dir/a file+ÆØÅ æøå.txt"), "the ETag, without quotes, of a key that needs encoding");
    test_check(test, s3_put(client, nullptr, test->arena, decomposed, S("decomposed"), nullptr, &etag, &err) == ERR_OK &&
                         s3_put(client, nullptr, test->arena, composed, S("composed"), nullptr, &etag, &err) == ERR_OK,
               err.msg);
    String stored = { 0 };
    S3Object described = { 0 };
    test_check(test, s3_get(client, nullptr, test->arena, decomposed, &stored, &described, &err) == ERR_OK &&
                         str_equal(stored, S("decomposed")),
               "a decomposed å is its own key");
    test_check(test, s3_get(client, nullptr, test->arena, composed, &stored, &described, &err) == ERR_OK &&
                         str_equal(stored, S("composed")),
               "and a composed å another");

    String body = { 0 };
    S3Object object = { 0 };
    test_check(test, s3_get(client, nullptr, test->arena, key, &body, &object, &err) == ERR_OK, err.msg);
    test_check_str(test, body, S("hello"), "reads the body");
    test_check(test, object.size == 5, "the size");
    test_check_str(test, object.content_type, S("text/plain"), "the content type");
    String value = { 0 };
    test_check(test, s3_metadata_get(&object, S("sha256"), &value) && str_equal(value, S("abc")),
               "metadata names come back lowercase");
    test_check(test, s3_metadata_get(&object, S("MTIME"), &value) && str_equal(value, S("42")),
               "metadata is found ignoring case");

    test_check(test, s3_head(client, nullptr, test->arena, key, &object, &err) == ERR_OK, err.msg);
    test_check(test, object.size == 5 && object.metadata.count == 2, "HEAD describes the object");
    test_check(test, object.last_modified_ns == 1710324000LL * NS_PER_SECOND, "Last-Modified as Unix nanoseconds");

    test_check(test, s3_delete(client, nullptr, test->arena, key, &err) == ERR_OK, err.msg);
    test_check(test, s3_head(client, nullptr, test->arena, key, &object, &err) == ERR_NOT_FOUND, "HEAD of a missing key");
    test_check(test, s3_get(client, nullptr, test->arena, key, &body, &object, &err) == ERR_NOT_FOUND,
               "GET of a missing key");
    test_check(test, s3_delete(client, nullptr, test->arena, key, &err) == ERR_OK, "deleting a missing key");

    test_check(test, s3_put(client, nullptr, test->arena, S("forbidden"), S("x"), nullptr, &etag, &err) == ERR_NETWORK,
               "a refused request");
    test_check(test, str_contains(S(err.msg), S("HTTP 403 AccessDenied: AccessDenied happened")),
               "names S3's code and message");
    test_check(test, atomic_load(&s3.unsigned_requests) == 0, "every request was signed");
    s3_client_destroy(client);
    fake_stop(&s3);
}

static void s3_moves_files_in_parts(Test *test)
{
    FakeS3 s3;
    test_check(test, fake_start(test, &s3), "starts the fake S3");
    S3Client *client = fake_client(test, &s3, 16);
    Err err = { 0 };
    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-s3-"), &dir, &err) == ERR_OK, err.msg);
    String source_path = path_join(test->arena, dir, S("source"));
    String content = S("0123456789abcdefghijklmnopqrstuvwxyzABCD");
    test_check(test, file_write_all(source_path, content, 0600, &err) == ERR_OK, err.msg);
    int source = -1;
    test_check(test, file_open_read(source_path, &source, &err) == ERR_OK, err.msg);
    const HttpHeader metadata[] = { { S("sha256"), S("big") } };
    S3PutOptions options = { .metadata = { metadata, countof(metadata) } };

    String etag = { 0 };
    test_check(test, s3_put_file(client, nullptr, test->arena, S("small"), source, 10, &options, &etag, &err) == ERR_OK,
               err.msg);
    test_check(test, s3_put_file(client, nullptr, test->arena, S("big"), source, (int64_t)content.len, &options, &etag,
                                 &err) == ERR_OK,
               err.msg);
    test_check_str(test, etag, S("whole-3"), "the multipart upload's ETag");

    String sink_path = path_join(test->arena, dir, S("sink"));
    int sink = -1;
    test_check(test, file_create(sink_path, 0600, &sink, &err) == ERR_OK, err.msg);
    test_check(test, file_write(sink, S("stale bytes, longer than the object"), &err) == ERR_OK, err.msg);
    S3Object object = { 0 };
    test_check(test, s3_get_file(client, nullptr, test->arena, S("big"), sink, &object, &err) == ERR_OK, err.msg);
    String downloaded = { 0 };
    test_check(test, file_read_all(test->arena, sink_path, &downloaded, &err) == ERR_OK, err.msg);
    test_check_str(test, downloaded, content, "three parts make the file again, replacing what the sink held");
    test_check(test, object.size == (int64_t)content.len, "the size of the download");
    String value = { 0 };
    test_check(test, s3_metadata_get(&object, S("sha256"), &value) && str_equal(value, S("big")),
               "the upload's metadata");
    test_check(test, s3_get_file(client, nullptr, test->arena, S("small"), sink, &object, &err) == ERR_OK, err.msg);
    test_check(test, file_read_all(test->arena, sink_path, &downloaded, &err) == ERR_OK, err.msg);
    test_check_str(test, downloaded, S("0123456789"), "a file below part_size goes in one request");

    // The first part fails every attempt, so the upload is aborted.
    atomic_store(&s3.failing_parts, true);
    test_check(test, s3_put_file(client, nullptr, test->arena, S("doomed"), source, (int64_t)content.len, &options,
                                 &etag, &err) == ERR_NETWORK,
               "a part that keeps failing fails the upload");
    test_check(test, atomic_load(&s3.aborted_uploads) == 1, "and aborts it");
    atomic_store(&s3.failing_parts, false);

    file_close(sink);
    file_close(source);
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, err.msg);
    s3_client_destroy(client);
    fake_stop(&s3);
}

static void s3_lists_every_page(Test *test)
{
    FakeS3 s3;
    test_check(test, fake_start(test, &s3), "starts the fake S3");
    S3Client *client = fake_client(test, &s3, 0);
    Err err = { 0 };
    const char *const keys[] = { "a/1", "a/2", "a/3 & æøå", "a/4 ÆØÅ", "a/5", "b/1" };
    for (size_t i = 0; i < countof(keys); i++) {
        String etag = { 0 };
        test_check(test, s3_put(client, nullptr, test->arena, S(keys[i]), S(keys[i]), nullptr, &etag, &err) == ERR_OK,
                   err.msg);
    }
    S3ObjectList objects = { 0 };
    test_check(test, s3_list(client, nullptr, test->arena, S("a/"), &objects, &err) == ERR_OK, err.msg);
    test_check(test, objects.count == 5, "five keys over three pages");
    test_check_str(test, objects.count > 2 ? objects.items[2].key : S(""), S("a/3 & æøå"), "an escaped key");
    test_check_str(test, objects.count > 3 ? objects.items[3].key : S(""), S("a/4 ÆØÅ"), "upper case æ, ø and å");
    test_check_str(test, objects.count > 2 ? objects.items[2].etag : S(""), S("etag-a/3 & æøå"), "the ETag without quotes");
    test_check(test, objects.count > 2 && objects.items[2].size == (int64_t)strlen("a/3 & æøå"), "the size in bytes");
    test_check(test, objects.count > 0 && objects.items[0].last_modified_ns == 1710324000LL * NS_PER_SECOND + 250000000,
               "LastModified with its milliseconds");
    test_check(test, s3_list(client, nullptr, test->arena, S("c/"), &objects, &err) == ERR_OK && objects.count == 0,
               "an empty listing");
    s3_client_destroy(client);
    fake_stop(&s3);
}

static void s3_tries_again_and_stops(Test *test)
{
    FakeS3 s3;
    test_check(test, fake_start(test, &s3), "starts the fake S3");
    S3Client *client = fake_client(test, &s3, 0);
    Err err = { 0 };
    String etag = { 0 };
    atomic_store(&s3.failures, 1);
    test_check(test, s3_put(client, nullptr, test->arena, S("k"), S("v"), nullptr, &etag, &err) == ERR_OK,
               "a 503 is tried again");
    test_check(test, atomic_load(&s3.requests) == 2, "once");

    Cancel cancel;
    cancel_init(&cancel);
    cancel_request(&cancel);
    test_check(test, s3_put(client, &cancel, test->arena, S("k"), S("v"), nullptr, &etag, &err) == ERR_CANCELLED,
               "a cancelled run sends nothing");
    test_check(test, atomic_load(&s3.requests) == 2, "not a request more");
    cancel_destroy(&cancel);

    S3Config incomplete = { .bucket = S("b") };
    S3Client *refused = nullptr;
    test_check(test, s3_client_create(&incomplete, &refused, &err) == ERR_INVALID_ARGUMENT, "no credentials");
    S3Config ftp = { .bucket = S("b"), .credentials = { S("a"), S("s"), S("") }, .endpoint = S("ftp://host") };
    test_check(test, s3_client_create(&ftp, &refused, &err) == ERR_INVALID_ARGUMENT, "an endpoint that is not HTTP");
    s3_client_destroy(client);
    fake_stop(&s3);
}

// swap_environment sets name to value, or unsets it when value is null, and returns what it was.
static String swap_environment(Test *test, const char *name, const char *value, bool *was_set)
{
    String previous = { 0 };
    *was_set = env_get(test->arena, S(name), &previous);
    if (value != nullptr) {
        (void)env_set(S(name), S(value), nullptr);
    } else {
        env_unset(S(name));
    }
    return previous;
}

static void restore_environment(const char *name, String previous, bool was_set)
{
    if (was_set) {
        (void)env_set(S(name), previous, nullptr);
    } else {
        env_unset(S(name));
    }
}

static void s3_finds_credentials_like_the_aws_tools(Test *test)
{
    static const char *const NAMES[] = { "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN", "AWS_PROFILE",
                                         "AWS_SHARED_CREDENTIALS_FILE" };
    String saved[countof(NAMES)];
    bool was_set[countof(NAMES)];
    for (size_t i = 0; i < countof(NAMES); i++) {
        saved[i] = swap_environment(test, NAMES[i], nullptr, &was_set[i]);
    }
    Err err = { 0 };
    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-aws-"), &dir, &err) == ERR_OK, err.msg);
    String file = path_join(test->arena, dir, S("credentials"));
    test_check(test, file_write_all(file,
                                    S("[default]\naws_access_key_id = DEFAULTKEY\naws_secret_access_key = defaultsecret\n"
                                      "\n# comment\n[ work ]\naws_access_key_id=WORKKEY\naws_secret_access_key=worksecret\n"
                                      "aws_session_token = worktoken\n"),
                                    0600, &err) == ERR_OK,
               err.msg);
    (void)env_set(S("AWS_SHARED_CREDENTIALS_FILE"), file, nullptr);

    S3Credentials credentials = { 0 };
    test_check(test, s3_credentials_from_environment(test->arena, &credentials, &err) == ERR_OK, err.msg);
    test_check_str(test, credentials.access_key, S("DEFAULTKEY"), "the default profile");
    (void)env_set(S("AWS_PROFILE"), S("work"), nullptr);
    test_check(test, s3_credentials_from_environment(test->arena, &credentials, &err) == ERR_OK, err.msg);
    test_check_str(test, credentials.session_token, S("worktoken"), "the named profile, with its token");
    (void)env_set(S("AWS_PROFILE"), S("absent"), nullptr);
    test_check(test, s3_credentials_from_environment(test->arena, &credentials, &err) == ERR_NOT_FOUND,
               "a profile the file lacks");
    (void)env_set(S("AWS_ACCESS_KEY_ID"), S("ENVKEY"), nullptr);
    (void)env_set(S("AWS_SECRET_ACCESS_KEY"), S("envsecret"), nullptr);
    test_check(test, s3_credentials_from_environment(test->arena, &credentials, &err) == ERR_OK, err.msg);
    test_check(test, str_equal(credentials.access_key, S("ENVKEY")) && credentials.session_token.len == 0,
               "the environment comes first");

    test_check(test, dir_remove_all(dir, &err) == ERR_OK, err.msg);
    for (size_t i = 0; i < countof(NAMES); i++) {
        restore_environment(NAMES[i], saved[i], was_set[i]);
    }
}

// s3_reaches_a_live_store runs only with LIBMC_S3_ENDPOINT and LIBMC_S3_BUCKET set, against a store that
// exists; LIBMC_S3_REGION and LIBMC_S3_PATH_STYLE=1 set the rest, as MinIO wants.
static void s3_reaches_a_live_store(Test *test)
{
    String endpoint = { 0 };
    String bucket = { 0 };
    if (!env_get(test->arena, S("LIBMC_S3_ENDPOINT"), &endpoint) || !env_get(test->arena, S("LIBMC_S3_BUCKET"), &bucket)) {
        return;
    }
    String region = { 0 };
    String path_style = { 0 };
    Err err = { 0 };
    S3Config config = {
        .endpoint = endpoint,
        .region = env_get(test->arena, S("LIBMC_S3_REGION"), &region) ? region : S(""),
        .bucket = bucket,
        .path_style = env_get(test->arena, S("LIBMC_S3_PATH_STYLE"), &path_style) && str_equal(path_style, S("1")),
        .part_size = 5 << 20,
    };
    test_check(test, s3_credentials_from_environment(test->arena, &config.credentials, &err) == ERR_OK, err.msg);
    S3Client *client = nullptr;
    test_check(test, s3_client_create(&config, &client, &err) == ERR_OK, err.msg);
    if (client == nullptr) {
        return;
    }
    test_check(test, s3_create_bucket(client, nullptr, test->arena, &err) == ERR_OK, err.msg);
    String prefix = str_format(test->arena, "libmc-test-%lld/", (long long)(clock_wall_ns() / NS_PER_MILLISECOND));
    String key = str_concat(test->arena, prefix, S("a æøå ÆØÅ+&=.txt"));
    const HttpHeader metadata[] = { { S("sha256"), S("live") } };
    S3PutOptions options = { .metadata = { metadata, countof(metadata) } };
    String etag = { 0 };
    test_check(test, s3_put(client, nullptr, test->arena, key, S("live"), &options, &etag, &err) == ERR_OK, err.msg);

    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-s3-live-"), &dir, &err) == ERR_OK, err.msg);
    String big_path = path_join(test->arena, dir, S("big"));
    String big = str_repeat(test->arena, S("0123456789abcdef"), (11 << 20) / 16);
    test_check(test, file_write_all(big_path, big, 0600, &err) == ERR_OK, err.msg);
    int fd = -1;
    test_check(test, file_open_append(big_path, &fd, &err) == ERR_OK, err.msg);
    String big_key = str_concat(test->arena, prefix, S("big"));
    test_check(test, s3_put_file(client, nullptr, test->arena, big_key, fd, (int64_t)big.len, nullptr, &etag, &err) == ERR_OK,
               err.msg);
    S3Object object = { 0 };
    test_check(test, s3_get_file(client, nullptr, test->arena, big_key, fd, &object, &err) == ERR_OK, err.msg);
    String back = { 0 };
    test_check(test, file_read_all(test->arena, big_path, &back, &err) == ERR_OK && str_equal(back, big),
               "a multipart upload comes back whole");
    test_check(test, s3_head(client, nullptr, test->arena, big_key, &object, &err) == ERR_OK &&
                         object.size == (int64_t)big.len,
               "HEAD of an object larger than max_response_bytes");
    file_close(fd);

    S3ObjectList objects = { 0 };
    test_check(test, s3_list(client, nullptr, test->arena, prefix, &objects, &err) == ERR_OK && objects.count == 2,
               "lists both");
    test_check(test, s3_head(client, nullptr, test->arena, key, &object, &err) == ERR_OK, err.msg);
    String value = { 0 };
    test_check(test, s3_metadata_get(&object, S("sha256"), &value) && str_equal(value, S("live")), "keeps metadata");
    test_check(test, s3_delete(client, nullptr, test->arena, key, &err) == ERR_OK, err.msg);
    test_check(test, s3_delete(client, nullptr, test->arena, big_key, &err) == ERR_OK, err.msg);
    test_check(test, s3_head(client, nullptr, test->arena, key, &object, &err) == ERR_NOT_FOUND, "gone");
    (void)dir_remove_all(dir, nullptr);
    s3_client_destroy(client);
}

const TestCase S3_TESTS[] = {
    { "s3_stores_reads_and_removes_objects", s3_stores_reads_and_removes_objects },
    { "s3_moves_files_in_parts", s3_moves_files_in_parts },
    { "s3_lists_every_page", s3_lists_every_page },
    { "s3_tries_again_and_stops", s3_tries_again_and_stops },
    { "s3_finds_credentials_like_the_aws_tools", s3_finds_credentials_like_the_aws_tools },
    { "s3_reaches_a_live_store", s3_reaches_a_live_store },
    { nullptr, nullptr },
};
