#include "mc/encoding/mail.h"

#include <stdckdint.h>

#include "mc/encoding/base64.h"
#include "mc/text/fmt.h"
#include "mc/text/unicode.h"
#include "mc/text/utf8.h"

enum {
    MAX_DEPTH = 16,
    MAX_PARTS = 512,
    // MAX_PARAMETERS bounds the parameters read from one field, since RFC 2231
    // sections are looked up one by one.
    MAX_PARAMETERS = 64,
    SECONDS_PER_DAY = 86400,
};

typedef enum Charset { CHARSET_UTF8, CHARSET_UNDECLARED, CHARSET_WINDOWS_1252, CHARSET_ISO_8859_15 } Charset;

static const struct {
    const char *name;
    Charset charset;
} CHARSET_NAMES[] = {
    { "", CHARSET_UNDECLARED },
    { "us-ascii", CHARSET_UNDECLARED },
    { "ascii", CHARSET_UNDECLARED },
    { "windows-1252", CHARSET_WINDOWS_1252 },
    { "cp1252", CHARSET_WINDOWS_1252 },
    { "x-cp1252", CHARSET_WINDOWS_1252 },
    { "iso-8859-1", CHARSET_WINDOWS_1252 },
    { "iso8859-1", CHARSET_WINDOWS_1252 },
    { "iso_8859-1", CHARSET_WINDOWS_1252 },
    { "latin1", CHARSET_WINDOWS_1252 },
    { "latin-1", CHARSET_WINDOWS_1252 },
    { "l1", CHARSET_WINDOWS_1252 },
    { "iso-8859-15", CHARSET_ISO_8859_15 },
    { "iso8859-15", CHARSET_ISO_8859_15 },
    { "iso_8859-15", CHARSET_ISO_8859_15 },
    { "latin9", CHARSET_ISO_8859_15 },
    { "latin-9", CHARSET_ISO_8859_15 },
    { "l9", CHARSET_ISO_8859_15 },
};

