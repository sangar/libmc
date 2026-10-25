// Self-bootstrapping build program. Bootstrap once with:
//     cc tools/build.c -o nob
// Afterwards ./nob rebuilds itself when this file changes.
//
//     ./nob                 debug library with sanitizers  -> out/debug/libmc.a
//     ./nob release         optimised library              -> out/release/libmc.a
//     ./nob test            build and run the tests (debug)
//     ./nob check           modern-c contract check, then the tests
//     ./nob cross           compile and archive for every target with zig cc
//     ./nob clean           remove out/
//     ./nob hash <dir>      print the deps.lock hash of a directory
//
// Every mode but clean and hash first fetches the dependencies in deps.lock.
// Host builds compile with $CC, or cc when it is unset.

#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum { MAX_ARGS = 1024, MAX_FILES = 512, MAX_JOBS = 16, PATH_MAX_LEN = 1024, FINGERPRINT_LEN = 4096 };

typedef struct {
    const char *items[MAX_ARGS];
    size_t count;
} Command;

typedef struct {
    char items[MAX_FILES][PATH_MAX_LEN];
    size_t count;
} FileList;

typedef struct {
    const char *name;
    const char *zig_target; // NULL builds with the host's cc
    // os names the directory of curl's configuration in src/net/curl.
    const char *os;
} Target;

// Linux builds link against glibc 2.39 (Ubuntu 24.04, Debian 13): curl's
// configuration in src/net/curl/linux was generated there, and zig's
// default, older glibc lacks functions it uses, such as arc4random.
static const Target TARGETS[] = {
    { "linux-x86_64", "x86_64-linux-gnu.2.39", "linux" },
    { "linux-aarch64", "aarch64-linux-gnu.2.39", "linux" },
    { "macos-aarch64", "aarch64-macos", "macos" },
};

static const char *const WARNING_FLAGS[] = {
    "-std=c23", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow", "-Wvla",
    "-Wstrict-prototypes", "-Wimplicit-fallthrough", "-Wno-unused-parameter",
};
static const char *const DEBUG_FLAGS[] = { "-g", "-O0", "-fsanitize=address,undefined", "-fno-omit-frame-pointer" };
static const char *const RELEASE_FLAGS[] = { "-O2" };
static const char *const AREAS[] = { "core", "text", "container", "crypto", "platform", "concurrency", "encoding", "log", "net" };


// Dependency is a vendored library, pinned in deps.lock. It compiles in its
// own standard without the warning flags: its warnings are upstream's to fix.
typedef struct {
    const char *const *directories;
    size_t directory_count;
    const char *const *flags;
    size_t flag_count;
    // curl_config adds src/net/curl/<os>, which holds the system's curl_config.h.
    bool curl_config;
} Dependency;

static const char *const CYAML_DIRECTORIES[] = { "deps/cyaml" };
static const char *const UTF8PROC_DIRECTORIES[] = { "deps/utf8proc" };
static const char *const YYJSON_DIRECTORIES[] = { "deps/yyjson" };
static const char *const C11_FLAGS[] = { "-std=c11", "-w" };

static const char *const MBEDTLS_DIRECTORIES[] = {
    "deps/mbedtls/library",           "deps/mbedtls/tf-psa-crypto/core",     "deps/mbedtls/tf-psa-crypto/drivers/builtin/src",
    "deps/mbedtls/tf-psa-crypto/extras", "deps/mbedtls/tf-psa-crypto/platform", "deps/mbedtls/tf-psa-crypto/utilities",
};
// mbedTLS and everything that includes its headers read libmc's additions to its configuration.
#define MBEDTLS_PUBLIC_FLAGS                                                                                         \
    "-Ideps/mbedtls/include", "-Ideps/mbedtls/tf-psa-crypto/include",                                               \
        "-Ideps/mbedtls/tf-psa-crypto/drivers/builtin/include", "-Isrc/net",                                        \
        "-DTF_PSA_CRYPTO_USER_CONFIG_FILE=\"mbedtls_user_config.h\""
static const char *const MBEDTLS_FLAGS[] = {
    "-std=c11", "-w", MBEDTLS_PUBLIC_FLAGS, "-Ideps/mbedtls/library", "-Ideps/mbedtls/tf-psa-crypto/core",
    "-Ideps/mbedtls/tf-psa-crypto/dispatch", "-Ideps/mbedtls/tf-psa-crypto/extras",
    "-Ideps/mbedtls/tf-psa-crypto/platform", "-Ideps/mbedtls/tf-psa-crypto/utilities",
    "-Ideps/mbedtls/tf-psa-crypto/drivers/builtin/src",
};

static const char *const NGHTTP2_DIRECTORIES[] = { "deps/nghttp2/lib" };
#define NGHTTP2_PUBLIC_FLAGS "-Ideps/nghttp2/lib/includes", "-DNGHTTP2_STATICLIB"
// nghttp2 builds without its configure step: the HAVE_ macros are what it finds on
// macOS and Linux, and glibc declares clock_gettime only for a GNU or POSIX dialect.
static const char *const NGHTTP2_FLAGS[] = {
    "-std=gnu11", "-w", NGHTTP2_PUBLIC_FLAGS, "-DBUILDING_NGHTTP2", "-DHAVE_ARPA_INET_H", "-DHAVE_NETINET_IN_H",
    "-DHAVE_CLOCK_GETTIME", "-DHAVE_DECL_CLOCK_MONOTONIC=1",
};

