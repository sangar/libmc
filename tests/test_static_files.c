#include "test.h"

#include "mc/encoding/gzip.h"
#include "mc/net/static_files.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

// Declared here rather than via <unistd.h>, which also declares write and
// would clash with this file's own write helper.
int symlink(const char *target, const char *linkpath);

// The tests serve files from a temporary directory, public/, with a secret
// file beside it that no request may reach.

enum { MAX_CACHED_FILE_BYTES = 1024, LARGE_FILE_BYTES = 5000 };

typedef struct Site {
    String directory; // holds public/ and secret.txt
    String root;
    String page; // index.html's content, small enough to cache
    StaticFiles *files;
} Site;

static bool write(Test *test, const Site *site, const char *name, String content)
{
    String path = str_format(test->arena, "%.*s/%s", (int)site->root.len, site->root.data, name);
    return file_write_all(path, content, 0600, nullptr) == ERR_OK;
}

static bool site_start(Test *test, Site *site)
{
    *site = (Site){ .page = str_repeat(test->arena, S("<p>blåbær og bringebær</p>\n"), 30) };
    Err err = { 0 };
    bool started = dir_create_temp(test->arena, S("libmc-static-"), &site->directory, &err) == ERR_OK;
    if (started) {
        site->root = path_join(test->arena, site->directory, S("public"));
        StaticFilesConfig config = { .root = site->root, .max_cached_file_bytes = MAX_CACHED_FILE_BYTES };
        started = dir_create_all(path_join(test->arena, site->root, S("sub")), 0700, &err) == ERR_OK &&
                  write(test, site, "index.html", site->page) && write(test, site, "style.css", S("p { color: red }")) &&
                  write(test, site, "sub/page.html", S("<p>sub</p>")) &&
                  write(test, site, "large.bin", str_repeat(test->arena, S("x"), LARGE_FILE_BYTES)) &&
                  file_write_all(path_join(test->arena, site->directory, S("secret.txt")), S("secret"), 0600, &err) ==
                      ERR_OK &&
                  static_files_create(&config, &site->files, &err) == ERR_OK;
    }
    test_check(test, started, err.msg);
    return started;
}

static void site_stop(Site *site)
{
    if (site->files != nullptr) {
        static_files_destroy(site->files);
    }
    if (site->directory.len > 0) {
        unused(dir_remove_all(site->directory, nullptr));
    }
}

// get asks for path with one header, if given, and reports whether the files answered.
static bool get(Test *test, const Site *site, const char *method, const char *path, const char *name, const char *value,
                HttpServerResponse *response)
{
    HttpHeader header = { S(name != nullptr ? name : "X-None"), S(value != nullptr ? value : "") };
    HttpServerRequest request = { .method = S(method), .target = S(path), .path = S(path), .headers = &header, .header_count = 1 };
    *response = (HttpServerResponse){ 0 };
    return static_files_serve(site->files, test->arena, &request, response);
}

static String header(const HttpServerResponse *response, const char *name)
{
    for (size_t i = 0; i < response->header_count; i++) {
        if (str_equal_ignore_case(response->headers[i].name, S(name))) {
            return response->headers[i].value;
        }
    }
    return S("");
}

static void static_files_serve_a_file_with_its_type_and_etag(Test *test)
{
    Site site;
    HttpServerResponse response;
    if (site_start(test, &site) && get(test, &site, "GET", "/style.css", nullptr, nullptr, &response)) {
        test_check_str(test, response.body, S("p { color: red }"), "the file");
        test_check_str(test, header(&response, "Content-Type"), S("text/css; charset=utf-8"), "its type");
        test_check_str(test, header(&response, "X-Content-Type-Options"), S("nosniff"), "no sniffing");
        String etag = header(&response, "ETag");
        test_check(test, etag.len > 2, "an ETag");
        const char *tags = str_format(test->arena, "\"other\", %.*s", (int)etag.len, etag.data).data;
        test_check(test, get(test, &site, "GET", "/style.css", "If-None-Match", tags, &response) && response.status == 304 &&
                             response.body.len == 0,
                   "a known version gets 304");
    } else {
        test_check(test, false, "serves style.css");
    }
    site_stop(&site);
}

