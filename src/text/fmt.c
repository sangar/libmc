#include "mc/text/fmt.h"

#include <assert.h>
#include <stdckdint.h>
#include <stdio.h>
#include <string.h>

enum { SECONDS_PER_DAY = 86400 };

static uint64_t magnitude(int64_t value)
{
    return value < 0 ? 0 - (uint64_t)value : (uint64_t)value;
}

// append_decimal writes whole.fraction with fraction's trailing zeros removed, as Go's Duration.String does.
static void append_decimal(StringBuilder *builder, uint64_t whole, uint64_t fraction, int digits)
{
    char text[32];
    snprintf(text, sizeof text, "%0*llu", digits, (unsigned long long)fraction);
    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == '0') {
        len--;
    }
    str_builder_append_format(builder, "%llu", (unsigned long long)whole);
    if (len > 0) {
        str_builder_append_format(builder, ".%.*s", (int)len, text);
    }
}

String fmt_duration(Arena *arena, int64_t nanoseconds)
{
    StringBuilder builder = str_builder_create(arena, 24);
    if (nanoseconds < 0) {
        str_builder_append_char(&builder, '-');
    }
    uint64_t ns = magnitude(nanoseconds);
    if (ns == 0) {
        str_builder_append(&builder, S("0s"));
    } else if (ns < NS_PER_MICROSECOND) {
        str_builder_append_format(&builder, "%lluns", (unsigned long long)ns);
    } else if (ns < NS_PER_MILLISECOND) {
        append_decimal(&builder, ns / NS_PER_MICROSECOND, ns % NS_PER_MICROSECOND, 3);
        str_builder_append(&builder, S("\xc2\xb5s"));
    } else if (ns < NS_PER_SECOND) {
        append_decimal(&builder, ns / NS_PER_MILLISECOND, ns % NS_PER_MILLISECOND, 6);
        str_builder_append(&builder, S("ms"));
    } else {
        uint64_t minute = 60ULL * NS_PER_SECOND;
        uint64_t hours = ns / (60 * minute);
        uint64_t minutes = ns / minute % 60;
        uint64_t seconds = ns % minute;
        if (hours > 0) {
            str_builder_append_format(&builder, "%lluh", (unsigned long long)hours);
        }
        if (hours > 0 || minutes > 0) {
            str_builder_append_format(&builder, "%llum", (unsigned long long)minutes);
        }
        append_decimal(&builder, seconds / NS_PER_SECOND, seconds % NS_PER_SECOND, 9);
        str_builder_append_char(&builder, 's');
    }
    return str_builder_finish(&builder);
}

// A DurationUnit is multiplier * 10^exponent nanoseconds.
typedef struct {
    const char *name;
    uint64_t multiplier;
    unsigned exponent;
} DurationUnit;

// Longer names come first where one is a prefix of another, so "ms" is not read as "m".
static const DurationUnit DURATION_UNITS[] = {
    { "ns", 1, 0 }, { "us", 1, 3 }, { "\xc2\xb5s", 1, 3 }, { "\xce\xbcs", 1, 3 },
    { "ms", 1, 6 }, { "s", 1, 9 },  { "m", 6, 10 },          { "h", 36, 11 },
};

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static String take_digits(String *text)
{
    size_t i = 0;
    while (i < text->len && is_digit(text->data[i])) {
        i++;
    }
    String digits = str_slice(*text, 0, i);
    *text = str_slice(*text, i, text->len);
    return digits;
}

static bool parse_whole(String digits, uint64_t *value)
{
    *value = 0;
    for (size_t i = 0; i < digits.len; i++) {
        if (ckd_mul(value, *value, 10) || ckd_add(value, *value, (uint64_t)(digits.data[i] - '0'))) {
            return false;
        }
    }
    return true;
}

// fraction_nanoseconds is the whole nanoseconds in 0.<digits> of unit, rounded toward zero.
// Digits within the unit's power of ten scale exactly; the rest are multiplied
// out right to left so that every digit counts toward the carry.
static uint64_t fraction_nanoseconds(String digits, DurationUnit unit)
{
    uint64_t scaled = 0;
    for (size_t i = 0; i < unit.exponent; i++) {
        scaled = scaled * 10 + (i < digits.len ? (uint64_t)(digits.data[i] - '0') : 0);
    }
    uint64_t carry = 0;
    for (size_t i = digits.len; i-- > unit.exponent;) {
        carry = ((uint64_t)(digits.data[i] - '0') * unit.multiplier + carry) / 10;
    }
    return scaled * unit.multiplier + carry;
}

static uint64_t power_of_ten(unsigned exponent)
{
    uint64_t result = 1;
    for (unsigned i = 0; i < exponent; i++) {
        result *= 10;
    }
    return result;
}