static const char *const ZLIB_DIRECTORIES[] = { "deps/zlib" };
static const char *const CACERT_DIRECTORIES[] = { "deps/cacert" };
static const char *const MD4C_DIRECTORIES[] = { "deps/md4c" };
static const char *const EXPAT_DIRECTORIES[] = { "deps/expat/lib" };
// xmltok.c includes xmltok_impl.c and xmltok_ns.c, which are kept in lib/included so they are not compiled alone.
static const char *const EXPAT_FLAGS[] = { "-std=c11", "-w", "-Isrc/encoding", "-Ideps/expat/lib/included" };
// HAVE_UNISTD_H is what zlib's configure step finds on macOS and Linux.
static const char *const ZLIB_FLAGS[] = { "-std=c11", "-w", "-DHAVE_UNISTD_H=1" };

static const char *const CURL_DIRECTORIES[] = {
    "deps/curl/lib", "deps/curl/lib/curlx", "deps/curl/lib/vauth", "deps/curl/lib/vdns",
    "deps/curl/lib/vquic", "deps/curl/lib/vssh", "deps/curl/lib/vtls",
};
// curl wants a GNU dialect: macOS declares memset_s only for one.
static const char *const CURL_FLAGS[] = {
    "-std=gnu11", "-w", "-DHAVE_CONFIG_H", "-DBUILDING_LIBCURL", "-DCURL_STATICLIB", "-Ideps/curl/include",
    "-Ideps/curl/lib", MBEDTLS_PUBLIC_FLAGS, NGHTTP2_PUBLIC_FLAGS, "-Ideps/zlib",
};

// libmc's own sources and tests also see the public headers of curl, mbedTLS
// and nghttp2: src/net/http.c includes curl's, and the HTTP tests run a TLS
// server that answers HTTP/2.
static const char *const LIBMC_FLAGS[] = {
    "-Iinclude", "-Ideps/cyaml", "-Ideps/utf8proc", "-Ideps/yyjson", "-Ideps/curl/include", "-DCURL_STATICLIB",
    "-Ideps/cacert", "-Ideps/md4c", "-Ideps/expat/lib", MBEDTLS_PUBLIC_FLAGS, NGHTTP2_PUBLIC_FLAGS,
};

static const Dependency DEPENDENCIES[] = {
    { CYAML_DIRECTORIES, sizeof CYAML_DIRECTORIES / sizeof *CYAML_DIRECTORIES, C11_FLAGS,
      sizeof C11_FLAGS / sizeof *C11_FLAGS, false },
    { UTF8PROC_DIRECTORIES, sizeof UTF8PROC_DIRECTORIES / sizeof *UTF8PROC_DIRECTORIES, C11_FLAGS,
      sizeof C11_FLAGS / sizeof *C11_FLAGS, false },
    { YYJSON_DIRECTORIES, sizeof YYJSON_DIRECTORIES / sizeof *YYJSON_DIRECTORIES, C11_FLAGS,
      sizeof C11_FLAGS / sizeof *C11_FLAGS, false },
    { MBEDTLS_DIRECTORIES, sizeof MBEDTLS_DIRECTORIES / sizeof *MBEDTLS_DIRECTORIES, MBEDTLS_FLAGS,
      sizeof MBEDTLS_FLAGS / sizeof *MBEDTLS_FLAGS, false },
    { NGHTTP2_DIRECTORIES, sizeof NGHTTP2_DIRECTORIES / sizeof *NGHTTP2_DIRECTORIES, NGHTTP2_FLAGS,
      sizeof NGHTTP2_FLAGS / sizeof *NGHTTP2_FLAGS, false },
    { ZLIB_DIRECTORIES, sizeof ZLIB_DIRECTORIES / sizeof *ZLIB_DIRECTORIES, ZLIB_FLAGS,
      sizeof ZLIB_FLAGS / sizeof *ZLIB_FLAGS, false },
    { CACERT_DIRECTORIES, sizeof CACERT_DIRECTORIES / sizeof *CACERT_DIRECTORIES, C11_FLAGS,
      sizeof C11_FLAGS / sizeof *C11_FLAGS, false },
    { MD4C_DIRECTORIES, sizeof MD4C_DIRECTORIES / sizeof *MD4C_DIRECTORIES, C11_FLAGS,
      sizeof C11_FLAGS / sizeof *C11_FLAGS, false },
    { EXPAT_DIRECTORIES, sizeof EXPAT_DIRECTORIES / sizeof *EXPAT_DIRECTORIES, EXPAT_FLAGS,
      sizeof EXPAT_FLAGS / sizeof *EXPAT_FLAGS, false },
    { CURL_DIRECTORIES, sizeof CURL_DIRECTORIES / sizeof *CURL_DIRECTORIES, CURL_FLAGS,
      sizeof CURL_FLAGS / sizeof *CURL_FLAGS, true },
};

