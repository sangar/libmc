#include "mc/net/static_files.h"

#include "mc/encoding/gzip.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "http_compress.h"

enum {
    DEFAULT_CACHE_BYTES = 64 << 20,
    DEFAULT_MAX_CACHED_FILE_BYTES = 1 << 20,
    DEFAULT_MAX_FILE_BYTES = 64 << 20,
    MAX_CACHED_FILES = 4096,
    MAX_RESPONSE_HEADERS = 6,
};

// CachedFile is one file's content as read when its size and mtime were
// these, with a gzipped copy when that is smaller.
typedef struct CachedFile {
    Arena *arena;
    String path;
    int64_t size;
    int64_t mtime_ns;
    String content;
    String gzipped;
    uint64_t last_used;
} CachedFile;

struct StaticFiles {
    Arena *arena;
    StaticFilesConfig config;
    Mutex mutex; // guards the cache
    CachedFile *cached[MAX_CACHED_FILES];
    size_t cached_count;
    size_t cached_bytes;
    uint64_t clock;
};

static size_t or_default(size_t value, size_t fallback)
{
    return value != 0 ? value : fallback;
}

Error static_files_create(const StaticFilesConfig *config, StaticFiles **files, Err *err)
{
    FileInfo info = { 0 };
    Error e = file_info_follow(config->root, &info, err);
    if (e == ERR_OK && !info.is_dir) {
        e = err_set(err, ERR_NOT_FOUND, "static: %.*s is not a directory", (int)config->root.len, config->root.data);
    }
    if (e != ERR_OK) {
        return e;
    }
    Arena *arena = arena_create(0);
    StaticFiles *created = arena_push(arena, sizeof *created);
    *created = (StaticFiles){
        .arena = arena,
        .config = {
            .root = str_copy(arena, config->root),
            .prefix = str_copy(arena, config->prefix.len > 0 ? config->prefix : S("/")),
            .cache_control = str_copy(arena, config->cache_control),
            .cache_bytes = or_default(config->cache_bytes, DEFAULT_CACHE_BYTES),
            .max_cached_file_bytes = or_default(config->max_cached_file_bytes, DEFAULT_MAX_CACHED_FILE_BYTES),
            .max_file_bytes = or_default(config->max_file_bytes, DEFAULT_MAX_FILE_BYTES),
        },
    };
    mutex_init(&created->mutex);
    *files = created;
    return ERR_OK;
}

void static_files_destroy(StaticFiles *files)
{
    for (size_t i = 0; i < files->cached_count; i++) {
        arena_destroy(files->cached[i]->arena);
    }
    mutex_destroy(&files->mutex);
    arena_destroy(files->arena);
}

