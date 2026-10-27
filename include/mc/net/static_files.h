#pragma once

#include <stddef.h>

#include "mc/core/error.h"
#include "mc/net/http_server.h"

// StaticFiles serves the files under a directory, as a web app's public
// assets, keeping recently served ones in memory, after Basecamp's Thruster.
typedef struct StaticFiles StaticFiles;

typedef struct StaticFilesConfig {
    String root;          // the directory served
    String prefix;        // the URL path it is served under: "/", or "/assets/"
    String cache_control; // sent with each file, such as "public, max-age=3600"; empty sends none
    size_t cache_bytes;   // memory for cached files, their gzipped copies counted: 64 MiB
    size_t max_cached_file_bytes; // larger files are read for each request: 1 MiB
    // max_file_bytes bounds what is served at all, as a response is held in
    // memory: 64 MiB.
    size_t max_file_bytes;
} StaticFilesConfig;

// static_files_create copies config, with zero fields taking the defaults
// beside them. The caller owns the result; handlers on several threads may
// share it.
[[nodiscard]] Error static_files_create(const StaticFilesConfig *config, StaticFiles **files, Err *err);
void static_files_destroy(StaticFiles *files);

// static_files_serve answers request from the files, in arena, when its
// path is under the prefix and names a regular file, or a directory with an
// index.html, no larger than max_file_bytes, and reports whether it did;
// otherwise the caller answers. The path is percent-decoded and cleaned, so
// no request path reaches outside root; symlinks under root are followed, as
// its owner put them there. A file goes with its Content-Type by its
// extension and an ETag, which If-None-Match turns into 304. A file small
// enough to cache goes gzipped when the client accepts it and that is
// smaller; larger ones are left to the server's compression. Methods but
// GET and HEAD get 405.
bool static_files_serve(StaticFiles *files, Arena *arena, const HttpServerRequest *request,
                        HttpServerResponse *response);
