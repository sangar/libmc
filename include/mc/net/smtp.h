#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// Sending mail over SMTP with TLS: libcurl on mbedTLS, pinned in deps.lock.
// smtp_send builds the MIME message itself, so a caller only fills in a
// MailMessage.

// MailAddress is a mailbox: an address and an optional display name, which may be UTF-8.
typedef struct MailAddress {
    String name;
    String address;
} MailAddress;

typedef struct MailAttachment {
    // filename is what the recipient sees, and may be UTF-8.
    String filename;
    // content_type is such as application/pdf; empty is application/octet-stream.
    String content_type;
    String data;
} MailAttachment;

typedef struct MailMessage {
    MailAddress from;
    const MailAddress *to;
    size_t to_count;
    const MailAddress *cc;
    size_t cc_count;
    // bcc receive the message without being named in it.
    const MailAddress *bcc;
    size_t bcc_count;
    // subject may be UTF-8.
    String subject;
    // text is the body as plain text. html, when not empty, is the same body
    // as HTML, which mail clients show instead.
    String text;
    String html;
    const MailAttachment *attachments;
    size_t attachment_count;
} MailMessage;

// SmtpConfig is the server and how to sign in. Zero values take the default
// named beside each field.
typedef struct SmtpConfig {
    // url is smtps://host for TLS from the start, port 465 by default, or
    // smtp://host:587 for STARTTLS, which is then required.
    String url;
    // username and password sign in when username is not empty.
    String username;
    String password;
    // ca_file is a PEM bundle of trusted roots; empty uses the system's,
    // http_default_ca_file, or Mozilla's roots built into libmc where it has none.
    String ca_file;
    // connect_timeout_ms bounds connecting and the TLS handshake: 10 s.
    int64_t connect_timeout_ms;
    // timeout_ms bounds the whole exchange: 60 s.
    int64_t timeout_ms;
} SmtpConfig;

// smtp_send sends message to its to, cc and bcc addresses. It adds the Date
// and Message-ID headers, encodes UTF-8 names and the subject as RFC 2047
// words, and sends the text and HTML as quoted-printable UTF-8 and the
// attachments as base64. No recipient, an address without @, a line break in
// a header, or a URL that is not smtp:// or smtps:// is ERR_INVALID_ARGUMENT;
// a ca_file that does not exist ERR_NOT_FOUND; a timeout ERR_TIMEOUT; and a
// server that refuses the sign-in, a recipient or the message, or offers no
// STARTTLS, ERR_NETWORK with its reply in err.
[[nodiscard]] Error smtp_send(const SmtpConfig *config, const MailMessage *message, Err *err);