static void static_files_serve_indexes_gzipped_from_the_cache(Test *test)
{
    Site site;
    HttpServerResponse response;
    if (site_start(test, &site)) {
        test_check(test, get(test, &site, "GET", "/", nullptr, nullptr, &response) && str_equal(response.body, site.page),
                   "a directory serves its index.html");
        test_check(test, get(test, &site, "GET", "/sub/page.html", nullptr, nullptr, &response) &&
                             str_equal(response.body, S("<p>sub</p>")),
                   "a file below");
        for (int i = 0; i < 2; i++) {
            String back = { 0 };
            bool gzipped = get(test, &site, "GET", "/index.html", "Accept-Encoding", "gzip", &response) &&
                           str_equal(header(&response, "Content-Encoding"), S("gzip")) &&
                           gzip_decompress(test->arena, response.body, site.page.len, &back, nullptr) == ERR_OK &&
                           str_equal(back, site.page);
            test_check(test, gzipped, i == 0 ? "gzipped when accepted" : "and again from the cache");
        }
        test_check(test, get(test, &site, "GET", "/large.bin", "Accept-Encoding", "gzip", &response) &&
                             response.body.len == LARGE_FILE_BYTES && header(&response, "Content-Encoding").len == 0,
                   "a file too large to cache is read, as it is");
    }
    site_stop(&site);
}

static void static_files_keep_requests_under_the_root(Test *test)
{
    static const char *const ESCAPES[] = {
        "/../secret.txt", "/%2e%2e/secret.txt", "/sub/../../secret.txt", "/%2E%2E%2Fsecret.txt", "/style.css%00.html", "/%",
        "/escape.txt", // a symlink under root pointing at the secret outside it
    };
    Site site;
    if (site_start(test, &site)) {
        String outside = path_join(test->arena, site.directory, S("secret.txt"));
        String escape = path_join(test->arena, site.root, S("escape.txt"));
        String inside = path_join(test->arena, site.root, S("alias.html"));
        test_check(test, symlink(str_cstr(test->arena, outside), str_cstr(test->arena, escape)) == 0, "escape symlink");
        test_check(test, symlink("index.html", str_cstr(test->arena, inside)) == 0, "in-root symlink");
        for (size_t i = 0; i < countof(ESCAPES); i++) {
            HttpServerResponse response;
            bool served = get(test, &site, "GET", ESCAPES[i], nullptr, nullptr, &response);
            test_check(test, !served || !str_contains(response.body, S("secret")), ESCAPES[i]);
        }
        HttpServerResponse response;
        test_check(test, get(test, &site, "GET", "/alias.html", nullptr, nullptr, &response) &&
                             str_equal(response.body, site.page),
                   "a symlink within root is followed");
    }
    site_stop(&site);
}

static void static_files_read_a_changed_file_again(Test *test)
{
    Site site;
    HttpServerResponse response;
    if (site_start(test, &site)) {
        test_check(test, get(test, &site, "GET", "/style.css", nullptr, nullptr, &response), "cache style.css");
        test_check(test, write(test, &site, "style.css", S("p { color: green; margin: 0 }")), "change it");
        test_check(test, get(test, &site, "GET", "/style.css", nullptr, nullptr, &response) &&
                             str_equal(response.body, S("p { color: green; margin: 0 }")),
                   "the change is served");
    }
    site_stop(&site);
}

static void static_files_refuse_other_methods_and_pass_on_missing_files(Test *test)
{
    Site site;
    HttpServerResponse response;
    if (site_start(test, &site)) {
        test_check(test, get(test, &site, "POST", "/style.css", nullptr, nullptr, &response) && response.status == 405 &&
                             str_equal(header(&response, "Allow"), S("GET, HEAD")),
                   "POST gets 405");
        test_check(test, !get(test, &site, "GET", "/missing.css", nullptr, nullptr, &response),
                   "a missing file is left to the caller");
    }
    site_stop(&site);
}

const TestCase STATIC_FILES_TESTS[] = {
    { "static_files_serve_a_file_with_its_type_and_etag", static_files_serve_a_file_with_its_type_and_etag },
    { "static_files_serve_indexes_gzipped_from_the_cache", static_files_serve_indexes_gzipped_from_the_cache },
    { "static_files_keep_requests_under_the_root", static_files_keep_requests_under_the_root },
    { "static_files_read_a_changed_file_again", static_files_read_a_changed_file_again },
    { "static_files_refuse_other_methods_and_pass_on_missing_files",
      static_files_refuse_other_methods_and_pass_on_missing_files },
    { nullptr, nullptr },
};
