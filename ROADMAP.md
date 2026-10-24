# libmc roadmap

libmc is the foundation library for C projects under the `modern-c` contract. A project pins it in `deps.lock`, runs the build and gets arenas, strings, one error model, containers, text, the common data formats and networking, with nothing to install but a C compiler, `curl` and `tar`. It grows toward a C counterpart to Apple's Foundation: what a program needs to call web APIs, send mail, move files and read the formats people exchange, in one library with one style.

It is not a port of Foundation's API, not a shared library with a stable ABI, and not for Windows until `src/platform/` has a Windows implementation. This file covers one repository, [sangar/libmc](https://github.com/sangar/libmc).

Checked on 2026-10-24

## Focus

libmc is in use by projects under the `modern-c` contract, such as dbox and bprocs, and is where they get shared code. JSON, YAML, CSV and the HTTP client are in. What moves it forward now are the pieces those projects reach for next, in this order:

1. **Markdown and XML:** rendering Markdown and reading XML, the two formats projects use most that libmc lacks. See [Markdown and XML](#markdown-and-xml).
2. **Mail and file transfer:** sending mail over SMTP and moving files over FTP, both through the curl libmc already pins. See [Mail and file transfer](#mail-and-file-transfer).
3. **SSH and SFTP:** waits for libssh2 to build with mbedTLS 4. See [SSH and SFTP](#ssh-and-sftp).

Small items, such as the unmerged commits on `terminal-width` and `unicode-text`, wait unless they block one of these.

## Principles

- **Nothing to install.** `deps.lock` pins every dependency by the hash of its content and the build fetches it from this repository's `deps-<name>-<version>` releases. A fresh clone needs a C compiler, `curl` and `tar`; see `modern-c` rule 10.
- **One branch per capability.** HTTP, Markdown, XML, SMTP, FTP and SSH each land through their own branch and pull request. Everything HTTP, HTTP/2 and response compression included, belongs to `http-client`.
- **curl before another library.** A protocol curl speaks is turned on in [`tools/curl-config.sh`](tools/curl-config.sh), not added as a new dependency. curl already carries the code for SMTP, FTP, WebSocket and URL parsing; it is compiled out today.
- **Safe defaults for input from the network.** HTTPS only unless a test asks for plain HTTP, allowed hosts checked on every redirect, and limits on time and size. Markdown from users renders without raw HTML.
- **A new dependency needs an approved decision report.** Each one is chosen with a `c-libraries` report, as `modern-c` rule 11 requires.

## Where we are

`master` is at `9b80f1b` from 24 October 2026, with the HTTP client (PR #19) merged. Mozilla's trusted roots for systems without their own are on `trusted-roots`.

| Branch | State | Last commit | Tests |
|---|---|---|---|
| `master` | Merged | `9b80f1b`, 24 Oct 2026, PR #19 | GitHub CI passes on macOS, Linux x86_64 and aarch64 with clang 18 and gcc 14, and the cross builds |
| `trusted-roots` | Not merged | On top of `9b80f1b` | `./nob test` passes on macOS; in `ubuntu:24.04` without `ca-certificates`, `LIBMC_LIVE_TESTS=1` reaches three live hosts through the built-in roots |
| `unicode-text` | One commit not merged, pushed, no pull request | `08e7868`, "Allow the deliberate ASCII ignore-case calls" | Not run |
| `terminal-width` | One commit not merged, local only | `b30d282`, "Measure the terminal width of UTF-8 text" | Not run |

On 24 October 2026 dependencies stopped being committed. curl and mbedTLS would have added 13.7 MB of source, 5.5 MB in git, to every clone and to every project that pins libmc. `deps.lock` now pins each dependency by content hash, the build fetches it, and cyaml, yyjson, utf8proc, mbedTLS, curl, nghttp2 and zlib are published as `deps-*` releases.

The first GitHub CI run of the HTTP client failed on Linux, where it had passed locally. The test's HTTP/2 server closed the connection with the client's last frames unread, and Linux then resets the connection, discarding the response. Keeping the connection open until the client closes it showed a second fault: destroying the last `HttpClient` frees mbedTLS's crypto state, which the test server shares, so the TLS test server now holds an `HttpClient` of its own.

## HTTP client

Done. Merged on 24 October 2026 through PR #19: `mc/net/http.h` on curl and mbedTLS, with HTTP/2 through nghttp2 and gzip and deflate through zlib, on macOS and both Linux targets. The bar was Go's `net/http`, which uses HTTP/2 and decodes compressed responses without being asked; a libmc caller gets the same.

- [x] **Trusted roots in minimal containers.** `ubuntu:24.04` has no bundle of trusted roots, so every request failed until `ca-certificates` was installed. Mozilla's roots, from curl's extract of 20 October 2026, are pinned as `deps-cacert-2026-10-20` and built into libmc, and the client uses them where `ca_file` is empty and the system has no bundle, as Go's `x509roots/fallback` does. `tools/cacert.sh` writes a newer extract into `deps/cacert`.

## Markdown and XML

Next, each on its own branch. JSON, YAML and CSV are in libmc; Markdown and XML are the formats projects use most that it lacks.

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

- **XML as events or as a tree.** Leaning: events first. Decided by the first project that reads XML.
