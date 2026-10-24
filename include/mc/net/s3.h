#pragma once

#include "mc/concurrency/cancel.h"
#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/net/http.h"
#include "mc/text/str.h"

// An S3 client for one bucket, on AWS S3 or a store that speaks its API, such
// as MinIO, Cloudflare R2 or Backblaze B2. Requests go over mc/net/http and
// are signed with AWS Signature Version 4. Transient failures, from the
// network or as 429 and 5xx statuses, are tried again with a short backoff.

typedef struct S3Credentials {
    String access_key;
    String secret_key;
    // session_token comes with temporary credentials; empty for long-term keys.
    String session_token;
} S3Credentials;

// s3_credentials_from_environment finds credentials the way the AWS tools do:
// AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY with AWS_SESSION_TOKEN, else the
// profile in AWS_PROFILE, or "default", of the shared credentials file at
// AWS_SHARED_CREDENTIALS_FILE or ~/.aws/credentials. ERR_NOT_FOUND when
// neither has a key pair.
[[nodiscard]] Error s3_credentials_from_environment(Arena *arena, S3Credentials *credentials, Err *err);

// S3Config holds what applies to every request. Zero values take the default
// named beside each field.
typedef struct S3Config {
    // endpoint is scheme://host[:port]: https://s3.<region>.amazonaws.com.
    // http:// is allowed, for a local store.
    String endpoint;
    // region signs the requests: us-east-1.
    String region;
    String bucket;
    // path_style puts the bucket in the path, endpoint/bucket/key, as MinIO
    // wants by default, instead of in the host name, bucket.endpoint/key.
    bool path_style;
    S3Credentials credentials;
    // part_size is the size of each part of a multipart upload, which
    // s3_put_file uses for files of at least this size: 64 MiB. S3 wants
    // every part but the last to be 5 MiB or more.
    int64_t part_size;
    // ca_file and user_agent are as in HttpClientConfig.
    String ca_file;
    String user_agent;
    // max_attempts bounds how often a request is tried: 3.
    int max_attempts;
    // stall_timeout_ms fails a transfer that moves no bytes for that long: 60 s.
    // Transfers have no other time limit, since a large file takes as long as
    // it takes.
    int64_t stall_timeout_ms;
} S3Config;

// S3Metadata is user metadata, x-amz-meta-<name> on the wire. Names are
// lowercase ASCII; S3 keeps them that way.
typedef struct S3Metadata {
    const HttpHeader *items;
    size_t count;
} S3Metadata;

typedef struct S3PutOptions {
    S3Metadata metadata;
    // content_type is stored with the object; empty leaves it to the store.
    String content_type;
    // storage_class such as STANDARD_IA or GLACIER_IR; empty is the bucket's default.
    String storage_class;
} S3PutOptions;

// S3Object describes a stored object. s3_list fills the key, size, etag and
// last_modified_ns; s3_head and s3_get also the content type and metadata.
typedef struct S3Object {
    String key;
    int64_t size;
    // etag is without its quotes. It is the MD5 of the content only for a
    // single-part upload without server-side encryption by KMS.
    String etag;
    int64_t last_modified_ns;
    String content_type;
    S3Metadata metadata;
} S3Object;

typedef struct S3ObjectList {
    S3Object *items;
    size_t count;
    size_t capacity;
} S3ObjectList;

// s3_metadata_get finds the user metadata called name, ignoring ASCII case.
bool s3_metadata_get(const S3Object *object, String name, String *value);

// S3Client may be used from several threads at once: each request borrows a
// connection of its own and returns it after, so connections are reused.
typedef struct S3Client S3Client;

// s3_client_create copies config. A missing bucket or access key, or an
// endpoint that is not http:// or https://, is ERR_INVALID_ARGUMENT.
[[nodiscard]] Error s3_client_create(const S3Config *config, S3Client **client, Err *err);
void s3_client_destroy(S3Client *client);

// Every operation takes an optional Cancel, which ends it with ERR_CANCELLED,
// and returns in arena what it hands back. A key that does not exist is
// ERR_NOT_FOUND; a status that is an error is ERR_NETWORK, with S3's code and
// message in err; the rest is as http_send returns it.

// s3_put stores body as the object key and returns its ETag.
[[nodiscard]] Error s3_put(S3Client *client, Cancel *cancel, Arena *arena, String key, String body,
                           const S3PutOptions *options, String *etag, Err *err);
// s3_put_file stores size bytes of the file from its start, read with pread,
// as one request below part_size and as a multipart upload from it on, which
// is aborted when a part fails.
[[nodiscard]] Error s3_put_file(S3Client *client, Cancel *cancel, Arena *arena, String key, int fd, int64_t size,
                                const S3PutOptions *options, String *etag, Err *err);
// s3_get reads the object into memory, up to 8 MiB; larger is ERR_UNSUPPORTED.
[[nodiscard]] Error s3_get(S3Client *client, Cancel *cancel, Arena *arena, String key, String *body, S3Object *object,
                           Err *err);
// s3_get_file writes the object into the regular file fd from its start,
// truncating it first, so a retry starts over. The file's position after is
// the end of what was written.
[[nodiscard]] Error s3_get_file(S3Client *client, Cancel *cancel, Arena *arena, String key, int fd, S3Object *object,
                                Err *err);
[[nodiscard]] Error s3_head(S3Client *client, Cancel *cancel, Arena *arena, String key, S3Object *object, Err *err);
// s3_delete removes the object; one that is already gone is ERR_OK, as S3 answers.
[[nodiscard]] Error s3_delete(S3Client *client, Cancel *cancel, Arena *arena, String key, Err *err);
// s3_list lists every object whose key starts with prefix, in key order,
// following ListObjectsV2's pages. Keys ending in / are listed like any other.
[[nodiscard]] Error s3_list(S3Client *client, Cancel *cancel, Arena *arena, String prefix, S3ObjectList *objects,
                            Err *err);
// s3_create_bucket creates the bucket; one the caller already owns is ERR_OK.
[[nodiscard]] Error s3_create_bucket(S3Client *client, Cancel *cancel, Arena *arena, Err *err);
