#include "mc/net/smtp.h"

#include <curl/curl.h>
#include <string.h>

#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "mc/text/utf8.h"
#include "roots.h"

enum {
    DEFAULT_CONNECT_TIMEOUT_MS = 10 * 1000,
    DEFAULT_TIMEOUT_MS = 60 * 1000,
    // An RFC 2047 word is at most 75 characters, of which =?UTF-8?Q? and ?= take 12.
    MAX_ENCODED_TEXT = 63,
    SECONDS_PER_DAY = 86400,
};

static const char *const WEEKDAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Mail holds what curl reads during the transfer; failed records any curl call that ran out of memory.
typedef struct Mail {
    Arena *arena;
    CURL *curl;
    struct curl_slist *headers;
    struct curl_slist *recipients;
    curl_mime *mime;
    bool failed;
} Mail;

static bool is_address(String address)
{
    size_t at = 0;
    return !str_contains_any(address, S(" \t\r\n<>(),;:\"")) && str_find_last_char(address, '@', &at) && at > 0 &&
           at + 1 < address.len;
}

static bool has_line_break(String s)
{
    return str_contains_any(s, S("\r\n"));
}

[[nodiscard]] static Error check_address(MailAddress mailbox, Err *err)
{
    if (!is_address(mailbox.address) || has_line_break(mailbox.name)) {
        return err_set(err, ERR_INVALID_ARGUMENT, "smtp: invalid mailbox %.*s", (int)min_size(mailbox.address.len, 256),
                       mailbox.address.data);
    }
    return ERR_OK;
}

[[nodiscard]] static Error check_message(const MailMessage *message, Err *err)
{
    if (message->to_count + message->cc_count + message->bcc_count == 0) {
        return err_set(err, ERR_INVALID_ARGUMENT, "smtp: the message has no recipients");
    }
    Error e = check_address(message->from, err);
    for (size_t i = 0; i < message->to_count && e == ERR_OK; i++) {
        e = check_address(message->to[i], err);
    }
    for (size_t i = 0; i < message->cc_count && e == ERR_OK; i++) {
        e = check_address(message->cc[i], err);
    }
    for (size_t i = 0; i < message->bcc_count && e == ERR_OK; i++) {
        e = check_address(message->bcc[i], err);
    }
    if (e == ERR_OK && has_line_break(message->subject)) {
        e = err_set(err, ERR_INVALID_ARGUMENT, "smtp: the subject holds a line break");
    }
    for (size_t i = 0; i < message->attachment_count && e == ERR_OK; i++) {
        const MailAttachment *attachment = &message->attachments[i];
        if (has_line_break(attachment->filename) || has_line_break(attachment->content_type)) {
            e = err_set(err, ERR_INVALID_ARGUMENT, "smtp: attachment %zu holds a line break in its name or type", i + 1);
        }
    }
    return e;
}

static bool is_plain_ascii(String s)
{
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.data[i];
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

// is_q_literal reports whether c stands for itself in an RFC 2047 word inside a phrase.
static bool is_q_literal(unsigned char c)
{
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
           (c != 0 && strchr("!*+-/", c) != nullptr);
}

static size_t q_length(String character)
{
    size_t length = 0;
    for (size_t i = 0; i < character.len; i++) {
        unsigned char c = (unsigned char)character.data[i];
        length += is_q_literal(c) || c == ' ' ? 1 : 3;
    }
    return length;
}

static void append_q(StringBuilder *out, String character)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < character.len; i++) {
        unsigned char c = (unsigned char)character.data[i];
        if (c == ' ') {
            str_builder_append_char(out, '_');
        } else if (is_q_literal(c)) {
            str_builder_append_char(out, (char)c);
        } else {
            str_builder_append_format(out, "=%c%c", HEX[c >> 4], HEX[c & 0xF]);
        }
    }
}

// append_encoded_words writes text as RFC 2047 words of Q-encoded UTF-8,
// split between characters and folded onto lines of their own.
static void append_encoded_words(StringBuilder *out, String text)
{
    str_builder_append(out, S("=?UTF-8?Q?"));
    size_t word_length = 0;
    size_t at = 0;
    while (at < text.len) {
        size_t start = at;
        uint32_t codepoint = 0;
        unused(utf8_decode(text, &at, &codepoint));
        String character = str_slice(text, start, at);
        size_t length = q_length(character);
        if (word_length + length > MAX_ENCODED_TEXT) {
            str_builder_append(out, S("?=\r\n =?UTF-8?Q?"));
            word_length = 0;
        }
        append_q(out, character);
        word_length += length;
    }
    str_builder_append(out, S("?="));
}

// append_phrase writes a display name or subject as is when it is plain ASCII, else as RFC 2047 words.
static void append_phrase(StringBuilder *out, String text, bool quoted)
{
    if (!is_plain_ascii(text)) {
        append_encoded_words(out, text);
        return;
    }
    if (!quoted) {
        str_builder_append(out, text);
        return;
    }
    str_builder_append_char(out, '"');
    for (size_t i = 0; i < text.len; i++) {
        if (text.data[i] == '"' || text.data[i] == '\\') {
            str_builder_append_char(out, '\\');
        }
        str_builder_append_char(out, text.data[i]);
    }
    str_builder_append_char(out, '"');
}