// parse_component reads one number and unit, such as 1.5h, from the front of *text.
static bool parse_component(String *text, uint64_t *nanoseconds)
{
    String whole_digits = take_digits(text);
    String fraction_digits = { 0 };
    if (str_starts_with(*text, S("."))) {
        *text = str_slice(*text, 1, text->len);
        fraction_digits = take_digits(text);
    }
    uint64_t whole;
    if (whole_digits.len + fraction_digits.len == 0 || !parse_whole(whole_digits, &whole)) {
        return false;
    }
    size_t index = 0;
    while (index < countof(DURATION_UNITS) && !str_starts_with(*text, S(DURATION_UNITS[index].name))) {
        index++;
    }
    if (index == countof(DURATION_UNITS)) {
        return false;
    }
    DurationUnit unit = DURATION_UNITS[index];
    *text = str_slice(*text, strlen(unit.name), text->len);
    uint64_t unit_ns = unit.multiplier * power_of_ten(unit.exponent);
    return !ckd_mul(nanoseconds, whole, unit_ns) &&
           !ckd_add(nanoseconds, *nanoseconds, fraction_nanoseconds(fraction_digits, unit));
}

bool fmt_parse_duration(String text, int64_t *nanoseconds)
{
    bool negative = str_starts_with(text, S("-"));
    if (negative || str_starts_with(text, S("+"))) {
        text = str_slice(text, 1, text.len);
    }
    if (str_equal(text, S("0"))) {
        *nanoseconds = 0;
        return true;
    }
    if (text.len == 0) {
        return false;
    }
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t total = 0;
    while (text.len > 0) {
        uint64_t component;
        if (!parse_component(&text, &component) || ckd_add(&total, total, component) || total > limit) {
            return false;
        }
    }
    if (negative) {
        *nanoseconds = total == limit ? INT64_MIN : -(int64_t)total;
    } else {
        *nanoseconds = (int64_t)total;
    }
    return true;
}

CivilDate civil_from_days(int64_t days)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned day_of_era = (unsigned)(days - era * 146097);
    unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    unsigned shifted_month = (5 * day_of_year + 2) / 153;
    unsigned month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    return (CivilDate){
        .year = (int64_t)year_of_era + era * 400 + (month <= 2),
        .month = month,
        .day = day_of_year - (153 * shifted_month + 2) / 5 + 1,
    };
}

int64_t civil_to_days(CivilDate date)
{
    assert(date.month >= 1 && date.month <= 12 && date.day >= 1 && date.day <= 31);
    int64_t year = date.year - (date.month <= 2);
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    unsigned shifted_month = date.month > 2 ? date.month - 3 : date.month + 9;
    unsigned day_of_year = (153 * shifted_month + 2) / 5 + date.day - 1;
    unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + (int64_t)day_of_era - 719468;
}

static int64_t floor_divide(int64_t value, int64_t divisor, int64_t *remainder)
{
    int64_t quotient = value / divisor;
    *remainder = value % divisor;
    if (*remainder < 0) {
        quotient--;
        *remainder += divisor;
    }
    return quotient;
}

String fmt_rfc3339(Arena *arena, int64_t unix_nanoseconds, int32_t utc_offset, bool milliseconds)
{
    int64_t fraction;
    int64_t seconds = floor_divide(unix_nanoseconds, NS_PER_SECOND, &fraction) + utc_offset;
    int64_t second_of_day;
    CivilDate date = civil_from_days(floor_divide(seconds, SECONDS_PER_DAY, &second_of_day));

    StringBuilder builder = str_builder_create(arena, 32);
    str_builder_append_format(&builder, "%04lld-%02u-%02uT%02lld:%02lld:%02lld", (long long)date.year, date.month,
                              date.day, (long long)(second_of_day / 3600), (long long)(second_of_day / 60 % 60),
                              (long long)(second_of_day % 60));
    if (milliseconds) {
        str_builder_append_format(&builder, ".%03lld", (long long)(fraction / NS_PER_MILLISECOND));
    }
    if (utc_offset == 0) {
        str_builder_append_char(&builder, 'Z');
    } else {
        uint64_t offset = magnitude(utc_offset);
        str_builder_append_format(&builder, "%c%02llu:%02llu", utc_offset < 0 ? '-' : '+',
                                  (unsigned long long)(offset / 3600), (unsigned long long)(offset / 60 % 60));
    }
    return str_builder_finish(&builder);
}

String fmt_thousands(Arena *arena, int64_t value)
{
    char digits[32];
    int len = snprintf(digits, sizeof digits, "%llu", (unsigned long long)magnitude(value));
    StringBuilder builder = str_builder_create(arena, 32);
    if (value < 0) {
        str_builder_append_char(&builder, '-');
    }
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            str_builder_append_char(&builder, ',');
        }
        str_builder_append_char(&builder, digits[i]);
    }
    return str_builder_finish(&builder);
}

String fmt_bytes(Arena *arena, int64_t bytes)
{
    if (bytes < 1024) {
        return str_format(arena, "%lld B", (long long)bytes);
    }
    static const char *const units[] = { "KB", "MB", "GB", "TB", "PB", "EB" };
    double scaled = (double)bytes;
    size_t unit = 0;
    for (scaled /= 1024; scaled >= 1024 && unit + 1 < countof(units); scaled /= 1024) {
        unit++;
    }
    if (scaled < 10) {
        return str_format(arena, "%.1f %s", scaled, units[unit]);
    }
    return str_format(arena, "%.0f %s", scaled, units[unit]);
}
