#pragma once

#include "mc/concurrency/cancel.h"
#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

// Moving files to and from an FTP server over TLS: libcurl on mbedTLS, pinned
// in deps.lock. Transfers stream between the server and local files, so a
// file's size is not bounded by memory. Paths on the server are relative to
// the directory it starts in, unless they begin with /, and may be UTF-8.

// FtpClientConfig is the server and how to sign in. Zero values take the
// default named beside each field.
typedef struct FtpClientConfig {
    // url is ftp://host for TLS through AUTH TLS, which is then required, or
    // ftps://host for TLS from the start, port 990 by default.
    String url;
    // username and password sign in; an empty username signs in as anonymous.
    String username;
    String password;
    // ca_file is a PEM bundle of trusted roots; empty uses the system's,
    // http_default_ca_file, or Mozilla's roots built into libmc where it has none.
    String ca_file;
    // connect_timeout_ms bounds connecting and the TLS handshake: 10 s.
    int64_t connect_timeout_ms;
    // stall_timeout_ms fails a transfer that moves no bytes, or a reply that
    // does not come, for that long: 60 s. Transfers have no other time limit,
    // since a large file takes as long as it takes while it keeps moving.
    int64_t stall_timeout_ms;
    // allow_plain_ftp lets ftp:// go on in the clear, password included, when the server offers no TLS.
    bool allow_plain_ftp;
} FtpClientConfig;

// FtpClient keeps its connection open between calls. One thread uses it at a
// time; give each thread its own.
typedef struct FtpClient FtpClient;

// ftp_client_create copies config and connects on the first call. A ca_file
// that does not exist is ERR_NOT_FOUND.
[[nodiscard]] Error ftp_client_create(const FtpClientConfig *config, FtpClient **client, Err *err);
void ftp_client_destroy(FtpClient *client);

// Every call takes an optional Cancel, which ends it with ERR_CANCELLED. A URL
// that is not ftp:// or ftps://, a path with a line break, or a remote_path
// that ends with / is ERR_INVALID_ARGUMENT; a file that is not on the server
// ERR_NOT_FOUND; a timeout ERR_TIMEOUT; failing to read or write fd ERR_IO;
// and anything else that stops a call, such as a refused sign-in or a server
// without TLS, ERR_NETWORK with the server's reply in err.

// ftp_download writes the file at remote_path into the regular file fd from
// its start, truncating it first. On failure fd may hold part of the file.
[[nodiscard]] Error ftp_download(FtpClient *client, Cancel *cancel, String remote_path, int fd, Err *err);

// ftp_upload stores the file fd, from its start to its end, read with pread,
// at remote_path, replacing any file there.
[[nodiscard]] Error ftp_upload(FtpClient *client, Cancel *cancel, String remote_path, int fd, Err *err);

// ftp_list names the files and directories in remote_directory, in arena. A
// listing over 16 MB is ERR_UNSUPPORTED.
[[nodiscard]] Error ftp_list(FtpClient *client, Cancel *cancel, Arena *arena, String remote_directory,
                             StringList *names, Err *err);