// HEADER_DIRECTORIES are the dependency headers whose changes rebuild everything.
static const char *const HEADER_DIRECTORIES[] = {
    "deps/cyaml", "deps/utf8proc", "deps/yyjson", "deps/curl/include/curl", "deps/mbedtls/include/mbedtls",
    "deps/mbedtls/tf-psa-crypto/include/psa", "deps/nghttp2/lib/includes/nghttp2", "deps/zlib", "deps/cacert",
    "deps/md4c", "deps/expat/lib", "src/encoding", "src/net",
};

static void command_push(Command *command, const char *item)
{
    if (command->count + 1 >= MAX_ARGS) {
        fprintf(stderr, "build: too many arguments\n");
        exit(2);
    }
    command->items[command->count++] = item;
    command->items[command->count] = NULL;
}

static void command_push_many(Command *command, const char *const *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        command_push(command, items[i]);
    }
}

static int command_run(const Command *command)
{
    for (size_t i = 0; i < command->count; i++) {
        printf("%s%s", i == 0 ? "" : " ", command->items[i]);
    }
    printf("\n");
    fflush(stdout);
    pid_t pid;
    int status = posix_spawnp(&pid, command->items[0], NULL, NULL, (char *const *)command->items, environ);
    if (status != 0) {
        fprintf(stderr, "build: cannot run %s: %s\n", command->items[0], strerror(status));
        return 1;
    }
    int wait_status = 0;
    if (waitpid(pid, &wait_status, 0) < 0) {
        return 1;
    }
    return WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 1;
}

static void run_or_die(const Command *command)
{
    int status = command_run(command);
    if (status != 0) {
        fprintf(stderr, "build: command failed with status %d\n", status);
        exit(status);
    }
}

static time_t mtime_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void mkdir_p(const char *path)
{
    char buffer[PATH_MAX_LEN];
    snprintf(buffer, sizeof buffer, "%s", path);
    for (char *p = buffer + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buffer, 0755);
            *p = '/';
        }
    }
    mkdir(buffer, 0755);
}

static void list_files(FileList *list, const char *directory, const char *extension)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        size_t ext_len = strlen(extension);
        if (name_len <= ext_len || strcmp(entry->d_name + name_len - ext_len, extension) != 0) {
            continue;
        }
        if (list->count >= MAX_FILES) {
            fprintf(stderr, "build: too many files in %s\n", directory);
            exit(2);
        }
        snprintf(list->items[list->count++], PATH_MAX_LEN, "%s/%s", directory, entry->d_name);
    }
    closedir(dir);
}

static time_t newest_header_mtime(void)
{
    FileList headers = { 0 };
    for (size_t i = 0; i < sizeof AREAS / sizeof *AREAS; i++) {
        char directory[PATH_MAX_LEN];
        snprintf(directory, sizeof directory, "include/mc/%s", AREAS[i]);
        list_files(&headers, directory, ".h");
        snprintf(directory, sizeof directory, "src/%s", AREAS[i]);
        list_files(&headers, directory, ".h");
    }
    for (size_t i = 0; i < sizeof HEADER_DIRECTORIES / sizeof *HEADER_DIRECTORIES; i++) {
        list_files(&headers, HEADER_DIRECTORIES[i], ".h");
    }
    list_files(&headers, "tests", ".h");
    time_t newest = 0;
    for (size_t i = 0; i < headers.count; i++) {
        time_t t = mtime_of(headers.items[i]);
        newest = t > newest ? t : newest;
    }
    return newest;
}

typedef struct {
    bool release;
    // dependency is the vendored library being compiled, NULL for libmc's own sources.
    const Dependency *dependency;
    const Target *target;
    char out_dir[PATH_MAX_LEN];
    char obj_dir[PATH_MAX_LEN];
    // curl_config_flag is -I for the curl configuration of the target's system.
    char curl_config_flag[PATH_MAX_LEN];
    time_t headers_mtime;
} Build;

// host_compiler is $CC when set, so one checkout can build with clang and gcc.
static const char *host_compiler(void)
{
    const char *compiler = getenv("CC");
    return compiler != NULL && compiler[0] != '\0' ? compiler : "cc";
}

static void push_compiler(Command *command, const Build *build)
{
    if (build->target != NULL && build->target->zig_target != NULL) {
        command_push(command, "zig");
        command_push(command, "cc");
        command_push(command, "-target");
        command_push(command, build->target->zig_target);
    } else {
        command_push(command, host_compiler());
    }
}

// Sanitizers need the host's runtime, so cross builds only compile.
static bool sanitized(const Build *build)
{
    return !build->release && build->target == NULL;
}

