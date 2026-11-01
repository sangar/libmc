#pragma once

#include <stdint.h>

#include "mc/core/arena.h"
#include "mc/text/str.h"

// Internet mail messages (RFC 5322) and their MIME bodies (RFC 2045 to 2047,
// 2183, 2231) read for showing them: the header fields, the subject,
// addresses, date and message ids, and the leaves of the MIME tree with their
// bodies decoded. Lines may end in CRLF or LF, and header names match in any
// ASCII case.
//
// Text is converted to UTF-8 from its declared charset: utf-8, iso-8859-15 or
// windows-1252, which is also how iso-8859-1 and latin1 are read, since mail
// that declares them uses bytes 0x80 to 0x9F as Windows-1252 does. Text with no
// charset or us-ascii, and 8-bit bytes in a header, are kept when they are
// valid UTF-8 and read as Windows-1252 otherwise. Any other charset is read as
// UTF-8. Bytes that are not valid UTF-8 become U+FFFD. Charset names match in
// any ASCII case, with aliases such as utf8 and latin1. The subject, display
// names and file names are in NFC, so an "å" that macOS Mail sends decomposed
// in a file name compares equal to a typed one.

// MailAddress is one mailbox: a display name, empty when there is none, and
// an address, either of which may be UTF-8. smtp_send takes it as it is;
// mail_parse gives the display name decoded and the address as written, and
// takes the comment of an address with no display name, as in
// a@example.com (Ann), as its name.
typedef struct MailAddress {
    String name;
    String address;
} MailAddress;

// MailAddressList is a field's mailboxes in order, those inside groups such
// as Team: a@example.com, b@example.com; included and the group names left
// out.
typedef struct MailAddressList {
    MailAddress *items;
    size_t count;
} MailAddressList;

// MailHeader is one header field, unfolded, its value as written without the
// whitespace around it.
typedef struct MailHeader {
    String name;
    String value;
} MailHeader;

// MailPart is one leaf of a message's MIME tree, in document order.
typedef struct MailPart {
    // content_type is type/subtype in lower case, text/plain when the part
    // names none or a malformed one.
    String content_type;
    String charset; // in lower case as declared, empty when none
    // attachment is set by Content-Disposition: attachment, and for a part
    // that is neither text nor inline with a file name.
    bool attachment;
    // filename is Content-Disposition's, or else Content-Type's name, decoded
    // from RFC 2231 or RFC 2047, and empty when there is none.
    String filename;
    String content_id; // without angle brackets
    // body is decoded from base64 or quoted-printable, and for a text/* part
    // converted to valid UTF-8.
    String body;
} MailPart;

// ParsedMail is a message as mail_parse reads it. smtp.h's MailMessage is
// one to send.
typedef struct ParsedMail {
    // headers are the top-level fields, in order.
    const MailHeader *headers;
    size_t header_count;
    String subject; // decoded to UTF-8
    MailAddressList from, to, cc, bcc, reply_to;
    int64_t date;      // the Date field in Unix nanoseconds, 0 when missing or malformed
    String message_id; // without angle brackets
    // in_reply_to is the first message id in In-Reply-To, and references the
    // message ids of References, oldest first, all without angle brackets.
    String in_reply_to;
    StringList references;
    const MailPart *parts;
    size_t part_count;
    size_t text; // index into parts of the plain text body to show, SIZE_MAX when none
    size_t html; // index into parts of the HTML body to show, SIZE_MAX when none
} ParsedMail;

// mail_parse reads raw into arena. It never fails: mail that breaks the RFCs,
// as much real mail does, gives empty fields or a best-effort body rather than
// an error, and every String it returns is valid UTF-8 except the bodies of
// non-text parts. The Strings view raw where they can, so raw must outlive
// the message, and they are not NUL-terminated.
//
// The header section ends at the first empty line, or at the first line that
// is neither a field nor a folded continuation, and a leading mbox "From "
// line is skipped. The date is RFC 5322's, with or without the weekday, with a
// numeric zone or an obsolete name such as GMT or PST, and a two-digit year
// taken as 1950 to 2049; a missing or unknown zone is UTC. A multipart body is
// split at its boundary, its preamble and epilogue ignored and a missing
// closing boundary tolerated; one with no boundary found is a text/plain leaf.
// A message/rfc822 part is one leaf, not opened. The text and html parts are
// the first text/plain and text/html leaves that are not attachments.
// Multiparts nested deeper than 16 levels are dropped, and leaves past the
// 512th.
ParsedMail mail_parse(Arena *arena, String raw);

// mail_decode_words decodes the RFC 2047 encoded words in a header value, B
// and Q in any charset above, to UTF-8 in NFC, in arena. It drops the
// whitespace between adjacent words and joins the bytes of adjacent words in
// one charset, so that a character split across two words survives. A
// malformed word is left as written.
String mail_decode_words(Arena *arena, String value);
