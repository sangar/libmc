# libmc

A small foundation library for C: arenas, length-carrying strings, one error model, containers, text utilities, JSON and YAML, structured logging and a portable platform layer. It builds to a single static library, `libmc.a`, with a plain C ABI, so it can be linked from C, C++ and any language with a C foreign function interface.

## Features

- **Arena allocation.** Allocations are freed together, and scratch work is undone with mark and release.
- **Strings as views.** A `String` is a pointer and a length, and every function that allocates one takes an `Arena`.
- **One error model.** Fallible functions return an `Error` code and can fill in an optional message.
- **Nothing to install** beyond libc, libm and pthreads, and no global state for the caller to manage. The one vendored library, [cyaml](https://github.com/andrewmd5/cyaml) for YAML, is pinned in `deps.lock` and builds into `libmc.a`.
- **A platform layer** for the clock, threads, files, directories, processes, environment, signals, sockets, file watching, login services and the terminal.
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

Cross builds of the macOS library need no macOS SDK: `src/platform/watch_darwin.c` declares the few FSEvents functions it calls, and where the SDK is present the compiler checks those declarations against its headers.

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

Host builds use `$CC`, or `cc` when it is unset: `CC=gcc-14 ./nob test`. Each build records the compiler version and flags in `out/<build>/fingerprint` and recompiles everything when they change.

CI runs `./nob test` and `./nob release` with clang 18 and gcc 14 on Linux x86_64 and aarch64 and with clang on macOS aarch64, and `./nob cross` with the pinned zig; see `.github/workflows/ci.yml`.

`./nob check` needs Ruby and the `modern-c` checker at `~/.agents/tools/modern-c/modern-c`. Neither ships with this repository, so the check runs only where they are installed.

## Usage

Link `libmc.a` with `-lpthread -lm`, and on macOS `-framework CoreServices` for file watching, and add `include/` to the include path. Alternatively, vendor the repository and compile `src/*/*.c` together with your own sources, plus `deps/cyaml/*.c` as C11 with `deps/cyaml` on the include path. cyaml is upstream's code, so it builds without libmc's warning flags. Headers are included by area and module:

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
- **No ABI stability promise.** Rebuild the library together with the code that uses it.

## Conventions

- **Strings.** A `String` is a view and owns nothing. Functions that take an `Arena` return NUL-terminated strings allocated in it.
- **Memory.** Arena memory comes back zeroed. Use `arena_mark` and `arena_release` for temporary allocations. Only `src/core/arena.c` calls `malloc`, apart from cyaml, whose allocations stay inside `src/encoding/yaml.c`.
- **Errors.** A function that can fail for reasons outside the caller's control, such as I/O, the network or a child process, returns `Error` and is marked `[[nodiscard]]`. Results come back through out-parameters. The last parameter is an optional `Err *` that receives a human-readable message; pass `nullptr` to ignore it. Parsers and lookups, for which "no" is an ordinary answer, return `bool` instead. Running out of memory, an allocation size that overflows, or a failed mutex or condition variable call aborts.
- **Ownership is in the signature.** A function that allocates takes an `Arena *`. A function without one returns a view or nothing.
- **No global state.** The exceptions are `env_set`, which changes the process environment, and the C locale that `float_parse` and `float_format` create once and share. `process_run` blocks `SIGPIPE` only in the calling thread while it feeds the child, and discards any `SIGPIPE` that leaves pending.
- **Bridging stays in the project.** libmc is plain C with a plain C ABI and knows nothing about who calls it. It takes and returns its own types: `String`, `Arena *`, `Error`, `Err *`, fixed-layout structs and opaque handles. A project that uses it from Swift, C++, Rust, Zig, Go or Objective-C does the translation on its own side, in one bridging module: a Swift bridging header and a `String` to `Swift.String` layer, an `extern "C"` wrapper with RAII handles in C++, cgo conversions in Go. Nothing for that goes into libmc: no `#ifdef __cplusplus` or `extern "C"` blocks in its headers, the caller adds them; no Foundation, CoreFoundation, `NSString` or `CFString`; no framework headers; no callback shapes designed for one host language; no OS-specific API outside `src/platform/`. A libmc PR that exists only to make one language's bridge easier is declined. The bridge changes instead.

## Modules

Headers live in `include/mc/<area>/` and their sources in `src/<area>/`.

| Area | Header | Contents |
|---|---|---|
| core | `mc/core/base.h` | `countof`, `unused`, `min_size`/`max_size`, nanosecond constants |
| | `mc/core/error.h` | `Error`, `Err`, `err_set`, `err_wrap`, `error_name` |
| | `mc/core/arena.h` | Block arena, mark/release, `arena_grow` for dynamic arrays |
| text | `mc/text/str.h` | `String`, `StringList`, `StringBuilder` |
| | `mc/text/utf8.h` | UTF-8 decode, encode, validate and count |
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
| platform | `mc/platform/platform.h` | Clock, locale-independent floats, threads, files and mappings, directories with metadata, processes, environment, signals, TCP and Unix sockets |
| | `mc/platform/watch.h` | Recursive file watching on FSEvents and inotify, in settled batches of paths |
| | `mc/platform/service.h` | Per-user login services: launchd agents and systemd user units |
| | `mc/platform/terminal.h` | The controlling terminal in raw mode, its size, and waits that wake on input, resizes or another thread |
| concurrency | `mc/concurrency/cancel.h` | Cancellation token with waits and sleeps that wake on cancel |
| | `mc/concurrency/debounce.h` | Per-key debouncing: a callback once a key has been quiet for a delay |
| | `mc/concurrency/queue.h` | Unbounded blocking FIFO of pointers, with close and timeouts |
| | `mc/concurrency/threadpool.h` | Thread pool, chunked parallel loops, and one thread per job |
| encoding | `mc/encoding/node.h` | Document tree of scalars, sequences and mappings shared by the parsers |
| | `mc/encoding/json.h` | Strict JSON parsing with line and column errors, encoding and quoting |
| | `mc/encoding/yaml.h` | YAML 1.2 parsing with core schema typing and shared aliases, on cyaml |
| log | `mc/log/log.h` | Structured logging like Go's slog, as text or JSON lines |

The header comments document each function's contract.

## License

MIT. See [LICENSE](LICENSE).