static void object_path(char *out, const char *obj_dir, const char *source)
{
    char flat[PATH_MAX_LEN];
    snprintf(flat, sizeof flat, "%s", source);
    for (char *p = flat; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '_';
        }
    }
    flat[strlen(flat) - 2] = '\0';
    snprintf(out, PATH_MAX_LEN, "%s/%s.o", obj_dir, flat);
}

// host_os is the system this program runs on, as Target.os names it.
static const char *host_os(void)
{
#if defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// build_os is the system the build is for, as Target.os names it.
static const char *build_os(const Build *build)
{
    return build->target != NULL ? build->target->os : host_os();
}

// push_compile_command pushes everything but the input and output files.
static void push_compile_command(Command *command, const Build *build)
{
    push_compiler(command, build);
    if (build->dependency != NULL) {
        command_push_many(command, build->dependency->flags, build->dependency->flag_count);
        if (build->dependency->curl_config) {
            command_push(command, build->curl_config_flag);
            // curl's CMake defines _GNU_SOURCE on Linux: glibc declares accept4 and memrchr only with it.
            if (strcmp(build_os(build), "linux") == 0) {
                command_push(command, "-D_GNU_SOURCE");
            } else {
                command_push(command, "-idirafter");
                command_push(command, "src/net/curl/macos/sdk");
            }
        }
    } else {
        command_push_many(command, WARNING_FLAGS, sizeof WARNING_FLAGS / sizeof *WARNING_FLAGS);
        command_push_many(command, LIBMC_FLAGS, sizeof LIBMC_FLAGS / sizeof *LIBMC_FLAGS);
    }
    if (build->release) {
        command_push_many(command, RELEASE_FLAGS, sizeof RELEASE_FLAGS / sizeof *RELEASE_FLAGS);
    } else if (sanitized(build)) {
        command_push_many(command, DEBUG_FLAGS, sizeof DEBUG_FLAGS / sizeof *DEBUG_FLAGS);
    } else {
        command_push(command, "-g");
    }
}

static void command_text(const Command *command, char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    for (size_t i = 0; i < command->count && at < size; i++) {
        int n = snprintf(out + at, size - at, "%s%s", i == 0 ? "" : " ", command->items[i]);
        at += n > 0 ? (size_t)n : 0;
    }
}

static bool file_holds(const char *path, const char *text)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    size_t len = strlen(text);
    size_t at = 0;
    bool same = true;
    int c;
    while (same && (c = fgetc(file)) != EOF) {
        same = at < len && (char)c == text[at++];
    }
    fclose(file);
    return same && at == len;
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (file == NULL || fputs(text, file) < 0 || fclose(file) != 0) {
        fprintf(stderr, "build: cannot write %s\n", path);
        exit(1);
    }
}

// Dependencies are fetched, not committed. deps.lock pins each one by the
// content hash of its directory, and the build fetches deps/<name> when it is
// missing or differs. The hash is `modern-c hash`, computed with a copy of
// libmc's SHA-256, since this program builds libmc and cannot link it.

enum { SHA256_LEN = 32, SHA256_BLOCK_LEN = 64, SHA256_HEX_LEN = 2 * SHA256_LEN, LOCK_HASH_LEN = 72 };

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t buffer[SHA256_BLOCK_LEN];
    size_t buffered;
} Sha256;

static const uint32_t SHA256_ROUND_CONSTANTS[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_transform(Sha256 *hash, const uint8_t block[SHA256_BLOCK_LEN])
{
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 |
               block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = hash->state[0];
    uint32_t b = hash->state[1];
    uint32_t c = hash->state[2];
    uint32_t d = hash->state[3];
    uint32_t e = hash->state[4];
    uint32_t f = hash->state[5];
    uint32_t g = hash->state[6];
    uint32_t h = hash->state[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choice + SHA256_ROUND_CONSTANTS[i] + w[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
    hash->state[4] += e;
    hash->state[5] += f;
    hash->state[6] += g;
    hash->state[7] += h;
}

static void sha256_init(Sha256 *hash)
{
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(hash->state, initial, sizeof initial);
    hash->length = 0;
    hash->buffered = 0;
}

static void sha256_update(Sha256 *hash, const void *data, size_t len)
{
    const uint8_t *bytes = data;
    hash->length += len;
    if (hash->buffered > 0) {
        size_t take = SHA256_BLOCK_LEN - hash->buffered < len ? SHA256_BLOCK_LEN - hash->buffered : len;
        memcpy(hash->buffer + hash->buffered, bytes, take);
        hash->buffered += take;
        bytes += take;
        len -= take;
        if (hash->buffered < SHA256_BLOCK_LEN) {
            return;
        }
        sha256_transform(hash, hash->buffer);
        hash->buffered = 0;
    }
    for (; len >= SHA256_BLOCK_LEN; bytes += SHA256_BLOCK_LEN, len -= SHA256_BLOCK_LEN) {
        sha256_transform(hash, bytes);
    }
    if (len > 0) {
        memcpy(hash->buffer, bytes, len);
    }
    hash->buffered = len;
}

static void sha256_final_hex(Sha256 *hash, char hex[SHA256_HEX_LEN + 1])
{
    uint64_t bits = hash->length * 8;
    uint8_t padding[SHA256_BLOCK_LEN + 8] = { 0x80 };
    size_t pad_len = hash->buffered < 56 ? 56 - hash->buffered : SHA256_BLOCK_LEN + 56 - hash->buffered;
    for (size_t i = 0; i < 8; i++) {
        padding[pad_len + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    sha256_update(hash, padding, pad_len + 8);
    for (size_t i = 0; i < 8; i++) {
        snprintf(hex + i * 8, 9, "%08x", hash->state[i]);
    }
}

static void hash_file(const char *path, char hex[SHA256_HEX_LEN + 1])
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "build: cannot read %s\n", path);
        exit(1);
    }
    Sha256 hash;
    sha256_init(&hash);
    static uint8_t chunk[1 << 16];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, file)) > 0) {
        sha256_update(&hash, chunk, n);
    }
    fclose(file);
    sha256_final_hex(&hash, hex);
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} PathList;

