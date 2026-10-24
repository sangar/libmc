# libmc roadmap

libmc is the foundation library for C projects under the `modern-c` contract. A project pins it in `deps.lock`, runs the build and gets arenas, strings, one error model, containers, text, the common data formats and networking, with nothing to install but a C compiler, `curl` and `tar`. It grows toward a C counterpart to Apple's Foundation: what a program needs to call web APIs, send mail, move files and read the formats people exchange, in one library with one style.

It is not a port of Foundation's API, not a shared library with a stable ABI, and not for Windows until `src/platform/` has a Windows implementation. This file covers one repository, [sangar/libmc](https://github.com/sangar/libmc).

Checked on 2026-10-24

## Focus

libmc is in use by projects under the `modern-c` contract, such as dbox and bprocs, and is where they get shared code. JSON, YAML and CSV are in. What moves it forward now are the pieces those projects reach for next, in this order:

1. **The HTTP client:** HTTPS through curl on mbedTLS, HTTP/2, compressed responses and green builds on all three platforms, all on the `http-client` branch. See [HTTP client](#http-client).
2. **Markdown and XML:** rendering Markdown and reading XML, the two formats projects use most that libmc lacks. See [Markdown and XML](#markdown-and-xml).
3. **Mail and file transfer:** sending mail over SMTP and moving files over FTP, both through the curl libmc already pins. See [Mail and file transfer](#mail-and-file-transfer).
4. **SSH and SFTP:** waits for libssh2 to build with mbedTLS 4. See [SSH and SFTP](#ssh-and-sftp).

Small items, such as the unmerged commits on `terminal-width` and `unicode-text`, wait unless they block one of these.

## Principles

- **Nothing to install.** `deps.lock` pins every dependency by the hash of its content and the build fetches it from this repository's `deps-<name>-<version>` releases. A fresh clone needs a C compiler, `curl` and `tar`; see `modern-c` rule 10.
- **One branch per capability.** HTTP, Markdown, XML, SMTP, FTP and SSH each land through their own branch and pull request. Everything HTTP, HTTP/2 and response compression included, belongs to `http-client`.
- **curl before another library.** A protocol curl speaks is turned on in [`tools/curl-config.sh`](tools/curl-config.sh), not added as a new dependency. curl already carries the code for SMTP, FTP, WebSocket and URL parsing; it is compiled out today.
- **Safe defaults for input from the network.** HTTPS only unless a test asks for plain HTTP, allowed hosts checked on every redirect, and limits on time and size. Markdown from users renders without raw HTML.
- **A new dependency needs an approved decision report.** Each one is chosen with a `c-libraries` report, as `modern-c` rule 11 requires.

## Where we are

`master` is at `e8ddf93` from 23 October 2026, with the CSV reader (PR #17) and NFC, Unicode case and Norwegian sort order on utf8proc (PR #18) merged. The HTTP client is uncommitted work on `http-client`, on top of `master`.

| Branch | State | Last commit | Tests |
|---|---|---|---|
| `master` | Merged | `e8ddf93`, 23 Oct 2026, PR #18 | Not run for this file |
| `http-client` | Uncommitted, on top of `master` | Based on `e8ddf93` | `./nob check` (no findings, 684 checks), `./nob release` and `./nob cross` pass on macOS; `CC=gcc-14 ./nob test` passes in `ubuntu:24.04` on aarch64 |
| `unicode-text` | One commit not merged, pushed, no pull request | `08e7868`, "Allow the deliberate ASCII ignore-case calls" | Not run |
| `terminal-width` | One commit not merged, local only | `b30d282`, "Measure the terminal width of UTF-8 text" | Not run |

On 24 October 2026 dependencies stopped being committed. curl and mbedTLS would have added 13.7 MB of source, 5.5 MB in git, to every clone and to every project that pins libmc. `deps.lock` now pins each dependency by content hash, the build fetches it, and cyaml, yyjson, utf8proc, mbedTLS, curl, nghttp2 and zlib are published as `deps-*` releases.

## HTTP client

Now, on `http-client`. Projects call web APIs, such as S3 through libmc's SigV4 signing, and need one client that is safe by default. The bar is Go's `net/http`: its default transport uses HTTP/2 over HTTPS without being asked and decodes compressed responses itself, so a caller sees the original only by setting `Transport.DisableCompression`. A libmc caller gets the same without setting anything. It is done when a project calls an API through `http_send` on macOS and both Linux targets.

- [x] **Builds on top of `master`.** Rebased onto `e8ddf93` on 24 October 2026; utf8proc 2.12.0 is published as `deps-utf8proc-2.12.0` and fetched like the others.
- [x] **Builds and tests pass on Linux and in the macOS cross build.** Fixed on 24 October 2026: curl compiles with `_GNU_SOURCE` on Linux, as its CMake does, for `accept4` and `memrchr`; the macOS cross build finds the two SystemConfiguration calls curl makes in `src/net/curl/macos/sdk/`, searched after the SDK; and the compiler fingerprint no longer passes the glibc version to `zig cc --version`.
- [x] **HTTP/2 when the server offers it.** Done on 24 October 2026 with nghttp2 1.70.0, pinned as `deps-nghttp2-1.70.0`, and `USE_NGHTTP2` in both `curl_config.h` files. The test server offers `h2` over TLS and answers through nghttp2's server API. With one request at a time per `HttpClient`, the gain is header compression and fewer connections; requests on one connection at once wait for a concurrent API.
- [x] **Compressed responses decoded.** Done on 24 October 2026 with zlib 1.3.2, pinned as `deps-zlib-1.3.2`. The client asks for gzip and deflate, and `max_response_bytes` counts the decoded body: a 96-byte gzip response that decodes to 64 KiB fails at a 1 KiB limit.
- [ ] **Merged.**
      1. [x] **Documented,** on 24 October 2026: the README module table and `modern-c` rule 11's have `mc/net/http.h`, and both name the macOS frameworks HTTP needs, SystemConfiguration and CoreFoundation.
      2. [ ] **Merged through a pull request.**

## Markdown and XML

After the HTTP client, each on its own branch. JSON, YAML and CSV are in libmc; Markdown and XML are the formats projects use most that it lacks.

- [ ] **Render Markdown to HTML.** md4c (MIT) passes the CommonMark 0.31.2 compliance tests and supports GitHub's tables, task lists, strikethrough and autolinks, which is what people expect from Markdown since GitHub. `mc/encoding/markdown.h` on a branch `markdown`. Raw HTML in Markdown from users lets them inject scripts, so it is off by default, with `MD_FLAG_NOHTMLSPANS` and `MD_FLAG_NOHTMLBLOCKS`, and a caller turns it on for trusted text.
      1. [ ] A decision report, md4c against cmark-gfm, approved.
      2. [ ] Pin md4c, wrap it and test it with examples from the CommonMark spec.
- [ ] **Read XML.** Expat (MIT) is a stream-oriented parser that calls handlers as it meets elements. Go's `encoding/xml` offers both a token stream, `Decoder.Token`, and `Unmarshal` into structs. libmc starts with the stream, `mc/encoding/xml.h` on a branch `xml`, and adds a tree when a project needs one.
      1. [ ] A decision report, Expat against libxml2, approved.
      2. [ ] Pin Expat, with its configuration header written by hand like curl's.
      3. [ ] The event API, with Expat's protection against entity expansion ("billion laughs") left on, and tests.

## Mail and file transfer

After Markdown and XML, each on its own branch. Both are in the curl libmc already pins and are turned off in `tools/curl-config.sh`; each turns its protocol on and regenerates `src/net/curl/<os>/curl_config.h`.

- [ ] **Send mail with attachments.** Go's `net/smtp` is frozen and takes the message as raw bytes, leaving MIME to the caller. `mc/net/smtp.h`, on a branch `smtp`, builds the message with its attachments through curl's MIME API and sends it over SMTPS or STARTTLS only. It turns on SMTP and MIME.
- [ ] **Upload and download files over FTP.** `mc/net/ftp.h`, on a branch `ftp`: FTPS by default, and plain FTP only when the caller asks, as `allow_plain_http` does for HTTP. It turns on FTP.

## SSH and SFTP

After mail and file transfer, and blocked. curl speaks SSH and SFTP through libssh2 (BSD), which needs a crypto library underneath. [libssh2 1.11.1 does not build with mbedTLS 4.x](https://github.com/libssh2/libssh2/pull/2284), and the pull request that moves its mbedTLS backend to the PSA API is open and, by its author on 5 October 2026, needs a heavy rework or a fresh start.

- [ ] **Run commands and move files over SSH.** `mc/net/ssh.h` on a branch `ssh`, once a libssh2 release builds with mbedTLS 4. Check each libssh2 release until then.

## Feature ideas

### Worth doing soon

- **URL parsing:** `mc/net/url.h` on curl's URL API, which is already compiled in, to parse, build and normalise URLs.
- **WebSocket client:** curl supports it; it is one flag in `tools/curl-config.sh`.
- **Compression:** gzip and deflate for files and archives, on the zlib the HTTP client brings.

### Later

- **Brotli and zstd responses.** Each is another dependency, and gzip covers most APIs.
- **Property lists,** on libmc's Foundation direction, once XML is in.
- **An XML tree and writing XML,** when a project needs more than the stream.

### Not now

- **HTTP/3.** curl needs a QUIC stack and a TLS library that supports it. Revisit when an API a project calls needs it.
- **libssh2 on OpenSSL.** OpenSSL is a far bigger dependency than mbedTLS. Revisit if SSH is next and no libssh2 release builds with mbedTLS 4.

### Not doing

- **libssh instead of libssh2.** It is LGPL, which complicates linking it statically into `libmc.a`.
- **Committing dependency sources.** Decided against on 24 October 2026; see [Where we are](#where-we-are).

## Open questions

- **Trusted roots in minimal containers.** `ubuntu:24.04` has no CA bundle, so `http_client_create` fails with "no bundle of trusted roots at " and an empty path, and the fix today is `apt install ca-certificates`. Leaning: pin curl's copy of Mozilla's bundle, `cacert.pem`, use it only when the system has none, and make the error name the fix. To be decided before the HTTP client merges.
- **XML as events or as a tree.** Leaning: events first. Decided by the first project that reads XML.