// WINDOWS_1252_C1 holds the code points of bytes 0x80 to 0x9F. The five bytes
// Windows-1252 leaves undefined keep their ISO-8859-1 meaning, as browsers do.
static const uint16_t WINDOWS_1252_C1[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static Charset charset_named(String name)
{
    for (size_t i = 0; i < countof(CHARSET_NAMES); i++) {
        if (str_equal_ignore_case(name, S(CHARSET_NAMES[i].name))) {
            return CHARSET_NAMES[i].charset;
        }
    }
    return CHARSET_UTF8;
}

static uint32_t decode_byte(Charset charset, unsigned char byte)
{
    if (charset == CHARSET_ISO_8859_15) {
        switch (byte) {
        case 0xA4: return 0x20AC;
        case 0xA6: return 0x0160;
        case 0xA8: return 0x0161;
        case 0xB4: return 0x017D;
        case 0xB8: return 0x017E;
        case 0xBC: return 0x0152;
        case 0xBD: return 0x0153;
        case 0xBE: return 0x0178;
        default: return byte;
        }
    }
    return byte >= 0x80 && byte < 0xA0 ? WINDOWS_1252_C1[byte - 0x80] : byte;
}

static void append_code_point(StringBuilder *out, uint32_t code_point)
{
    char bytes[UTF8_MAX_BYTES];
    str_builder_append(out, (String){ bytes, utf8_encode(code_point, bytes) });
}

static String from_single_byte(Arena *arena, String text, Charset charset)
{
    StringBuilder out = str_builder_create(arena, text.len + text.len / 2);
    for (size_t i = 0; i < text.len; i++) {
        append_code_point(&out, decode_byte(charset, (unsigned char)text.data[i]));
    }
    return str_builder_finish(&out);
}

static String replace_invalid_utf8(Arena *arena, String text)
{
    StringBuilder out = str_builder_create(arena, text.len + 16);
    size_t position = 0;
    while (position < text.len) {
        uint32_t decoded;
        utf8_decode(text, &position, &decoded);
        append_code_point(&out, decoded);
    }
    return str_builder_finish(&out);
}

// to_utf8 converts text in charset to valid UTF-8, returning text itself when
// it is UTF-8 already.
static String to_utf8(Arena *arena, String text, Charset charset)
{
    if (charset == CHARSET_WINDOWS_1252 || charset == CHARSET_ISO_8859_15) {
        return from_single_byte(arena, text, charset);
    }
    if (utf8_is_valid(text)) {
        return text;
    }
    return charset == CHARSET_UNDECLARED ? from_single_byte(arena, text, CHARSET_WINDOWS_1252)
                                         : replace_invalid_utf8(arena, text);
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_blank(String s)
{
    for (size_t i = 0; i < s.len; i++) {
        if (!is_space(s.data[i])) {
            return false;
        }
    }
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// decode_quoted_printable decodes a body (RFC 2045 6.7), or with
// underscore_is_space an RFC 2047 Q word. An '=' that begins neither an
// escape nor a soft line break stays as it is.
static String decode_quoted_printable(Arena *arena, String text, bool underscore_is_space)
{
    StringBuilder out = str_builder_create(arena, text.len);
    for (size_t i = 0; i < text.len; i++) {
        char c = text.data[i];
        if (c == '_' && underscore_is_space) {
            c = ' ';
        } else if (c == '=') {
            size_t next = i + 1;
            while (next < text.len && (text.data[next] == ' ' || text.data[next] == '\t')) {
                next++;
            }
            if (next < text.len && text.data[next] == '\r' && next + 1 < text.len && text.data[next + 1] == '\n') {
                next++;
            }
            if (next == text.len || text.data[next] == '\n') {
                i = next;
                continue;
            }
            int high = i + 2 < text.len ? hex_value(text.data[i + 1]) : -1;
            int low = high >= 0 ? hex_value(text.data[i + 2]) : -1;
            if (low >= 0) {
                c = (char)(high << 4 | low);
                i += 2;
            }
        }
        str_builder_append_char(&out, c);
    }
    return str_builder_finish(&out);
}

// EncodedWord is an RFC 2047 =?charset?encoding?text?= decoded to its bytes,
// and the offset just past it.
typedef struct EncodedWord {
    String charset;
    String bytes;
    size_t end;
} EncodedWord;

// read_encoded_word reads the word that starts at value[start], which is "=?".
static bool read_encoded_word(Arena *arena, String value, size_t start, EncodedWord *word)
{
    size_t i = start + 2;
    while (i < value.len && value.data[i] != '?' && !is_space(value.data[i])) {
        i++;
    }
    if (i == start + 2 || i + 2 >= value.len || value.data[i] != '?' || value.data[i + 2] != '?') {
        return false;
    }
    String charset = str_slice(value, start + 2, i);
    String language;
    str_cut(charset, '*', &charset, &language);
    char encoding = value.data[i + 1];
    bool base64 = encoding == 'B' || encoding == 'b';
    if (!base64 && encoding != 'Q' && encoding != 'q') {
        return false;
    }
    size_t text_start = i + 3;
    i = text_start;
    while (i < value.len && value.data[i] != '?' && !is_space(value.data[i])) {
        i++;
    }
    if (i + 1 >= value.len || value.data[i] != '?' || value.data[i + 1] != '=') {
        return false;
    }
    String text = str_slice(value, text_start, i);
    word->charset = charset;
    word->bytes = base64 ? base64_decode_lenient(arena, text) : decode_quoted_printable(arena, text, true);
    word->end = i + 2;
    return true;
}

// flush_words appends the bytes of the pending words to out in UTF-8 and empties pending.
static void flush_words(StringBuilder *out, StringBuilder *pending, String charset)
{
    str_builder_append(out, to_utf8(out->arena, str_builder_finish(pending), charset_named(charset)));
    pending->len = 0;
}

String mail_decode_words(Arena *arena, String value)
{
    if (!str_contains(value, S("=?"))) {
        return unicode_nfc(arena, to_utf8(arena, value, CHARSET_UNDECLARED));
    }
    StringBuilder out = str_builder_create(arena, value.len);
    // pending holds the bytes of adjacent words in one charset until a
    // different charset or other text comes.
    StringBuilder pending = str_builder_create(arena, value.len);
    String pending_charset = S("");
    bool after_word = false;
    size_t text_start = 0;
    size_t i = 0;
    while (i < value.len) {
        EncodedWord word;
        if (value.data[i] != '=' || i + 1 == value.len || value.data[i + 1] != '?' ||
            !read_encoded_word(arena, value, i, &word)) {
            i++;
            continue;
        }
        String between = str_slice(value, text_start, i);
        bool adjacent = after_word && is_blank(between);
        if (!adjacent || !str_equal_ignore_case(word.charset, pending_charset)) {
            flush_words(&out, &pending, pending_charset);
        }
        if (!adjacent) {
            str_builder_append(&out, to_utf8(arena, between, CHARSET_UNDECLARED));
        }
        str_builder_append(&pending, word.bytes);
        pending_charset = word.charset;
        after_word = true;
        i = text_start = word.end;
    }
    flush_words(&out, &pending, pending_charset);
    str_builder_append(&out, to_utf8(arena, str_slice(value, text_start, value.len), CHARSET_UNDECLARED));
    return unicode_nfc(arena, str_builder_finish(&out));
}

// next_line returns the line at *position without its line break and moves
// *position past the break.
static String next_line(String text, size_t *position)
{
    String rest = str_slice(text, *position, text.len);
    size_t end;
    if (!str_find_char(rest, '\n', &end)) {
        end = rest.len;
    }
    *position += end < rest.len ? end + 1 : end;
    return str_trim_suffix(str_slice(rest, 0, end), S("\r"));
}

static bool is_field_name(String name)
{
    for (size_t i = 0; i < name.len; i++) {
        unsigned char c = (unsigned char)name.data[i];
        if (c <= ' ' || c > '~') {
            return false;
        }
    }
    return name.len > 0;
}

static String without_line_breaks(Arena *arena, String s)
{
    if (!str_contains_any(s, S("\r\n"))) {
        return s;
    }
    StringBuilder out = str_builder_create(arena, s.len);
    for (size_t i = 0; i < s.len; i++) {
        if (s.data[i] != '\r' && s.data[i] != '\n') {
            str_builder_append_char(&out, s.data[i]);
        }
    }
    return str_builder_finish(&out);
}

// Fields is a header section's fields as they are read.
typedef struct Fields {
    MailHeader *items;
    size_t count;
    size_t capacity;
} Fields;

static void add_field(Arena *arena, Fields *fields, String name, String folded)
{
    fields->items = arena_grow(arena, fields->items, &fields->capacity, fields->count, sizeof *fields->items);
    String value = str_trim(without_line_breaks(arena, folded));
    fields->items[fields->count++] = (MailHeader){ name, to_utf8(arena, value, CHARSET_UNDECLARED) };
}

// read_fields reads the header section at the start of entity into fields and
// returns the body that follows it.
static String read_fields(Arena *arena, String entity, Fields *fields)
{
    String name = { 0 };
    size_t value_start = 0;
    size_t value_end = 0;
    size_t position = 0;
    String body = str_slice(entity, entity.len, entity.len);
    while (position < entity.len) {
        size_t line_start = position;
        String line = next_line(entity, &position);
        if (line.len == 0) {
            body = str_slice(entity, position, entity.len);
            break;
        }
        if (line.data[0] == ' ' || line.data[0] == '\t') {
            value_end = name.len > 0 ? line_start + line.len : value_end;
            continue;
        }
        String head;
        String tail;
        if (str_cut(line, ':', &head, &tail) && is_field_name(str_trim(head))) {
            if (name.len > 0) {
                add_field(arena, fields, name, str_slice(entity, value_start, value_end));
            }
            name = str_trim(head);
            value_start = line_start + head.len + 1;
            value_end = line_start + line.len;
            continue;
        }
        if (line_start == 0 && str_starts_with(line, S("From "))) {
            continue;
        }
        body = str_slice(entity, line_start, entity.len);
        break;
    }
    if (name.len > 0) {
        add_field(arena, fields, name, str_slice(entity, value_start, value_end));
    }
    return body;
}

// field_value is the value of the first field called name, empty when there is none.
static String field_value(Fields fields, const char *name)
{
    for (size_t i = 0; i < fields.count; i++) {
        if (str_equal_ignore_case(fields.items[i].name, S(name))) {
            return fields.items[i].value;
        }
    }
    return S("");
}

// skip_comment returns the offset past the comment that starts at s[start],
// which is '(', or the end of s when it is never closed.
static size_t skip_comment(String s, size_t start)
{
    size_t depth = 0;
    for (size_t i = start; i < s.len; i++) {
        if (s.data[i] == '\\') {
            i++;
        } else if (s.data[i] == '(') {
            depth++;
        } else if (s.data[i] == ')' && --depth == 0) {
            return i + 1;
        }
    }
    return s.len;
}

// read_quoted appends the quoted string that starts at s[start], which is
// '"', unescaped, and returns the offset past it.
static size_t read_quoted(String s, size_t start, StringBuilder *out)
{
    for (size_t i = start + 1; i < s.len; i++) {
        if (s.data[i] == '"') {
            return i + 1;
        }
        if (s.data[i] == '\\' && i + 1 < s.len) {
            i++;
        }
        str_builder_append_char(out, s.data[i]);
    }
    return s.len;
}

// Mailbox is one mailbox of an address list as it is read: the words before
// an address in angle brackets, or the bare address, with whitespace runs
// made single spaces and comments left out.
typedef struct Mailbox {
    StringBuilder words;
    bool space;
    String angle_address;
    bool has_angle_address;
    String comment;
} Mailbox;

static void add_mailbox(Arena *arena, MailAddressList *list, size_t *capacity, Mailbox *mailbox)
{
    String words = str_builder_finish(&mailbox->words);
    MailAddress address = { .name = words, .address = str_trim(mailbox->angle_address) };
    if (!mailbox->has_angle_address) {
        address = (MailAddress){ .name = S(""), .address = words };
    }
    if (address.name.len == 0) {
        address.name = str_trim(mailbox->comment);
    }
    address.name = mail_decode_words(arena, address.name);
    if (address.address.len > 0 || address.name.len > 0) {
        list->items = arena_grow(arena, list->items, capacity, list->count, sizeof *list->items);
        list->items[list->count++] = address;
    }
    *mailbox = (Mailbox){ .words = str_builder_create(arena, 0) };
}

static MailAddressList read_addresses(Arena *arena, String value)
{
    MailAddressList list = { 0 };
    size_t capacity = 0;
    Mailbox mailbox = { .words = str_builder_create(arena, 0) };
    size_t i = 0;
    while (i < value.len) {
        char c = value.data[i];
        if (c == ',' || c == ';') {
            add_mailbox(arena, &list, &capacity, &mailbox);
            i++;
        } else if (c == ':' && !mailbox.has_angle_address) {
            mailbox = (Mailbox){ .words = str_builder_create(arena, 0) };
            i++;
        } else if (c == '(') {
            size_t end = skip_comment(value, i);
            if (mailbox.comment.len == 0) {
                mailbox.comment = str_trim_suffix(str_slice(value, i + 1, end), S(")"));
            }
            i = end;
        } else if (c == '<') {
            size_t close;
            String rest = str_slice(value, i + 1, value.len);
            if (!str_find_char(rest, '>', &close)) {
                close = rest.len;
            }
            mailbox.angle_address = str_slice(rest, 0, close);
            mailbox.has_angle_address = true;
            i += close + 2;
        } else if (is_space(c)) {
            mailbox.space = mailbox.words.len > 0;
            i++;
        } else {
            if (mailbox.space) {
                str_builder_append_char(&mailbox.words, ' ');
                mailbox.space = false;
            }
            // An encoded word is kept whole, since some mailers leave a comma
            // in one unencoded.
            EncodedWord word;
            if (c == '"') {
                i = read_quoted(value, i, &mailbox.words);
            } else if (str_starts_with(str_slice(value, i, value.len), S("=?")) &&
                       read_encoded_word(arena, value, i, &word)) {
                str_builder_append(&mailbox.words, str_slice(value, i, word.end));
                i = word.end;
            } else {
                str_builder_append_char(&mailbox.words, c);
                i++;
            }
        }
    }
    add_mailbox(arena, &list, &capacity, &mailbox);
    return list;
}

static String without_space(Arena *arena, String s)
{
    StringList fields = str_fields(arena, s);
    return fields.count == 1 ? fields.items[0] : str_join(arena, fields, S(""));
}

// message_ids returns what each <...> in value holds, without whitespace, or,
// when value has none, its words that hold an '@'.
static StringList message_ids(Arena *arena, String value)
{
    StringList ids = { 0 };
    String rest = value;
    size_t open;
    while (str_find_char(rest, '<', &open)) {
        rest = str_slice(rest, open + 1, rest.len);
        size_t close;
        if (!str_find_char(rest, '>', &close)) {
            close = rest.len;
        }
        String id = without_space(arena, str_slice(rest, 0, close));
        if (id.len > 0) {
            strlist_push(arena, &ids, id);
        }
        rest = str_slice(rest, close + 1, rest.len);
    }
    if (ids.count > 0) {
        return ids;
    }
    StringList words = str_fields(arena, value);
    for (size_t i = 0; i < words.count; i++) {
        if (str_contains(words.items[i], S("@"))) {
            strlist_push(arena, &ids, words.items[i]);
        }
    }
    return ids;
}

static String first_message_id(Arena *arena, String value)
{
    StringList ids = message_ids(arena, value);
    return ids.count > 0 ? ids.items[0] : S("");
}

// read_number reads 1 to max_digits decimal digits.
static bool read_number(String s, size_t max_digits, uint64_t *value)
{
    return s.len > 0 && s.len <= max_digits && str_is_digits(s) && str_parse_u64(s, value);
}

static bool read_month(String name, unsigned *month)
{
    static const char *const MONTHS[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                          "jul", "aug", "sep", "oct", "nov", "dec" };
    for (unsigned i = 0; i < countof(MONTHS); i++) {
        if (name.len >= 3 && str_equal_ignore_case(str_slice(name, 0, 3), S(MONTHS[i]))) {
            *month = i + 1;
            return true;
        }
    }
    return false;
}

// zone_offset is the zone's offset east of UTC in seconds: a numeric +hhmm or
// -hhmm, or an obsolete name of RFC 5322 4.3. Anything else is UTC, as RFC
// 5322 asks of the military zones.
static int64_t zone_offset(String zone)
{
    static const struct {
        const char *name;
        int hours;
    } NAMES[] = {
        { "EST", -5 }, { "EDT", -4 }, { "CST", -6 }, { "CDT", -5 },
        { "MST", -7 }, { "MDT", -6 }, { "PST", -8 }, { "PDT", -7 },
    };
    uint64_t hhmm;
    if (zone.len == 5 && (zone.data[0] == '+' || zone.data[0] == '-') &&
        read_number(str_slice(zone, 1, 5), 4, &hhmm) && hhmm % 100 < 60) {
        int64_t seconds = (int64_t)(hhmm / 100 * 3600 + hhmm % 100 * 60);
        return zone.data[0] == '-' ? -seconds : seconds;
    }
    for (size_t i = 0; i < countof(NAMES); i++) {
        if (str_equal_ignore_case(zone, S(NAMES[i].name))) {
            return NAMES[i].hours * 3600;
        }
    }
    return 0;
}

// read_time reads hh:mm or hh:mm:ss into seconds since midnight.
static bool read_time(String time, int64_t *seconds)
{
    String hour;
    String minute;
    String second;
    str_cut(time, ':', &hour, &minute);
    if (!str_cut(minute, ':', &minute, &second)) {
        second = S("0");
    }
    uint64_t h;
    uint64_t m;
    uint64_t s;
    if (!read_number(hour, 2, &h) || !read_number(minute, 2, &m) || !read_number(second, 2, &s) || h > 23 ||
        m > 59 || s > 60) {
        return false;
    }
    *seconds = (int64_t)(h * 3600 + m * 60 + s);
    return true;
}

// read_date reads an RFC 5322 date-time, [day-of-week ","] day month year
// time [zone], into Unix nanoseconds, or 0 when it is malformed.
static int64_t read_date(String value)
{
    String tokens[6];
    size_t count = 0;
    size_t i = 0;
    while (i < value.len && count < countof(tokens)) {
        if (value.data[i] == '(') {
            i = skip_comment(value, i);
            continue;
        }
        if (is_space(value.data[i]) || value.data[i] == ',') {
            i++;
            continue;
        }
        size_t start = i;
        while (i < value.len && !is_space(value.data[i]) && value.data[i] != ',' && value.data[i] != '(') {
            i++;
        }
        tokens[count++] = str_slice(value, start, i);
    }
    size_t first = count > 0 && !str_is_digits(str_slice(tokens[0], 0, 1)) ? 1 : 0;
    if (count < first + 4) {
        return 0;
    }
    const String *t = tokens + first;
    uint64_t day;
    unsigned month;
    uint64_t year;
    int64_t seconds;
    if (!read_number(t[0], 2, &day) || day < 1 || day > 31 || !read_month(t[1], &month) ||
        !read_number(t[2], 4, &year) || t[2].len == 1 || !read_time(t[3], &seconds)) {
        return 0;
    }
    if (t[2].len == 2) {
        year += year < 50 ? 2000 : 1900;
    } else if (t[2].len == 3) {
        year += 1900;
    }
    CivilDate date = { .year = (int64_t)year, .month = month, .day = (unsigned)day };
    int64_t days = civil_to_days(date);
    CivilDate check = civil_from_days(days);
    if (check.month != date.month || check.day != date.day) {
        return 0;
    }
    int64_t offset = first + 4 < count ? zone_offset(t[4]) : 0;
    int64_t nanoseconds;
    if (ckd_mul(&nanoseconds, days * SECONDS_PER_DAY + seconds - offset, (int64_t)NS_PER_SECOND)) {
        return 0;
    }
    return nanoseconds;
}

// Parameter is one name=value of a Content-Type or Content-Disposition field,
// with an RFC 2231 name such as filename*1* taken apart.
typedef struct Parameter {
    String name;    // without the section and the stars
    size_t section; // SIZE_MAX for a plain name
    bool encoded;   // the name ends in '*': the value is %XX, after charset'language' in section 0
    String value;   // unquoted
} Parameter;

typedef struct Parameters {
    Parameter items[MAX_PARAMETERS];
    size_t count;
} Parameters;

// parameter_named takes an RFC 2231 name apart: name* is one encoded
// section, name*N and name*N* are section N, plain and encoded.
static Parameter parameter_named(String name, String value)
{
    Parameter parameter = { .name = name, .section = SIZE_MAX, .value = value };
    String base;
    String suffix;
    if (!str_cut(name, '*', &base, &suffix)) {
        return parameter;
    }
    String digits = str_trim_suffix(suffix, S("*"));
    uint64_t section = 0;
    if (digits.len > 0 && !read_number(digits, 3, &section)) {
        return parameter;
    }
    bool encoded = digits.len < suffix.len || suffix.len == 0;
    return (Parameter){ .name = base, .section = (size_t)section, .encoded = encoded, .value = value };
}

// read_parameters reads a field value of the form token *(";" name "=" value)
// and returns the token.
static String read_parameters(Arena *arena, String value, Parameters *parameters)
{
    String token;
    String rest;
    str_cut(value, ';', &token, &rest);
    size_t i = 0;
    while (i < rest.len && parameters->count < MAX_PARAMETERS) {
        if (is_space(rest.data[i]) || rest.data[i] == ';') {
            i++;
            continue;
        }
        size_t name_start = i;
        while (i < rest.len && rest.data[i] != '=' && rest.data[i] != ';') {
            i++;
        }
        String name = str_trim(str_slice(rest, name_start, i));
        if (i == rest.len || rest.data[i] == ';') {
            continue;
        }
        i++;
        while (i < rest.len && is_space(rest.data[i])) {
            i++;
        }
        String parameter_value;
        if (i < rest.len && rest.data[i] == '"') {
            StringBuilder unquoted = str_builder_create(arena, 0);
            i = read_quoted(rest, i, &unquoted);
            parameter_value = str_builder_finish(&unquoted);
        } else {
            size_t value_start = i;
            while (i < rest.len && rest.data[i] != ';') {
                i++;
            }
            parameter_value = str_trim(str_slice(rest, value_start, i));
        }
        parameters->items[parameters->count++] = parameter_named(name, parameter_value);
    }
    return str_trim(token);
}

static const Parameter *find_parameter(const Parameters *parameters, const char *name, size_t section)
{
    for (size_t i = 0; i < parameters->count; i++) {
        const Parameter *p = &parameters->items[i];
        if (p->section == section && str_equal_ignore_case(p->name, S(name))) {
            return p;
        }
    }
    return nullptr;
}

// percent_decoded decodes %XX, keeping value as it is when a '%' begins none.
static String percent_decoded(Arena *arena, String value)
{
    String decoded;
    return str_percent_decode(arena, value, &decoded) ? decoded : value;
}

// parameter_value is the value of the parameter called name in UTF-8: RFC
// 2231's sections name*0, name*1 and on joined, or name*, when there are any,
// else name, and empty when there is none.
static String parameter_value(Arena *arena, const Parameters *parameters, const char *name)
{
    String bytes = S("");
    Charset charset = CHARSET_UNDECLARED;
    size_t section = 0;
    for (; section < parameters->count; section++) {
        const Parameter *p = find_parameter(parameters, name, section);
        if (p == nullptr) {
            break;
        }
        String value = p->value;
        String declared;
        String language;
        String rest;
        if (p->encoded && section == 0 && str_cut(value, '\'', &declared, &rest) &&
            str_cut(rest, '\'', &language, &rest)) {
            charset = charset_named(declared);
            value = rest;
        }
        bytes = str_concat(arena, bytes, p->encoded ? percent_decoded(arena, value) : value);
    }
    if (section > 0) {
        return to_utf8(arena, bytes, charset);
    }
    const Parameter *plain = find_parameter(parameters, name, SIZE_MAX);
    return plain != nullptr ? plain->value : S("");
}

// Walk collects the leaves of a MIME tree.
typedef struct Walk {
    Arena *arena;
    MailPart *parts;
    size_t count;
    size_t capacity;
} Walk;

static bool is_mime_type(String type)
{
    String top;
    String sub;
    return str_cut(type, '/', &top, &sub) && top.len > 0 && sub.len > 0 && !str_contains_any(type, S(" \t\"(,"));
}

static String decode_transfer(Arena *arena, String body, String encoding)
{
    if (str_equal_ignore_case(encoding, S("base64"))) {
        return base64_decode_lenient(arena, body);
    }
    if (str_equal_ignore_case(encoding, S("quoted-printable"))) {
        return decode_quoted_printable(arena, body, false);
    }
    return body;
}

static void add_leaf(Walk *walk, Fields fields, String type, const Parameters *type_parameters, String body)
{
    Arena *arena = walk->arena;
    Parameters disposition_parameters = { 0 };
    String disposition = read_parameters(arena, field_value(fields, "Content-Disposition"), &disposition_parameters);
    String charset = parameter_value(arena, type_parameters, "charset");
    MailPart part = {
        .content_type = type,
        .charset = str_lower_ascii(arena, charset), // modern-c: allow ascii-text
        .filename = mail_decode_words(arena, parameter_value(arena, &disposition_parameters, "filename")),
        .body = decode_transfer(arena, body, field_value(fields, "Content-Transfer-Encoding")),
    };
    if (part.filename.len == 0) {
        part.filename = mail_decode_words(arena, parameter_value(arena, type_parameters, "name"));
    }
    String content_id = field_value(fields, "Content-ID");
    part.content_id = first_message_id(arena, content_id);
    if (part.content_id.len == 0) {
        part.content_id = content_id;
    }
    bool text = str_starts_with(type, S("text/"));
    bool shown_inline = str_equal_ignore_case(disposition, S("inline")) && part.filename.len > 0;
    part.attachment = str_equal_ignore_case(disposition, S("attachment")) || (!text && !shown_inline);
    if (text) {
        part.body = to_utf8(arena, part.body, charset_named(part.charset));
    }
    walk->parts = arena_grow(arena, walk->parts, &walk->capacity, walk->count, sizeof *walk->parts);
    walk->parts[walk->count++] = part;
}

static void walk_entity(Walk *walk, Fields fields, String body, unsigned depth);

// is_delimiter reports whether line is --boundary, with closing set when it is
// the closing --boundary--. Transport padding after it is allowed.
static bool is_delimiter(String line, String boundary, bool *closing)
{
    if (!str_starts_with(line, S("--")) || !str_starts_with(str_slice(line, 2, line.len), boundary)) {
        return false;
    }
    String rest = str_slice(line, boundary.len + 2, line.len);
    *closing = str_starts_with(rest, S("--"));
    return *closing || is_blank(rest);
}

static void walk_part(Walk *walk, String entity, unsigned depth)
{
    Fields fields = { 0 };
    String body = read_fields(walk->arena, entity, &fields);
    walk_entity(walk, fields, body, depth);
}

// walk_multipart walks each part of body between boundary delimiters and
// reports whether it found a delimiter. The line break before a delimiter is
// part of the delimiter.
static bool walk_multipart(Walk *walk, String body, String boundary, unsigned depth)
{
    bool found = false;
    bool in_part = false;
    size_t part_start = 0;
    size_t position = 0;
    while (position < body.len && walk->count < MAX_PARTS) {
        size_t line_start = position;
        bool closing;
        if (!is_delimiter(next_line(body, &position), boundary, &closing)) {
            continue;
        }
        found = true;
        if (in_part) {
            size_t end = line_start;
            if (end > part_start && body.data[end - 1] == '\n') {
                end--;
            }
            if (end > part_start && body.data[end - 1] == '\r') {
                end--;
            }
            walk_part(walk, str_slice(body, part_start, end), depth);
        }
        if (closing) {
            return true;
        }
        in_part = true;
        part_start = position;
    }
    if (in_part) {
        walk_part(walk, str_slice(body, part_start, body.len), depth);
    }
    return found;
}

static void walk_entity(Walk *walk, Fields fields, String body, unsigned depth)
{
    if (walk->count == MAX_PARTS) {
        return;
    }
    Parameters parameters = { 0 };
    String declared = read_parameters(walk->arena, field_value(fields, "Content-Type"), &parameters);
    String type = str_lower_ascii(walk->arena, declared); // modern-c: allow ascii-text
    if (str_starts_with(type, S("multipart/"))) {
        if (depth == MAX_DEPTH) {
            return;
        }
        String boundary = parameter_value(walk->arena, &parameters, "boundary");
        if (boundary.len > 0 && walk_multipart(walk, body, boundary, depth + 1)) {
            return;
        }
    }
    if (!is_mime_type(type) || str_starts_with(type, S("multipart/"))) {
        type = S("text/plain");
    }
    add_leaf(walk, fields, type, &parameters, body);
}

static size_t first_body(const MailPart *parts, size_t count, String type)
{
    for (size_t i = 0; i < count; i++) {
        if (!parts[i].attachment && str_equal(parts[i].content_type, type)) {
            return i;
        }
    }
    return SIZE_MAX;
}

ParsedMail mail_parse(Arena *arena, String raw)
{
    Fields fields = { 0 };
    String body = read_fields(arena, raw, &fields);
    StringList in_reply_to = message_ids(arena, field_value(fields, "In-Reply-To"));
    ParsedMail message = {
        .headers = fields.items,
        .header_count = fields.count,
        .subject = mail_decode_words(arena, field_value(fields, "Subject")),
        .from = read_addresses(arena, field_value(fields, "From")),
        .to = read_addresses(arena, field_value(fields, "To")),
        .cc = read_addresses(arena, field_value(fields, "Cc")),
        .bcc = read_addresses(arena, field_value(fields, "Bcc")),
        .reply_to = read_addresses(arena, field_value(fields, "Reply-To")),
        .date = read_date(field_value(fields, "Date")),
        .message_id = first_message_id(arena, field_value(fields, "Message-ID")),
        .in_reply_to = in_reply_to.count > 0 ? in_reply_to.items[0] : S(""),
        .references = message_ids(arena, field_value(fields, "References")),
    };
    Walk walk = { .arena = arena };
    walk_entity(&walk, fields, body, 0);
    message.parts = walk.parts;
    message.part_count = walk.count;
    message.text = first_body(walk.parts, walk.count, S("text/plain"));
    message.html = first_body(walk.parts, walk.count, S("text/html"));
    return message;
}