// list_tree appends the path of every file under root/relative, relative to
// root, skipping what `modern-c hash` skips: .DS_Store, and at the top .git
// and what a dependency's own build creates, deps/, out/ and nob.
static void list_tree(PathList *list, const char *root, const char *relative)
{
    static const char *const SKIPPED_AT_TOP[] = { ".git", "deps", "out", "nob" };
    char directory[PATH_MAX_LEN];
    snprintf(directory, sizeof directory, "%s/%s", root, relative);
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        bool skipped = strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, ".DS_Store") == 0;
        for (size_t i = 0; relative[0] == '\0' && i < sizeof SKIPPED_AT_TOP / sizeof *SKIPPED_AT_TOP; i++) {
            skipped = skipped || strcmp(name, SKIPPED_AT_TOP[i]) == 0;
        }
        if (skipped) {
            continue;
        }
        char child[PATH_MAX_LEN];
        snprintf(child, sizeof child, "%s%s%s", relative, relative[0] == '\0' ? "" : "/", name);
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof path, "%s/%s", root, child);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            list_tree(list, root, child);
            continue;
        }
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (list->count == list->capacity) {
            list->capacity = list->capacity == 0 ? 256 : list->capacity * 2;
            list->items = realloc(list->items, list->capacity * sizeof *list->items);
        }
        list->items[list->count] = strdup(child);
        if (list->items == NULL || list->items[list->count] == NULL) {
            fprintf(stderr, "build: out of memory\n");
            exit(1);
        }
        list->count++;
    }
    closedir(dir);
}

static int compare_paths(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// tree_hash computes the hash deps.lock records for root: SHA-256 over each
// file's relative path, a NUL, the hex SHA-256 of its content and a newline,
// in byte order of path. A missing root hashes as an empty one.
static void tree_hash(const char *root, char out[LOCK_HASH_LEN])
{
    PathList files = { 0 };
    list_tree(&files, root, "");
    qsort(files.items, files.count, sizeof *files.items, compare_paths);
    Sha256 hash;
    sha256_init(&hash);
    for (size_t i = 0; i < files.count; i++) {
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof path, "%s/%s", root, files.items[i]);
        char file_hex[SHA256_HEX_LEN + 1];
        hash_file(path, file_hex);
        sha256_update(&hash, files.items[i], strlen(files.items[i]) + 1);
        sha256_update(&hash, file_hex, SHA256_HEX_LEN);
        sha256_update(&hash, "\n", 1);
        free(files.items[i]);
    }
    free(files.items);
    char hex[SHA256_HEX_LEN + 1];
    sha256_final_hex(&hash, hex);
    snprintf(out, LOCK_HASH_LEN, "sha256:%s", hex);
}

typedef struct {
    char name[128];
    char version[128];
    char url[PATH_MAX_LEN];
    char hash[LOCK_HASH_LEN];
} LockedDependency;

static void remove_tree(const char *path)
{
    Command command = { 0 };
    command_push(&command, "rm");
    command_push(&command, "-rf");
    command_push(&command, path);
    run_or_die(&command);
}

// unpacked_root finds the one top-level directory an archive unpacked into
// staging: <name>/ in a release asset, <repository>-<commit>/ in GitHub's
// archive of a commit.
static void unpacked_root(const char *staging, const char *url, char out[PATH_MAX_LEN])
{
    DIR *dir = opendir(staging);
    size_t count = 0;
    struct dirent *entry;
    while (dir != NULL && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            snprintf(out, PATH_MAX_LEN, "%s/%s", staging, entry->d_name);
            count++;
        }
    }
    if (dir != NULL) {
        closedir(dir);
    }
    struct stat st;
    if (count != 1 || stat(out, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "build: %s must hold one top-level directory\n", url);
        exit(1);
    }
}

