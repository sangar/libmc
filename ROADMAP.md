# libmc roadmap

libmc is the foundation library for C projects under the `modern-c` contract. A project pins it in `deps.lock`, runs the build and gets arenas, strings, one error model, containers, text, the common data formats and networking, with nothing to install but a C compiler, `curl` and `tar`. It grows toward a C counterpart to Apple's Foundation: what a program needs to call web APIs, send mail, move files and read the formats people exchange, in one library with one style.

It is not a port of Foundation's API, not a shared library with a stable ABI, and not for Windows until `src/platform/` has a Windows implementation. This file covers one repository, [sangar/libmc](https://github.com/sangar/libmc).

Checked on 2026-10-25

## Focus

libmc is in use by projects under the `modern-c` contract, such as dbox and bprocs, and is where they get shared code. JSON, YAML, CSV, Markdown, XML, the HTTP and S3 clients, mail and FTP are in. What moves it forward now is the piece those projects reach for next:

1. **SSH and SFTP:** waits for libssh2 to build with mbedTLS 4. See [SSH and SFTP](#ssh-and-sftp).

While it is blocked, the next piece comes from the ideas [worth doing soon](#worth-doing-soon). Small items wait unless they block one of these.

## Principles

- **Nothing to install.** `deps.lock` pins every dependency by the hash of its content and the build fetches it from this repository's `deps-<name>-<version>` releases. A fresh clone needs a C compiler, `curl` and `tar`; see `modern-c` rule 10.
- **One branch per capability.** HTTP, Markdown, XML, SMTP, FTP and SSH each land through their own branch and pull request. Everything HTTP, HTTP/2 and response compression included, belongs to `http-client`.
- **curl before another library.** A protocol curl speaks is turned on in [`tools/curl-config.sh`](tools/curl-config.sh), not added as a new dependency. curl already carries the code for WebSocket, compiled out today, and for URL parsing.
- **Safe defaults for input from the network.** HTTPS only unless a test asks for plain HTTP, allowed hosts checked on every redirect, and limits on time and size. Markdown from users renders without raw HTML.
- **A new dependency needs an approved decision report.** Each one is chosen with a `c-libraries` report, as `modern-c` rule 11 requires.

## Where we are

`master` is at `bfc56d7` from 25 October 2026, with the HTTP client (PR #19), Mozilla's trusted roots for systems without their own (PR #20), Markdown (PR #21), XML (PR #22), text measured in terminal columns (PR #23), sending mail (PR #24), an S3 client (PR #25), moving files over FTPS (PR #26), socket pairs (PR #27) and `assert` kept in release builds (PR #28) merged. PR #25 also streams HTTP bodies through files, with cancellation and per-request timeouts. The branches already merged or superseded were deleted on GitHub on 24 October 2026.

| Branch | State | Last commit | Tests |
|---|---|---|---|
| `master` | Merged | `bfc56d7`, 25 Oct 2026, PR #28 | GitHub CI passes on macOS, Linux x86_64 and aarch64 with clang 18 and gcc 14, and the cross builds |

On 24 October 2026 dependencies stopped being committed. curl and mbedTLS would have added 13.7 MB of source, 5.5 MB in git, to every clone and to every project that pins libmc. `deps.lock` now pins each dependency by content hash, the build fetches it, and cyaml, yyjson, utf8proc, mbedTLS, curl, nghttp2 and zlib are published as `deps-*` releases.

The first GitHub CI run of the HTTP client failed on Linux, where it had passed locally. The test's HTTP/2 server closed the connection with the client's last frames unread, and Linux then resets the connection, discarding the response. Keeping the connection open until the client closes it showed a second fault: destroying the last `HttpClient` frees mbedTLS's crypto state, which the test server shares, so the TLS test server now holds an `HttpClient` of its own.

Under gcc's sanitizers on Linux, the billion laughs test of XML took 100 seconds and timed the suite out, against 2 seconds under clang's; its innermost entity is now 1 KiB, so Expat's check comes after far fewer expansions. `watch_outlives_many_removed_directories` fails about 2 runs in 7 on macOS, on `master` too, and is not fixed yet.

## HTTP client

Done. Merged on 24 October 2026 through PR #19: `mc/net/http.h` on curl and mbedTLS, with HTTP/2 through nghttp2 and gzip and deflate through zlib, on macOS and both Linux targets. The bar was Go's `net/http`, which uses HTTP/2 and decodes compressed responses without being asked; a libmc caller gets the same.

- [x] **Trusted roots in minimal containers.** `ubuntu:24.04` has no bundle of trusted roots, so every request failed until `ca-certificates` was installed. Mozilla's roots, from curl's extract of 20 October 2026, are pinned as `deps-cacert-2026-10-20` and built into libmc, and the client uses them where `ca_file` is empty and the system has no bundle, as Go's `x509roots/fallback` does. `tools/cacert.sh` writes a newer extract into `deps/cacert`.

## Markdown and XML

Done. Merged on 24 October 2026 through PRs #21 and #22: the two formats projects use most that libmc lacked.

- [x] **Render Markdown to HTML.** Merged on 24 October 2026 through PR #21: `mc/encoding/markdown.h` renders CommonMark 0.31.2 with GitHub's tables, task lists, strikethrough and autolinks, which is what people expect from Markdown since GitHub. Raw HTML in Markdown from users lets them inject scripts, so by default it is shown as text and links to `javascript:`, `vbscript:`, `file:` and `data:` URLs are emptied, as cmark does; `allow_html` turns both off for trusted text.
      1. [x] **A decision report, md4c against cmark-gfm, approved** on 24 October 2026. cmark-gfm's last release is from July 2023 and keeps its extensions in a process-wide registry; md4c 0.6.0 is from September 2026 and has no global state.
      2. [x] **md4c pinned and wrapped.** md4c's own HTML renderer has no link filter, so libmc renders from md4c's callbacks itself. The output matches all 652 examples of the CommonMark spec, which come with md4c in `deps/md4c/test/spec.txt`; in three of them GitHub's autolinks link a bare URL, as GitHub does.
- [x] **Read XML.** Merged on 24 October 2026 through PR #22: `mc/encoding/xml.h` reads a document into start tags, end tags and text, like the tokens of Go's `xml.Decoder`, with names as written and namespaces not resolved. A tree, as Go's `Unmarshal` gives, waits for a project that needs one.
      1. [x] **A decision report, Expat against libxml2, approved** on 24 October 2026. libxml2's maintainer stepped down in September 2025; Expat 2.9.0 is from October 2026 and fixes vulnerabilities as they come.
      2. [x] **Expat pinned,** with `src/encoding/expat_config.h` written by hand. It seeds its hash salt with `getentropy`, which macOS and glibc both have, so one configuration fits both.
      3. [x] **The event API,** with Expat's protection against entity expansion ("billion laughs") left on and external entities never loaded, both tested. Against James Clark's XML test cases, which may not be redistributed in part and so are not in the tests, it refuses all 186 malformed documents and matches the canonical form of the 113 valid ones that have no processing instructions, except 4 whose canonical form keeps notation declarations.

## Mail and file transfer

Done. Merged on 24 October 2026 through PRs #24 and #26. Both use the curl libmc already pins, each with its protocol turned on in `tools/curl-config.sh`.

- [x] **Send mail with attachments.** Merged on 24 October 2026 through PR #24. Go's `net/smtp` is frozen and takes the message as raw bytes, leaving MIME to the caller; Python's `email` package and Node's nodemailer build it from fields, and so does `smtp_send`: from, to, cc and bcc, a UTF-8 subject and names as RFC 2047 words, the text with an HTML alternative, and attachments with UTF-8 file names as RFC 2231 has them. It sends over SMTPS or STARTTLS only, and refuses a server that offers no STARTTLS before the password or the mail leaves. SMTP and MIME are on in `tools/curl-config.sh`. Tested against a server in the tests, not yet against a real provider.
- [x] **Upload and download files over FTP.** Merged on 24 October 2026 through PR #26. Python's `ftplib` and Go's `jlaffaye/ftp` keep a connection open across calls, and so does an `FtpClient` in `mc/net/ftp.h`: it uploads, downloads and lists, streaming between the server and local files, and takes a `Cancel` as the S3 client does. It requires TLS, through AUTH TLS for `ftp://` or from the start for `ftps://`, for the data connections too; plain FTP only when the caller sets `allow_plain_ftp`. FTP is on in `tools/curl-config.sh`. Tested against a server in the tests, and once against vsftpd 3.0.5 in `ubuntu:24.04` in both modes, with its default of requiring TLS session reuse on data connections.

## SSH and SFTP

Now, and blocked. curl speaks SSH and SFTP through libssh2 (BSD), which needs a crypto library underneath. [libssh2 1.11.1 does not build with mbedTLS 4.x](https://github.com/libssh2/libssh2/pull/2284), and the pull request that moves its mbedTLS backend to the PSA API is open and, by its author on 5 October 2026, needs a heavy rework or a fresh start.

- [ ] **Run commands and move files over SSH.** `mc/net/ssh.h` on a branch `ssh`, once a libssh2 release builds with mbedTLS 4. Check each libssh2 release until then.

## Feature ideas

### Worth doing soon

- **URL parsing:** `mc/net/url.h` on curl's URL API, which is already compiled in, to parse, build and normalise URLs.
- **WebSocket client:** curl supports it; it is one flag in `tools/curl-config.sh`.
- **Compression:** gzip and deflate for files and archives, on the zlib the HTTP client brings.
- **Signing in to Gmail and Microsoft 365 with OAuth:** both refuse passwords for most accounts; curl sends an OAuth token with `CURLOPT_XOAUTH2_BEARER`.

### Later

- **Footnotes and GitHub's alerts in Markdown.** md4c 0.6.0 parses both; `src/encoding/markdown.c` needs their HTML.
- **Brotli and zstd responses.** Each is another dependency, and gzip covers most APIs.
- **Property lists,** on libmc's Foundation direction, once XML is in.
- **An XML tree and writing XML,** when a project needs more than the stream.
- **XML namespaces,** resolving prefixes to URIs as Go's `xml.Name.Space` does, when a project reads XML that mixes them.

### Not now

- **HTTP/3.** curl needs a QUIC stack and a TLS library that supports it. Revisit when an API a project calls needs it.
- **libssh2 on OpenSSL.** OpenSSL is a far bigger dependency than mbedTLS. Revisit if SSH is next and no libssh2 release builds with mbedTLS 4.

### Not doing

- **libssh instead of libssh2.** It is LGPL, which complicates linking it statically into `libmc.a`.
- **Committing dependency sources.** Decided against on 24 October 2026; see [Where we are](#where-we-are).

## Open questions

None right now.
