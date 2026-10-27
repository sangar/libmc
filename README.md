# libmc

A small foundation library for C: arenas, length-carrying strings, one error model, containers, text utilities, JSON and YAML, structured logging and a portable platform layer. It builds to a single static library, `libmc.a`, with a plain C ABI, so it can be linked from C, C++ and any language with a C foreign function interface.

## Features

- **Arena allocation.** Allocations are freed together, and scratch work is undone with mark and release.
- **Strings as views.** A `String` is a pointer and a length, and every function that allocates one takes an `Arena`.
- **One error model.** Fallible functions return an `Error` code and can fill in an optional message.
- **Nothing to install** beyond libc, libm and pthreads, and no global state for the caller to manage. Its libraries, [yyjson](https://github.com/ibireme/yyjson) for JSON, [cyaml](https://github.com/andrewmd5/cyaml) for YAML, [utf8proc](https://github.com/JuliaStrings/utf8proc) for Unicode, [md4c](https://github.com/mity/md4c) for Markdown, [Expat](https://libexpat.github.io) for XML, [llhttp](https://github.com/nodejs/llhttp) for the HTTP server and [curl](https://curl.se) on [mbedTLS](https://github.com/Mbed-TLS/mbedtls), [nghttp2](https://nghttp2.org) and [zlib](https://zlib.net) for HTTP, mail and FTP, with [Mozilla's trusted roots](https://curl.se/docs/caextract.html) for systems that have none, are pinned in `deps.lock`, fetched by the build and built into `libmc.a`.
- **Terminal output** with colours that fall back to what the terminal shows, boxes and joins as in Lip Gloss, spinners, springs, progress bars and lists redrawn in place, and a full-screen cell grid with graphs.
- **A platform layer** for the clock, threads, files, directories, processes, environment, signals, sockets, file watching, login services, the terminal, pseudo-terminals and system statistics.
- **JSON and YAML** parsing into one document tree, and JSON encoding back, with numbers read the same whatever locale the program set.

## Requirements

| | |
|---|---|
| Language | C23 |
| Compilers | clang 18 or later (primary), gcc 14 or later |
| Platforms | macOS aarch64, Linux x86_64, Linux aarch64 |
| Cross builds | [zig](https://ziglang.org) 0.15.2, pinned in `mise.toml` |
| Profile | Modern C Level 2 |

Windows is not supported until `src/platform/` has a Windows implementation.

Cross builds of the macOS library need no macOS SDK: `src/platform/watch_darwin.c` declares the few FSEvents functions it calls, and where the SDK is present the compiler checks those declarations against its headers. `src/net/curl/macos/sdk/` does the same for the two SystemConfiguration calls curl makes; the build searches it after the SDK.

## Building

The build program is written in C and rebuilds itself when it changes. Bootstrap it once:

```sh
cc tools/build.c -o nob
```

Then:

| Command | Result |
|---|---|
| `./nob` | Debug library with AddressSanitizer and UBSan: `out/debug/libmc.a` |
| `./nob release` | Optimised library: `out/release/libmc.a` |
| `./nob test` | Builds and runs the tests under the sanitizers |
| `./nob check` | Runs the `modern-c` contract checker, then the tests (see below) |
| `./nob cross` | Release library for every platform: `out/<platform>/release/libmc.a` |
| `./nob clean` | Removes `out/` |
| `./nob hash <dir>` | Prints the `deps.lock` hash of a directory |

The dependencies are not committed. Every build but `clean` checks `deps/<name>` against the hash in `deps.lock` and, when it is missing or differs, fetches it with `curl` from this repository's release `deps-<name>-<version>`, so the first build needs the network. To update one, put the new source in `deps/<name>`, trimmed to what the build compiles plus its license, update its line with the version, the new release URL and `./nob hash deps/<name>`, and publish it with `tools/publish-dep.sh`. `tools/cacert.sh` writes a newer extract of Mozilla's roots into `deps/cacert`.

Host builds use `$CC`, or `cc` when it is unset: `CC=gcc-14 ./nob test`. Each build records the compiler version and flags in `out/<build>/fingerprint` and recompiles everything when they change.

CI runs `./nob test` and `./nob release` with clang 18 and gcc 14 on Linux x86_64 and aarch64 and with clang on macOS aarch64, and `./nob cross` with the pinned zig; see `.github/workflows/ci.yml`.

`./nob check` needs Ruby and the `modern-c` checker at `~/.agents/tools/modern-c/modern-c`. Neither ships with this repository, so the check runs only where they are installed.

## Usage

Link `libmc.a` with `-lpthread -lm`, and on macOS `-framework CoreServices` for file watching and `-framework SystemConfiguration -framework CoreFoundation` for HTTP, mail and FTP, and add `include/` to the include path. To build it with your project, pin this repository as a dependency and run `./nob release` in it, which fetches libmc's own dependencies. Headers are included by area and module:

```c
#include <stdio.h>

#include "mc/platform/platform.h"
#include "mc/text/str.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    Arena *arena = arena_create(0);
    Err err = { 0 };
    String contents;
    if (file_read_all(arena, S(argv[1]), &contents, &err) != ERR_OK) {
        fprintf(stderr, "%s\n", err.msg);
        arena_destroy(arena);
        return 1;
    }
    StringList lines = strlist_from_lines(arena, contents);
    printf("%zu lines\n", lines.count);
    if (lines.count > 0) {
        printf("first: %.*s\n", (int)lines.items[0].len, lines.items[0].data);
    }
    arena_destroy(arena);
    return 0;
}
```

```sh
cc -std=c23 -Iinclude main.c out/release/libmc.a -lpthread -lm -o example
```

### From other languages

The library exports plain C functions and fixed-layout structs, so any language that can link a static C library can call it. Examples include C++, Rust, Zig, Go (through cgo) and Swift. Keep the following in mind:

- **C++.** Include the headers inside an `extern "C" { ... }` block.
- **Structs passed by value.** `String` is `{ const char *data; size_t len; }` and is passed by value. `Err` is a 512-byte message buffer that the caller provides. `Mutex`, `Cond` and `Thread` are opaque, fixed-size storage. `Cancel` holds a C11 `atomic_bool`; treat it as opaque and touch it only through `cancel_*`.
- **Header-only helpers.** The definitions in `mc/core/base.h` (`countof`, `min_size`, `max_size` and the `NS_PER_*` constants) are macros and inline functions. They are not exported symbols.
- **Out of memory aborts.** When an arena cannot get memory from the system, the process aborts. No error is returned.
- **Arenas are single-threaded.** Give each thread its own arena.
- **Unprefixed names.** Symbols such as `S`, `str_*` and `file_*` have no library prefix and can clash with other C libraries in the same binary.
- **Vendored symbols.** `libmc.a` also contains yyjson's `yyjson_*`, cyaml's `cyaml_*` and utf8proc's `utf8proc_*` symbols. A project that uses yyjson directly includes `deps/yyjson/yyjson.h` from libmc instead of linking a second copy.
- **No ABI stability promise.** Rebuild the library together with the code that uses it.

## Conventions

- **Strings.** A `String` is a view and owns nothing. Functions that take an `Arena` return NUL-terminated strings allocated in it.
- **Memory.** Arena memory comes back zeroed. Use `arena_mark` and `arena_release` for temporary allocations. Only `src/core/arena.c` calls `malloc`, apart from cyaml, whose allocations stay inside `src/encoding/yaml.c`. `src/text/unicode.c` calls only the utf8proc functions that write into a buffer it passes, never the ones that allocate. yyjson allocates from a scratch arena that `json_parse` frees before it returns.
- **Errors.** A function that can fail for reasons outside the caller's control, such as I/O, the network or a child process, returns `Error` and is marked `[[nodiscard]]`. Results come back through out-parameters. The last parameter is an optional `Err *` that receives a human-readable message; pass `nullptr` to ignore it. Parsers and lookups, for which "no" is an ordinary answer, return `bool` instead. Running out of memory, an allocation size that overflows, or a failed mutex or condition variable call aborts.
- **Assertions stay on.** A broken invariant, such as an index out of range, is an `assert`, and `./nob release` keeps it: the build never defines `NDEBUG`.
- **Ownership is in the signature.** A function that allocates takes an `Arena *`. A function without one returns a view or nothing.
- **No global state.** The exceptions are `env_set`, which changes the process environment, and the C locale that `float_parse` and `float_format` create once and share. `process_run` blocks `SIGPIPE` only in the calling thread while it feeds the child, and discards any `SIGPIPE` that leaves pending.
- **Bundled libraries.** A dependency is bundled when it serves a capability that libmc exposes through its own types, and never on its own terms. yyjson and cyaml both feed one `Node` tree, so JSON and YAML stay in one library and a program pays for only the parser it calls: `libmc.a` is a static archive, each parser is its own translation unit, and the linker pulls in only what is referenced. Splitting the library would buy nothing.
- **Bridging stays in the project.** libmc is plain C with a plain C ABI and knows nothing about who calls it. It takes and returns its own types: `String`, `Arena *`, `Error`, `Err *`, fixed-layout structs and opaque handles. A project that uses it from Swift, C++, Rust, Zig, Go or Objective-C does the translation on its own side, in one bridging module: a Swift bridging header and a `String` to `Swift.String` layer, an `extern "C"` wrapper with RAII handles in C++, cgo conversions in Go. Nothing for that goes into libmc: no `#ifdef __cplusplus` or `extern "C"` blocks in its headers, the caller adds them; no Foundation, CoreFoundation, `NSString` or `CFString`; no framework headers; no callback shapes designed for one host language; no OS-specific API outside `src/platform/`. A libmc PR that exists only to make one language's bridge easier is declined. The bridge changes instead.

## Modules

Headers live in `include/mc/<area>/` and their sources in `src/<area>/`.

| Area | Header | Contents |
|---|---|---|
| core | `mc/core/base.h` | `countof`, `unused`, `min_size`/`max_size`, nanosecond constants |
| | `mc/core/error.h` | `Error`, `Err`, `err_set`, `err_wrap`, `error_name` |
| | `mc/core/arena.h` | Block arena, mark/release, `arena_grow` for dynamic arrays |
| text | `mc/text/str.h` | `String`, `StringList`, `StringBuilder` |
| | `mc/text/utf8.h` | UTF-8 decode, encode, validate, count and terminal width |
| | `mc/text/unicode.h` | NFC normalization, Unicode upper and lower case, case-insensitive equality, Danish and Norwegian sort order, on utf8proc |
| | `mc/text/glob.h` | Glob matching with `**` and character classes |
| | `mc/text/path.h` | Lexical path handling: join, clean, base, dir, ext, relative |
| | `mc/text/fmt.h` | Go-style durations, RFC 3339 timestamps, thousands separators, byte sizes |
| | `mc/text/table.h` | Aligned columns like Go's tabwriter |
| container | `mc/container/strmap.h` | String-keyed open-addressing hash map with removal |
| | `mc/container/idtable.h` | Hash set of ids whose keys live elsewhere, four bytes a slot |
| | `mc/container/hash.h` | FNV-1a hashing |
| | `mc/container/sort.h` | Stable merge sort and top-k selection with a comparator context |
| crypto | `mc/crypto/sha256.h` | SHA-256, HMAC-SHA256, hex encoding |
| | `mc/crypto/sigv4.h` | AWS Signature Version 4 request signing |
| platform | `mc/platform/platform.h` | Clock, locale-independent floats, threads, files and mappings, reading a descriptor to its end, directories with metadata, advisory file locks, a search for a file up through parent directories, a private per-user runtime directory, processes (also detached into a session of their own), environment, signals and a thread that watches for them, TCP and Unix sockets, non-blocking accepts, sends and receives, and socket pairs for driving a server in-process |
| | `mc/platform/frame.h` | Length-prefixed frames of a header and a body on a stream socket |
| | `mc/platform/poll.h` | An event loop's wait on many sockets, on kqueue and epoll, with a wake from other threads; the `net_try_*` calls move bytes without waiting |
| | `mc/platform/watch.h` | Recursive file watching on FSEvents and inotify, in settled batches of paths |
| | `mc/platform/service.h` | Per-user login services: launchd agents and systemd user units |
| | `mc/platform/terminal.h` | The controlling terminal in raw mode, its size and the width of any terminal descriptor, and waits that wake on input, resizes or another thread |
| | `mc/platform/pty.h` | Programs run on a pseudo-terminal of their own: output, typed input, resizing, signals to the whole process group |
| | `mc/platform/sysinfo.h` | CPU ticks per core, memory, swap, network bytes, load and uptime, and the CPU time and memory of a process group |
| concurrency | `mc/concurrency/cancel.h` | Cancellation token with waits and sleeps that wake on cancel |
| | `mc/concurrency/debounce.h` | Per-key debouncing: a callback once a key has been quiet for a delay |
| | `mc/concurrency/outbox.h` | Bytes waiting for a writer thread, with a limit that drops a peer that stopped reading |
| | `mc/concurrency/queue.h` | Unbounded blocking FIFO of pointers, with close and timeouts |
| | `mc/concurrency/threadpool.h` | Thread pool, chunked parallel loops, and one thread per job |
| | `mc/concurrency/waker.h` | Waking a thread that waits for something to do, with a timeout |
| encoding | `mc/encoding/node.h` | Document tree of scalars, sequences and mappings shared by the parsers |
| | `mc/encoding/json.h` | Strict JSON parsing with line and column errors, encoding and quoting, on yyjson |
| | `mc/encoding/yaml.h` | YAML 1.2 parsing with core schema typing and shared aliases, on cyaml |
| | `mc/encoding/base64.h` | RFC 4648 Base64, standard with padding or URL-safe without, decoding only the canonical encoding |
| | `mc/encoding/dotenv.h` | `.env` files into `NAME=value` entries, with quotes and comments and no expansion |
| | `mc/encoding/markdown.h` | CommonMark with GitHub's tables, task lists, strikethrough and autolinks to HTML, on md4c; raw HTML and script links shown as text unless allowed |
| | `mc/encoding/xml.h` | XML 1.0 as start tags, end tags and text, on Expat, with entity expansion bounded and external entities never loaded |
| | `mc/encoding/csv.h` | RFC 4180 records read one at a time: quoted fields with line breaks and `""`, CRLF or LF, a byte order mark; fields are views into the text |
| net | `mc/net/http.h` | HTTPS requests on libcurl: HTTP/2 where the server offers it, gzip and deflate decoded, allowed hosts checked on every redirect, limits on time and size, bodies streamed from and to files, cancellation, Mozilla's trusted roots where the system has none |
| | `mc/net/smtp.h` | Mail over SMTPS or STARTTLS: from, to, cc and bcc, UTF-8 subject and names, text with an HTML alternative, attachments |
| | `mc/net/http_server.h` | An HTTP/1.1 server for web apps on llhttp: event loops own the connections, worker threads run the handler; requests refused unless unambiguous, limits on size, connections and time, pipelining, `100-continue`, a graceful stop, and the handler run without a socket for tests |
| | `mc/net/s3.h` | S3 and the stores that speak its API: objects in memory or files, multipart uploads, metadata, paged listing, retries with backoff, credentials found as the AWS tools find them; thread-safe, with pooled connections |
| | `mc/net/ftp.h` | Upload, download and list files over FTPS, streaming to and from local files |
| log | `mc/log/log.h` | Structured logging like Go's slog, as text or JSON lines |
| | `mc/log/logfile.h` | Lines appended to a file after their time, rotated at a size limit |
| term | `mc/term/color.h` | RGB, three-stop gradients, the nearest of the 256 or 16 xterm colours, `#rrggbb` and xterm numbers |
| | `mc/term/style.h` | Colour depth from the terminal and `NO_COLOR`, `FORCE_COLOR`, `COLORTERM` and `TERM`; styled and gradient text that falls back to what the terminal shows |
| | `mc/term/ansi.h` | Width, truncation and stripping of text with escape sequences in it |
| | `mc/term/live.h` | A region redrawn in place below the prompt: only changed lines, cut to the width, one synchronized update a frame |
| | `mc/term/layout.h` | Boxes with borders, padding, margins and alignment, and blocks joined side by side or above each other, as Lip Gloss does |
| | `mc/term/spinner.h`, `spring.h`, `progress.h` | Spinners from cli-spinners, damped springs as in Harmonica, progress bars in eighths of a column |
| | `mc/term/input.h` | Keys, mouse and bracketed paste decoded from a terminal's bytes |
| | `mc/term/list.h` | A list to pick from, scrolled to keep the cursor in view |
| | `mc/term/screen.h` | A full-screen grid of cells that writes only what changed, the way btop draws, and takes styled text too, wrapped as in a log view with the theme's 16 colours |
| | `mc/term/draw.h`, `history.h` | Panels with titles, meters, braille graphs and sparklines of a history of samples |
| | `mc/term/logbuf.h` | A program's terminal output as lines: colours kept, carriage returns rewriting the line, other control dropped |

The header comments document each function's contract.

## License

MIT. See [LICENSE](LICENSE).