static void append_mailbox(StringBuilder *out, MailAddress mailbox)
{
    if (mailbox.name.len == 0) {
        str_builder_append(out, mailbox.address);
        return;
    }
    append_phrase(out, mailbox.name, true);
    str_builder_append_format(out, " <%.*s>", (int)mailbox.address.len, mailbox.address.data);
}

static void add_line(Mail *mail, struct curl_slist **list, String line)
{
    struct curl_slist *appended = curl_slist_append(*list, str_cstr(mail->arena, line));
    if (appended == nullptr) {
        mail->failed = true;
        return;
    }
    *list = appended;
}

static void add_mailbox_header(Mail *mail, const char *name, const MailAddress *mailboxes, size_t count)
{
    if (count == 0) {
        return;
    }
    StringBuilder header = str_builder_create(mail->arena, 64);
    str_builder_append_format(&header, "%s: ", name);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            str_builder_append(&header, S(",\r\n "));
        }
        append_mailbox(&header, mailboxes[i]);
    }
    add_line(mail, &mail->headers, str_builder_finish(&header));
}

static void add_recipients(Mail *mail, const MailAddress *mailboxes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        add_line(mail, &mail->recipients,
                 str_format(mail->arena, "<%.*s>", (int)mailboxes[i].address.len, mailboxes[i].address.data));
    }
}

// rfc5322_date writes a Unix time in UTC as Sat, 24 Oct 2026 12:00:00 +0000.
static String rfc5322_date(Arena *arena, int64_t unix_seconds)
{
    int64_t days = unix_seconds / SECONDS_PER_DAY;
    int64_t second_of_day = unix_seconds % SECONDS_PER_DAY;
    CivilDate date = civil_from_days(days);
    // 1 January 1970 was a Thursday.
    return str_format(arena, "%s, %02u %s %04lld %02lld:%02lld:%02lld +0000", WEEKDAYS[(days + 4) % 7], date.day,
                      MONTHS[date.month - 1], (long long)date.year, (long long)(second_of_day / 3600),
                      (long long)(second_of_day / 60 % 60), (long long)(second_of_day % 60));
}

static void add_headers(Mail *mail, const MailMessage *message)
{
    int64_t now = clock_wall_ns();
    add_line(mail, &mail->headers, str_format(mail->arena, "Date: %s", rfc5322_date(mail->arena, now / NS_PER_SECOND).data));
    String domain = { 0 };
    String local = { 0 };
    str_cut(message->from.address, '@', &local, &domain);
    add_line(mail, &mail->headers,
             str_format(mail->arena, "Message-ID: <%lld@%.*s>", (long long)now, (int)domain.len, domain.data));
    add_mailbox_header(mail, "From", &message->from, 1);
    add_mailbox_header(mail, "To", message->to, message->to_count);
    add_mailbox_header(mail, "Cc", message->cc, message->cc_count);
    StringBuilder subject = str_builder_create(mail->arena, 64);
    str_builder_append(&subject, S("Subject: "));
    append_phrase(&subject, message->subject, false);
    add_line(mail, &mail->headers, str_builder_finish(&subject));
}

static void check(Mail *mail, CURLcode code)
{
    if (code != CURLE_OK) {
        mail->failed = true;
    }
}

static curl_mimepart *add_part(Mail *mail, curl_mime *mime, String data, const char *type, const char *encoder)
{
    curl_mimepart *part = curl_mime_addpart(mime);
    if (part == nullptr) {
        mail->failed = true;
        return nullptr;
    }
    check(mail, curl_mime_data(part, data.len > 0 ? data.data : "", data.len));
    check(mail, curl_mime_type(part, type));
    check(mail, curl_mime_encoder(part, encoder));
    return part;
}

// add_text adds text with CRLF line breaks, which quoted-printable needs.
static void add_text(Mail *mail, curl_mime *mime, String text, const char *type)
{
    String lf = str_replace_all(mail->arena, text, S("\r\n"), S("\n"));
    unused(add_part(mail, mime, str_replace_all(mail->arena, lf, S("\n"), S("\r\n")), type, "quoted-printable"));
}

static void add_body(Mail *mail, const MailMessage *message)
{
    if (message->html.len == 0) {
        add_text(mail, mail->mime, message->text, "text/plain; charset=utf-8");
        return;
    }
    curl_mime *alternative = curl_mime_init(mail->curl);
    curl_mimepart *part = curl_mime_addpart(mail->mime);
    if (alternative == nullptr || part == nullptr) {
        curl_mime_free(alternative);
        mail->failed = true;
        return;
    }
    add_text(mail, alternative, message->text, "text/plain; charset=utf-8");
    add_text(mail, alternative, message->html, "text/html; charset=utf-8");
    check(mail, curl_mime_subparts(part, alternative));
    check(mail, curl_mime_type(part, "multipart/alternative"));
}