static String content_type(String path)
{
    static const struct {
        const char *extension;
        const char *type;
    } TYPES[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm", "text/html; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" },
        { ".mjs", "text/javascript; charset=utf-8" },
        { ".json", "application/json" },
        { ".map", "application/json" },
        { ".webmanifest", "application/manifest+json" },
        { ".txt", "text/plain; charset=utf-8" },
        { ".xml", "application/xml" },
        { ".svg", "image/svg+xml" },
        { ".png", "image/png" },
        { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" },
        { ".webp", "image/webp" },
        { ".avif", "image/avif" },
        { ".ico", "image/x-icon" },
        { ".woff", "font/woff" },
        { ".woff2", "font/woff2" },
        { ".ttf", "font/ttf" },
        { ".wasm", "application/wasm" },
        { ".pdf", "application/pdf" },
    };
    String extension = path_ext(path);
    for (size_t i = 0; i < countof(TYPES); i++) {
        if (str_equal_ignore_case(extension, S(TYPES[i].extension))) {
            return S(TYPES[i].type);
        }
    }
    return S("application/octet-stream");
}

// under_root reports whether the canonical path stays within the canonical
// root, so a symlink inside root cannot serve a file outside it. path_clean on
// the request only stops .. at the virtual root; a symlink escapes that, which
// only resolving the real path and comparing catches.
static bool under_root(Arena *arena, String root, String path)
{
    String real_root = { 0 };
    String real_path = { 0 };
    if (path_resolve(arena, root, &real_root, nullptr) != ERR_OK ||
        path_resolve(arena, path, &real_path, nullptr) != ERR_OK) {
        return false;
    }
    return str_equal(real_path, real_root) ||
           str_starts_with(real_path, str_format(arena, "%.*s/", (int)real_root.len, real_root.data));
}

// file_for maps the request's path to a file under root: decoded, cleaned so
// .. stops at root, a directory's index.html, and the real path kept in root.
static bool file_for(const StaticFiles *files, Arena *arena, String request_path, String *path, FileInfo *info)
{
    String prefix = files->config.prefix;
    String decoded = { 0 };
    if (!str_starts_with(request_path, prefix) ||
        !str_percent_decode(arena, str_slice(request_path, prefix.len, request_path.len), &decoded) ||
        str_contains(decoded, (String){ "\0", 1 })) {
        return false;
    }
    String within = path_clean(arena, str_format(arena, "/%.*s", (int)decoded.len, decoded.data));
    *path = str_format(arena, "%.*s%.*s", (int)files->config.root.len, files->config.root.data, (int)within.len,
                       within.data);
    if (file_info_follow(*path, info, nullptr) != ERR_OK || !info->exists) {
        return false;
    }
    if (info->is_dir) {
        *path = path_join(arena, *path, S("index.html"));
        if (file_info_follow(*path, info, nullptr) != ERR_OK || !info->exists) {
            return false;
        }
    }
    return info->is_regular && info->size >= 0 && (size_t)info->size <= files->config.max_file_bytes &&
           under_root(arena, files->config.root, *path);
}

// matches says whether If-None-Match names etag, or is * (RFC 9110 13.1.2),
// comparing weakly as a GET may.
static bool matches(Arena *arena, String if_none_match, String etag)
{
    StringList tags = str_split(arena, if_none_match, ',');
    for (size_t i = 0; i < tags.count; i++) {
        String tag = str_trim(tags.items[i]);
        tag = str_starts_with(tag, S("W/")) ? str_slice(tag, 2, tag.len) : tag;
        if (str_equal(tag, S("*")) || str_equal(tag, etag)) {
            return true;
        }
    }
    return false;
}

static size_t cached_size(const CachedFile *file)
{
    return file->content.len + file->gzipped.len;
}

// cache_find copies the cached content of path into arena while the file
// is unchanged, the gzipped copy when gzipped and there is one.
static bool cache_find(StaticFiles *files, Arena *arena, String path, const FileInfo *info, bool gzipped,
                       String *content, bool *was_gzipped)
{
    bool found = false;
    mutex_lock(&files->mutex);
    for (size_t i = 0; i < files->cached_count && !found; i++) {
        CachedFile *file = files->cached[i];
        if (!str_equal(file->path, path) || file->size != info->size || file->mtime_ns != info->mtime_ns) {
            continue;
        }
        found = true;
        file->last_used = ++files->clock;
        *was_gzipped = gzipped && file->gzipped.len > 0;
        *content = str_copy(arena, *was_gzipped ? file->gzipped : file->content);
    }
    mutex_unlock(&files->mutex);
    return found;
}

static void cache_remove(StaticFiles *files, size_t index)
{
    CachedFile *file = files->cached[index];
    files->cached_bytes -= cached_size(file);
    files->cached[index] = files->cached[--files->cached_count];
    arena_destroy(file->arena);
}

// cache_add keeps file, dropping the least recently used ones and any older
// copy of the same path until it fits.
static void cache_add(StaticFiles *files, CachedFile *file)
{
    mutex_lock(&files->mutex);
    for (size_t i = 0; i < files->cached_count; i++) {
        if (str_equal(files->cached[i]->path, file->path)) {
            cache_remove(files, i);
            break;
        }
    }
    while (files->cached_count > 0 && (files->cached_count == MAX_CACHED_FILES ||
                                       files->cached_bytes + cached_size(file) > files->config.cache_bytes)) {
        size_t oldest = 0;
        for (size_t i = 1; i < files->cached_count; i++) {
            oldest = files->cached[i]->last_used < files->cached[oldest]->last_used ? i : oldest;
        }
        cache_remove(files, oldest);
    }
    file->last_used = ++files->clock;
    files->cached[files->cached_count++] = file;
    files->cached_bytes += cached_size(file);
    mutex_unlock(&files->mutex);
}

// read_file reads path into arena, and keeps it in the cache when it is small enough.
static bool read_file(StaticFiles *files, Arena *arena, String path, const FileInfo *info, bool gzipped,
                      String *content, bool *was_gzipped)
{
    *was_gzipped = false;
    if ((size_t)info->size > files->config.max_cached_file_bytes) {
        return file_read_all(arena, path, content, nullptr) == ERR_OK;
    }
    Arena *cached_arena = arena_create(0);
    CachedFile *file = arena_push(cached_arena, sizeof *file);
    *file = (CachedFile){
        .arena = cached_arena,
        .path = str_copy(cached_arena, path),
        .size = info->size,
        .mtime_ns = info->mtime_ns,
    };
    if (file_read_all(cached_arena, path, &file->content, nullptr) != ERR_OK) {
        arena_destroy(cached_arena);
        return false;
    }
    String squeezed = { 0 };
    if (http_compressible(content_type(path)) && gzip_compress(cached_arena, file->content, nullptr, &squeezed, nullptr) == ERR_OK &&
        squeezed.len < file->content.len) {
        file->gzipped = squeezed;
    }
    *was_gzipped = gzipped && file->gzipped.len > 0;
    *content = str_copy(arena, *was_gzipped ? file->gzipped : file->content);
    if (cached_size(file) > files->config.cache_bytes) {
        arena_destroy(cached_arena);
    } else {
        cache_add(files, file);
    }
    return true;
}

static void add_header(Arena *arena, HttpServerResponse *response, const char *name, String value)
{
    if (response->headers == nullptr) {
        response->headers = arena_push(arena, MAX_RESPONSE_HEADERS * sizeof *response->headers);
    }
    response->headers[response->header_count++] = (HttpHeader){ S(name), value };
}

bool static_files_serve(StaticFiles *files, Arena *arena, const HttpServerRequest *request,
                        HttpServerResponse *response)
{
    String path = { 0 };
    FileInfo info = { 0 };
    if (!file_for(files, arena, request->path, &path, &info)) {
        return false;
    }
    *response = (HttpServerResponse){ .status = 200 };
    if (!str_equal(request->method, S("GET")) && !str_equal(request->method, S("HEAD"))) {
        response->status = 405;
        add_header(arena, response, "Allow", S("GET, HEAD"));
        return true;
    }
    String etag = str_format(arena, "\"%llx-%llx\"", (unsigned long long)info.size, (unsigned long long)info.mtime_ns);
    String type = content_type(path);
    add_header(arena, response, "ETag", etag);
    if (files->config.cache_control.len > 0) {
        add_header(arena, response, "Cache-Control", files->config.cache_control);
    }
    if (http_compressible(type)) {
        add_header(arena, response, "Vary", S("Accept-Encoding"));
    }
    String if_none_match = S("");
    http_header_find(request->headers, request->header_count, S("If-None-Match"), &if_none_match);
    if (if_none_match.len > 0 && matches(arena, if_none_match, etag)) {
        response->status = 304;
        return true;
    }
    bool gzipped = http_accepts_gzip(arena, request);
    bool was_gzipped = false;
    if (!cache_find(files, arena, path, &info, gzipped, &response->body, &was_gzipped) &&
        !read_file(files, arena, path, &info, gzipped, &response->body, &was_gzipped)) {
        *response = (HttpServerResponse){ 0 };
        return false;
    }
    add_header(arena, response, "Content-Type", type);
    add_header(arena, response, "X-Content-Type-Options", S("nosniff"));
    if (was_gzipped) {
        add_header(arena, response, "Content-Encoding", S("gzip"));
    }
    return true;
}