// fetch downloads the archive deps.lock points at and moves its top-level
// directory to deps/<name> once its content matches the lock.
static void fetch(const LockedDependency *dependency)
{
    char staging[PATH_MAX_LEN];
    char archive[PATH_MAX_LEN];
    char unpacked[PATH_MAX_LEN];
    char directory[PATH_MAX_LEN];
    snprintf(staging, sizeof staging, "out/fetch/%s", dependency->name);
    snprintf(archive, sizeof archive, "%s.tar.gz", staging);
    snprintf(directory, sizeof directory, "deps/%s", dependency->name);
    printf("build: fetching %s %s\n", dependency->name, dependency->version);
    remove_tree(staging);
    mkdir_p(staging);

    Command download = { 0 };
    command_push(&download, "curl");
    command_push(&download, "-fsSL");
    command_push(&download, "-o");
    command_push(&download, archive);
    command_push(&download, dependency->url);
    if (command_run(&download) != 0) {
        fprintf(stderr, "build: cannot fetch %s from %s; fetching dependencies needs the network and curl\n",
                dependency->name, dependency->url);
        exit(1);
    }
    Command unpack = { 0 };
    command_push(&unpack, "tar");
    command_push(&unpack, "-xzf");
    command_push(&unpack, archive);
    command_push(&unpack, "-C");
    command_push(&unpack, staging);
    run_or_die(&unpack);
    unpacked_root(staging, dependency->url, unpacked);

    char actual[LOCK_HASH_LEN];
    tree_hash(unpacked, actual);
    if (strcmp(actual, dependency->hash) != 0) {
        fprintf(stderr, "build: %s holds %s, deps.lock says %s\n", dependency->url, actual, dependency->hash);
        exit(1);
    }
    mkdir_p("deps");
    remove_tree(directory);
    if (rename(unpacked, directory) != 0) {
        fprintf(stderr, "build: cannot move %s to %s: %s\n", unpacked, directory, strerror(errno));
        exit(1);
    }
    remove_tree(staging);
    remove(archive);
}

// fetch_dependencies fetches each dependency whose directory is missing or
// does not match deps.lock. Lines with a fifth field, prebuilt archives, are
// not fetched here.
static void fetch_dependencies(void)
{
    FILE *lock = fopen("deps.lock", "r");
    if (lock == NULL) {
        fprintf(stderr, "build: cannot read deps.lock\n");
        exit(1);
    }
    char line[2 * PATH_MAX_LEN];
    while (fgets(line, sizeof line, lock) != NULL) {
        LockedDependency dependency;
        char fifth[2];
        if (line[0] == '#' || sscanf(line, "%127s %127s %1023s %71s %1s", dependency.name, dependency.version,
                                     dependency.url, dependency.hash, fifth) != 4) {
            continue;
        }
        char directory[PATH_MAX_LEN];
        snprintf(directory, sizeof directory, "deps/%s", dependency.name);
        char actual[LOCK_HASH_LEN];
        tree_hash(directory, actual);
        if (strcmp(actual, dependency.hash) != 0) {
            fetch(&dependency);
        }
    }
    fclose(lock);
}

// fingerprint describes what shapes objects besides their sources and headers:
// the compiler's version and the compile command.
static void fingerprint(const Build *build, char *out, size_t size)
{
    // The version does not depend on the target, and zig cc rejects the glibc version in one with --version.
    Command command = { 0 };
    bool zig = build->target != NULL && build->target->zig_target != NULL;
    command_push(&command, zig ? "zig" : host_compiler());
    if (zig) {
        command_push(&command, "cc");
    }
    command_push(&command, "--version");
    char version_command[FINGERPRINT_LEN];
    command_text(&command, version_command, sizeof version_command);
    char version[FINGERPRINT_LEN / 2] = "";
    FILE *pipe = popen(version_command, "r");
    if (pipe != NULL) {
        version[fread(version, 1, sizeof version - 1, pipe)] = '\0';
        pclose(pipe);
    }
    Command compile_command = { 0 };
    push_compile_command(&compile_command, build);
    char flags[FINGERPRINT_LEN / 2];
    command_text(&compile_command, flags, sizeof flags);
    snprintf(out, size, "%s%s\n", version, flags);
}

// discard_stale_objects removes the objects when the fingerprint changed since they were built.
static void discard_stale_objects(const Build *build)
{
    char current[FINGERPRINT_LEN];
    fingerprint(build, current, sizeof current);
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof path, "%s/fingerprint", build->out_dir);
    if (file_holds(path, current)) {
        return;
    }
    Command command = { 0 };
    command_push(&command, "rm");
    command_push(&command, "-rf");
    command_push(&command, build->obj_dir);
    run_or_die(&command);
    mkdir_p(build->obj_dir);
    write_text(path, current);
}

// Jobs holds compile commands and runs them in parallel, at most MAX_JOBS at
// a time, stopping the build at the first failure once the running ones end.
typedef struct {
    Command commands[MAX_FILES];
    // sources holds each command's input, since the lists it came from do not outlive the queueing.
    char sources[MAX_FILES][PATH_MAX_LEN];
    size_t count;
} Jobs;

static Jobs *jobs_create(void)
{
    Jobs *jobs = calloc(1, sizeof *jobs);
    if (jobs == NULL) {
        fprintf(stderr, "build: out of memory\n");
        exit(1);
    }
    return jobs;
}

