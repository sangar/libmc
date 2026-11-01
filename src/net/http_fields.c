#include <assert.h>

#include "mc/net/http.h"
#include "mc/net/http_server.h"
#include "mc/text/fmt.h"

enum { SECONDS_PER_DAY = 86400 };

bool http_header_find(const HttpHeader *headers, size_t header_count, String name, String *value)
{
    for (size_t i = 0; i < header_count; i++) {
        if (str_equal_ignore_case(headers[i].name, name)) {
            *value = headers[i].value;
            return true;
        }
    }
    return false;
}

String http_date(Arena *arena, int64_t unix_nanoseconds)
{
    static const char *const DAYS[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
    static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int64_t seconds = unix_nanoseconds / NS_PER_SECOND - (unix_nanoseconds % NS_PER_SECOND < 0);
    int64_t days = seconds / SECONDS_PER_DAY - (seconds % SECONDS_PER_DAY < 0);
    int64_t time = seconds - days * SECONDS_PER_DAY;
    CivilDate civil = civil_from_days(days);
    return str_format(arena, "%s, %02u %s %04lld %02lld:%02lld:%02lld GMT", DAYS[(days % 7 + 7) % 7], civil.day,
                      MONTHS[civil.month - 1], (long long)civil.year, (long long)(time / 3600),
                      (long long)(time / 60 % 60), (long long)(time % 60));
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static String form_decode(Arena *arena, String s)
{
    StringBuilder out = str_builder_create(arena, s.len);
    for (size_t i = 0; i < s.len; i++) {
        int high = s.data[i] == '%' && i + 2 < s.len ? hex_value(s.data[i + 1]) : -1;
        int low = high >= 0 ? hex_value(s.data[i + 2]) : -1;
        if (low >= 0) {
            str_builder_append_char(&out, (char)(high * 16 + low));
            i += 2;
        } else {
            str_builder_append_char(&out, s.data[i] == '+' ? ' ' : s.data[i]);
        }
    }
    return str_builder_finish(&out);
}

bool http_query_param(Arena *arena, String query, String name, String *value)
{
    String rest = query;
    while (rest.len > 0) {
        String pair;
        str_cut(rest, '&', &pair, &rest);
        String key = pair;
        String encoded = S("");
        str_cut(pair, '=', &key, &encoded);
        if (str_equal(form_decode(arena, key), name)) {
            *value = form_decode(arena, encoded);
            return true;
        }
    }
    return false;
}

HttpRangeResult http_parse_range(String field, int64_t size, int64_t *first, int64_t *last)
{
    String spec = str_trim_prefix(str_trim(field), S("bytes="));
    String from;
    String to;
    if (spec.len == str_trim(field).len || !str_cut(spec, '-', &from, &to) || str_contains(spec, S(","))) {
        return HTTP_RANGE_NONE;
    }
    int64_t start = 0;
    int64_t end = 0;
    if (from.len == 0) {
        if (!str_parse_i64(to, &end) || end < 0) {
            return HTTP_RANGE_NONE;
        }
        if (end == 0 || size == 0) {
            return HTTP_RANGE_UNSATISFIABLE;
        }
        *first = end >= size ? 0 : size - end;
        *last = size - 1;
        return HTTP_RANGE_SATISFIABLE;
    }
    if (!str_parse_i64(from, &start) || start < 0 || (to.len > 0 && (!str_parse_i64(to, &end) || end < start))) {
        return HTTP_RANGE_NONE;
    }
    if (start >= size) {
        return HTTP_RANGE_UNSATISFIABLE;
    }
    *first = start;
    *last = to.len == 0 || end >= size ? size - 1 : end;
    return HTTP_RANGE_SATISFIABLE;
}

// language_weight reads the parameter of one Accept-Language element, "q="
// and a qvalue, in thousandths, and fails on anything else.
static bool language_weight(String parameter, unsigned *weight)
{
    if (parameter.len < 3 || (parameter.data[0] != 'q' && parameter.data[0] != 'Q') || parameter.data[1] != '=') {
        return false;
    }
    String value = str_slice(parameter, 2, parameter.len);
    char whole = value.data[0];
    if ((whole != '0' && whole != '1') || value.len > 5 || (value.len > 1 && value.data[1] != '.')) {
        return false;
    }
    unsigned thousandths = whole == '1' ? 1000 : 0;
    unsigned scale = 100;
    for (size_t i = 2; i < value.len; i++, scale /= 10) {
        char digit = value.data[i];
        if (digit < '0' || digit > '9' || (whole == '1' && digit != '0')) {
            return false;
        }
        thousandths += (unsigned)(digit - '0') * scale;
    }
    *weight = thousandths;
    return true;
}

static bool language_matches(String range, String tag)
{
    String shorter = range.len < tag.len ? range : tag;
    String longer = range.len < tag.len ? tag : range;
    return str_equal_ignore_case(str_slice(longer, 0, shorter.len), shorter) &&
           (longer.len == shorter.len || longer.data[shorter.len] == '-');
}

String http_negotiate_language(String accept, const String *offered, size_t count)
{
    assert(count > 0);
    String chosen = offered[0];
    unsigned chosen_weight = 0;
    String rest = accept;
    while (rest.len > 0) {
        String element;
        String parameter;
        str_cut(rest, ',', &element, &rest);
        bool weighted = str_cut(element, ';', &element, &parameter);
        String range = str_trim(element);
        unsigned weight = 1000;
        if (range.len == 0 || (weighted && !language_weight(str_trim(parameter), &weight)) ||
            weight <= chosen_weight) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            if ((i == 0 && str_equal(range, S("*"))) || language_matches(range, offered[i])) {
                chosen = offered[i];
                chosen_weight = weight;
                break;
            }
        }
    }
    return chosen;
}