// disposition names the file as RFC 2231 has it for UTF-8 names, and quoted otherwise.
static String disposition(Arena *arena, String filename)
{
    if (filename.len == 0) {
        return S("Content-Disposition: attachment");
    }
    if (!is_plain_ascii(filename)) {
        return str_format(arena, "Content-Disposition: attachment; filename*=UTF-8''%s",
                          str_percent_encode(arena, filename, false).data);
    }
    StringBuilder header = str_builder_create(arena, 64);
    str_builder_append(&header, S("Content-Disposition: attachment; filename="));
    append_phrase(&header, filename, true);
    return str_builder_finish(&header);
}

static void add_attachment(Mail *mail, const MailAttachment *attachment)
{
    String type = attachment->content_type.len > 0 ? attachment->content_type : S("application/octet-stream");
    curl_mimepart *part = add_part(mail, mail->mime, attachment->data, str_cstr(mail->arena, type), "base64");
    struct curl_slist *headers = nullptr;
    add_line(mail, &headers, disposition(mail->arena, attachment->filename));
    if (part == nullptr || curl_mime_headers(part, headers, 1) != CURLE_OK) {
        curl_slist_free_all(headers);
        mail->failed = true;
    }
}

static void build(Mail *mail, const MailMessage *message)
{
    add_recipients(mail, message->to, message->to_count);
    add_recipients(mail, message->cc, message->cc_count);
    add_recipients(mail, message->bcc, message->bcc_count);
    add_headers(mail, message);
    mail->mime = curl_mime_init(mail->curl);
    if (mail->mime == nullptr) {
        mail->failed = true;
        return;
    }
    add_body(mail, message);
    for (size_t i = 0; i < message->attachment_count; i++) {
        add_attachment(mail, &message->attachments[i]);
    }
}

static void set_options(Mail *mail, const SmtpConfig *config, const MailMessage *message, const char *ca_file,
                        char *error_buffer)
{
    CURL *curl = mail->curl;
    curl_easy_setopt(curl, CURLOPT_URL, str_cstr(mail->arena, config->url));
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "smtp,smtps");
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    roots_set(curl, ca_file);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    int64_t connect_timeout = config->connect_timeout_ms > 0 ? config->connect_timeout_ms : DEFAULT_CONNECT_TIMEOUT_MS;
    int64_t timeout = config->timeout_ms > 0 ? config->timeout_ms : DEFAULT_TIMEOUT_MS;
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connect_timeout);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    if (config->username.len > 0) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, str_cstr(mail->arena, config->username));
        curl_easy_setopt(curl, CURLOPT_PASSWORD, str_cstr(mail->arena, config->password));
    }
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM,
                     str_cstr(mail->arena, str_format(mail->arena, "<%.*s>", (int)message->from.address.len,
                                                      message->from.address.data)));
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, mail->recipients);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, mail->headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mail->mime);
}

[[nodiscard]] static Error map_curl_error(CURLcode code, const char *detail, Err *err)
{
    const char *message = detail[0] != '\0' ? detail : curl_easy_strerror(code);
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
        return err_set(err, ERR_TIMEOUT, "smtp: %s", message);
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return err_set(err, ERR_INVALID_ARGUMENT, "smtp: %s", message);
    case CURLE_OUT_OF_MEMORY:
        return err_set(err, ERR_OUT_OF_MEMORY, "smtp: %s", message);
    default:
        return err_set(err, ERR_NETWORK, "smtp: %s", message);
    }
}

Error smtp_send(const SmtpConfig *config, const MailMessage *message, Err *err)
{
    Error e = check_message(message, err);
    if (e) {
        return e;
    }
    String ca_file = roots_file(config->ca_file);
    if (ca_file.len > 0 && !file_exists(ca_file)) {
        return err_set(err, ERR_NOT_FOUND, "smtp: no bundle of trusted roots at %.*s", (int)ca_file.len, ca_file.data);
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return err_set(err, ERR_PLATFORM, "smtp: curl_global_init failed");
    }
    Mail mail = { .arena = arena_create(0), .curl = curl_easy_init() };
    if (mail.curl != nullptr) {
        build(&mail, message);
    }
    if (mail.curl == nullptr || mail.failed) {
        e = err_set(err, ERR_OUT_OF_MEMORY, "smtp: cannot build the message");
    } else {
        char error_buffer[CURL_ERROR_SIZE] = "";
        set_options(&mail, config, message, ca_file.len > 0 ? str_cstr(mail.arena, ca_file) : nullptr, error_buffer);
        CURLcode code = curl_easy_perform(mail.curl);
        if (code != CURLE_OK) {
            e = map_curl_error(code, error_buffer, err);
        }
    }
    curl_mime_free(mail.mime);
    curl_slist_free_all(mail.recipients);
    curl_slist_free_all(mail.headers);
    curl_easy_cleanup(mail.curl);
    curl_global_cleanup();
    arena_destroy(mail.arena);
    return e;
}