static pid_t command_spawn(const Command *command)
{
    for (size_t i = 0; i < command->count; i++) {
        printf("%s%s", i == 0 ? "" : " ", command->items[i]);
    }
    printf("\n");
    fflush(stdout);
    pid_t pid;
    int status = posix_spawnp(&pid, command->items[0], NULL, NULL, (char *const *)command->items, environ);
    if (status != 0) {
        fprintf(stderr, "build: cannot run %s: %s\n", command->items[0], strerror(status));
        exit(1);
    }
    return pid;
}

static void jobs_run(Jobs *jobs)
{
    size_t next = 0;
    size_t running = 0;
    int failed = 0;
    while (next < jobs->count || running > 0) {
        while (failed == 0 && next < jobs->count && running < MAX_JOBS) {
            command_spawn(&jobs->commands[next++]);
            running++;
        }
        if (running == 0) {
            break;
        }
        int wait_status = 0;
        if (wait(&wait_status) < 0) {
            break;
        }
        running--;
        if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
            failed = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 1;
        }
    }
    jobs->count = 0;
    if (failed != 0) {
        fprintf(stderr, "build: compile failed with status %d\n", failed);
        exit(failed);
    }
}

// compile queues a compile of source into object unless object is current.
static void compile(const Build *build, const char *source, const char *object, Jobs *jobs)
{
    if (mtime_of(object) > mtime_of(source) && mtime_of(object) > build->headers_mtime) {
        return;
    }
    if (jobs->count == MAX_FILES) {
        fprintf(stderr, "build: too many sources\n");
        exit(2);
    }
    char *queued_source = jobs->sources[jobs->count];
    snprintf(queued_source, PATH_MAX_LEN, "%s", source);
    Command *command = &jobs->commands[jobs->count++];
    command->count = 0;
    push_compile_command(command, build);
    command_push(command, "-c");
    command_push(command, queued_source);
    command_push(command, "-o");
    command_push(command, object);
}

static void compile_directory(const Build *build, const char *directory, FileList *objects, Jobs *jobs)
{
    FileList *sources = calloc(1, sizeof *sources);
    list_files(sources, directory, ".c");
    for (size_t i = 0; i < sources->count; i++) {
        if (objects->count == MAX_FILES) {
            fprintf(stderr, "build: too many objects\n");
            exit(2);
        }
        char *object = objects->items[objects->count++];
        object_path(object, build->obj_dir, sources->items[i]);
        compile(build, sources->items[i], object, jobs);
    }
    free(sources);
}

static time_t newest_of(const FileList *files)
{
    time_t newest = 0;
    for (size_t i = 0; i < files->count; i++) {
        time_t t = mtime_of(files->items[i]);
        newest = t > newest ? t : newest;
    }
    return newest;
}

static void build_library(const Build *build, FileList *objects, char *library_out)
{
    mkdir_p(build->obj_dir);
    discard_stale_objects(build);
    Jobs *jobs = jobs_create();
    for (size_t i = 0; i < sizeof AREAS / sizeof *AREAS; i++) {
        char directory[PATH_MAX_LEN];
        snprintf(directory, sizeof directory, "src/%s", AREAS[i]);
        compile_directory(build, directory, objects, jobs);
    }
    // Each dependency's Build outlives the queueing, since queued commands point into it.
    Build dependencies[sizeof DEPENDENCIES / sizeof *DEPENDENCIES];
    for (size_t i = 0; i < sizeof DEPENDENCIES / sizeof *DEPENDENCIES; i++) {
        dependencies[i] = *build;
        dependencies[i].dependency = &DEPENDENCIES[i];
        for (size_t d = 0; d < DEPENDENCIES[i].directory_count; d++) {
            compile_directory(&dependencies[i], DEPENDENCIES[i].directories[d], objects, jobs);
        }
    }
    jobs_run(jobs);
    free(jobs);
    snprintf(library_out, PATH_MAX_LEN, "%s/libmc.a", build->out_dir);
    // The member list catches a removed source, which leaves every object older than the archive.
    static char members[MAX_FILES * PATH_MAX_LEN];
    size_t at = 0;
    for (size_t i = 0; i < objects->count; i++) {
        at += (size_t)snprintf(members + at, sizeof members - at, "%s\n", objects->items[i]);
    }
    char members_path[PATH_MAX_LEN];
    snprintf(members_path, sizeof members_path, "%s/libmc.members", build->out_dir);
    if (mtime_of(library_out) > newest_of(objects) && file_holds(members_path, members)) {
        return;
    }
    remove(library_out);
    Command command = { 0 };
    if (build->target != NULL && build->target->zig_target != NULL) {
        command_push(&command, "zig");
    }
    command_push(&command, "ar");
    command_push(&command, "rcs");
    command_push(&command, library_out);
    for (size_t i = 0; i < objects->count; i++) {
        command_push(&command, objects->items[i]);
    }
    run_or_die(&command);
    write_text(members_path, members);
}

static int build_and_run_tests(const Build *build)
{
    FileList objects = { 0 };
    char library[PATH_MAX_LEN];
    build_library(build, &objects, library);
    Build tests = *build;
    snprintf(tests.obj_dir, sizeof tests.obj_dir, "%s/tests", build->obj_dir);
    mkdir_p(tests.obj_dir);
    FileList test_objects = { 0 };
    Jobs *jobs = jobs_create();
    compile_directory(&tests, "tests", &test_objects, jobs);
    jobs_run(jobs);
    free(jobs);

    char binary[PATH_MAX_LEN];
    snprintf(binary, sizeof binary, "%s/tests", build->out_dir);
    if (mtime_of(binary) <= newest_of(&test_objects) || mtime_of(binary) <= mtime_of(library)) {
        Command command = { 0 };
        push_compiler(&command, build);
        if (sanitized(build)) {
            command_push(&command, "-fsanitize=address,undefined");
        }
        for (size_t i = 0; i < test_objects.count; i++) {
            command_push(&command, test_objects.items[i]);
        }
        command_push(&command, library);
        command_push(&command, "-lpthread");
        command_push(&command, "-lm");
#ifdef __APPLE__
        // File watching uses CoreServices; curl asks SystemConfiguration for the proxies once, so that
        // NAT64 address synthesis works.
        static const char *const FRAMEWORKS[] = { "CoreServices", "SystemConfiguration", "CoreFoundation" };
        for (size_t i = 0; i < sizeof FRAMEWORKS / sizeof *FRAMEWORKS; i++) {
            command_push(&command, "-framework");
            command_push(&command, FRAMEWORKS[i]);
        }
#endif
        command_push(&command, "-o");
        command_push(&command, binary);
        run_or_die(&command);
    }
    Command run = { 0 };
    command_push(&run, binary);
    return command_run(&run);
}

static void configure(Build *build, bool release, const Target *target)
{
    build->release = release;
    build->dependency = NULL;
    build->target = target;
    snprintf(build->curl_config_flag, sizeof build->curl_config_flag, "-Isrc/net/curl/%s", build_os(build));
    build->headers_mtime = newest_header_mtime();
    snprintf(build->out_dir, sizeof build->out_dir, "out/%s%s%s", target != NULL ? target->name : "",
             target != NULL ? "/" : "", release ? "release" : "debug");
    snprintf(build->obj_dir, sizeof build->obj_dir, "%s/obj", build->out_dir);
}

static int run_cross(void)
{
    for (size_t i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++) {
        Build build;
        configure(&build, true, &TARGETS[i]);
        FileList objects = { 0 };
        char library[PATH_MAX_LEN];
        build_library(&build, &objects, library);
        printf("built %s\n", library);
    }
    return 0;
}

static void rebuild_self_if_needed(char **argv)
{
    const char *source = "tools/build.c";
    if (mtime_of(source) <= mtime_of(argv[0])) {
        return;
    }
    printf("build: rebuilding self\n");
    Command command = { 0 };
    command_push(&command, "cc");
    command_push(&command, source);
    command_push(&command, "-o");
    command_push(&command, argv[0]);
    run_or_die(&command);
    execv(argv[0], argv);
    fprintf(stderr, "build: exec failed: %s\n", strerror(errno));
    exit(1);
}

static int run_check(void)
{
    const char *home = getenv("HOME");
    char checker[PATH_MAX_LEN];
    snprintf(checker, sizeof checker, "%s/.agents/tools/modern-c/modern-c", home == NULL ? "" : home);
    Command command = { 0 };
    command_push(&command, "ruby");
    command_push(&command, checker);
    command_push(&command, "check");
    command_push(&command, ".");
    return command_run(&command);
}

int main(int argc, char **argv)
{
    rebuild_self_if_needed(argv);
    const char *mode = argc > 1 ? argv[1] : "build";

    if (strcmp(mode, "clean") == 0) {
        Command command = { 0 };
        command_push(&command, "rm");
        command_push(&command, "-rf");
        command_push(&command, "out");
        return command_run(&command);
    }
    if (strcmp(mode, "hash") == 0 && argc == 3) {
        char hash[LOCK_HASH_LEN];
        tree_hash(argv[2], hash);
        printf("%s\n", hash);
        return 0;
    }

    fetch_dependencies();
    if (strcmp(mode, "cross") == 0) {
        return run_cross();
    }

    Build build;
    configure(&build, strcmp(mode, "release") == 0, NULL);
    if (strcmp(mode, "build") == 0 || strcmp(mode, "release") == 0) {
        FileList objects = { 0 };
        char library[PATH_MAX_LEN];
        build_library(&build, &objects, library);
        printf("built %s\n", library);
        return 0;
    }
    if (strcmp(mode, "test") == 0) {
        return build_and_run_tests(&build);
    }
    if (strcmp(mode, "check") == 0) {
        int status = run_check();
        return status != 0 ? status : build_and_run_tests(&build);
    }

    fprintf(stderr, "build: unknown mode '%s'\nmodes: build, release, test, check, cross, clean, hash <dir>\n", mode);
    return 2;
}
