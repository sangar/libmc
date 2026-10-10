#include "test.h"

#include "mc/text/fmt.h"
#include "mc/text/glob.h"
#include "mc/text/path.h"
#include "mc/crypto/sha256.h"
#include "mc/crypto/sigv4.h"
#include "mc/text/table.h"
#include "mc/container/sort.h"
#include "mc/text/utf8.h"

static void utf8_roundtrip_and_validation(Test *test)
{
    char encoded[UTF8_MAX_BYTES];
    size_t length = utf8_encode(0xE5, encoded);
    size_t position = 0;
    uint32_t codepoint = 0;
    utf8_decode((String){ encoded, length }, &position, &codepoint);
    test_check(test, length == 2 && codepoint == 0xE5 && position == 2, "å encodes to two bytes and back");
    test_check(test, utf8_is_valid(S("Blåbærsyltetøy")), "valid utf-8 literal accepted");
    test_check(test, !utf8_is_valid((String){ "\xE5\x20", 2 }), "stray latin-1 byte rejected");
    test_check(test, !utf8_is_valid((String){ "\xC0\x80", 2 }), "overlong encoding rejected");
    test_check(test, utf8_count(S("blåbær")) == 6, "count counts code points");
    const uint32_t invalid[] = { 0xD800, 0xDFFF, 0x110000, UINT32_MAX };
    bool replaced = true;
    for (size_t i = 0; i < countof(invalid); i++) {
        length = utf8_encode(invalid[i], encoded);
        replaced = replaced && str_equal((String){ encoded, length }, S("\xEF\xBF\xBD"));
    }
    test_check(test, replaced, "surrogates and values past U+10FFFF encode as U+FFFD");
}

static void utf8_converts_latin1(Test *test)
{
    Arena *a = test->arena;
    test_check_str(test, utf8_from_latin1(a, (String){ "Bl\xE5" "b\xE6" "r", 6 }), S("Blåbær"),
                   "latin-1 letters convert");
    test_check_str(test, utf8_from_latin1(a, S("plain ascii")), S("plain ascii"), "ascii is unchanged");
    test_check(test, utf8_from_latin1(a, S("")).len == 0, "empty text stays empty");
    char every_byte[256];
    for (size_t i = 0; i < countof(every_byte); i++) {
        every_byte[i] = (char)i;
    }
    String converted = utf8_from_latin1(a, (String){ every_byte, countof(every_byte) });
    test_check(test, utf8_is_valid(converted) && utf8_count(converted) == 256 && converted.len == 384,
               "every byte becomes one code point");
    test_check(test, converted.data[converted.len] == '\0', "the result is NUL-terminated");
}

static void glob_matches_paths(Test *test)
{
    test_check(test, glob_match(S("*.tmp"), S("a.tmp")), "star matches within an element");
    test_check(test, !glob_match(S("*.tmp"), S("dir/a.tmp")), "star does not cross a slash");
    test_check(test, glob_match(S("**/.git"), S("/home/me/x/.git")), "leading ** matches any depth");
    test_check(test, glob_match(S("/a/**/c"), S("/a/c")), "inner ** matches no elements");
    test_check(test, glob_match(S("/a/**/c"), S("/a/b/b/c")), "inner ** matches several elements");
    test_check(test, glob_match(S("IMG_[0-9]???.JPG"), S("IMG_0001.JPG")), "class and question marks");
    test_check(test, !glob_match(S("[!a]*"), S("abc")), "negated class");
    test_check(test, glob_match(S("a\\*"), S("a*")) && !glob_match(S("a\\*"), S("ab")), "escaped star");
    test_check(test, glob_valid(S("a[bc]")) && !glob_valid(S("a[bc")) && !glob_valid(S("a\\")), "validity");
    test_check(test, !glob_valid(S("[a\\]")), "an escaped ] does not close a class");
    test_check(test, !glob_match(S("[a\\]"), S("a")) && glob_match(S("[a\\]"), S("[a]")),
               "an unclosed [ matches itself");
    test_check(test, glob_valid(S("[a\\]]")) && glob_match(S("[a\\]]"), S("]")) && glob_match(S("[a\\]]"), S("a")),
               "an escaped ] is a member");
    test_check(test, glob_valid(S("[\\\\]")) && glob_match(S("[\\\\]"), S("\\")), "an escaped backslash");
    test_check(test, glob_valid(S("[]a]")) && glob_match(S("[]a]"), S("]")), "a leading ] is a member");
    test_check(test, glob_valid(S("[!]a]")) && glob_match(S("[!]a]"), S("b")) && !glob_match(S("[!]a]"), S("]")),
               "a leading ] in a negated class");
    test_check(test, !glob_valid(S("[a\\")), "an unfinished escape in a class");
    String stars = str_concat(test->arena, str_repeat(test->arena, S("*a"), 24), S("b"));
    test_check(test, !glob_match(stars, str_repeat(test->arena, S("a"), 48)), "many stars fail fast");
    String depths = str_concat(test->arena, str_repeat(test->arena, S("**/a/"), 16), S("b"));
    test_check(test, !glob_match(depths, str_repeat(test->arena, S("a/"), 40)), "many ** elements fail fast");
    test_check(test, glob_match(S("**/a/**/b"), S("x/a/y/z/b")), "two ** elements match");
}

static void path_lexical_operations(Test *test)
{
    Arena *a = test->arena;
    test_check_str(test, path_clean(a, S("a//b/./c/../d/")), S("a/b/d"), "clean resolves . and ..");
    test_check_str(test, path_clean(a, S("/../x")), S("/x"), "clean stops at the root");
    test_check_str(test, path_clean(a, S("../../a")), S("../../a"), "clean keeps leading .. of a relative path");
    test_check_str(test, path_clean(a, S("")), S("."), "clean of empty is .");
    test_check_str(test, path_join(a, S("/usr/"), S("bin")), S("/usr/bin"), "join adds one slash");
    test_check_str(test, path_join(a, S("/"), S("etc")), S("/etc"), "join to the root");
    test_check_str(test, path_base(S("/a/b.txt/")), S("b.txt"), "base ignores trailing slashes");
    test_check_str(test, path_dir(S("/a/b.txt")), S("/a"), "dir");
    test_check_str(test, path_dir(S("b.txt")), S("."), "dir without a slash");
    test_check_str(test, path_ext(S("/a.d/archive.tar.gz")), S(".gz"), "ext of the last element");
    test_check_str(test, path_ext(S("/a.d/README")), S(""), "no ext");
    String relative;
    test_check(test, path_relative(S("/home/me/"), S("/home/me/x/y"), &relative), "relative below root");
    test_check_str(test, relative, S("x/y"), "relative path");
    test_check(test, !path_relative(S("/home/me"), S("/home/meme/x"), &relative), "a sibling is outside root");
    test_check(test, !path_relative(S("/home/me"), S("/home/me"), &relative), "root itself is not below root");
    test_check_str(test, path_expand_home(a, S("~/x"), S("/Users/me")), S("/Users/me/x"), "expand home");
}

static void fmt_durations(Test *test)
{
    Arena *a = test->arena;
    test_check_str(test, fmt_duration(a, 30LL * NS_PER_SECOND), S("30s"), "seconds");
    test_check_str(test, fmt_duration(a, 600LL * NS_PER_SECOND), S("10m0s"), "minutes");
    test_check_str(test, fmt_duration(a, 750LL * NS_PER_MILLISECOND), S("750ms"), "milliseconds");
    test_check_str(test, fmt_duration(a, 90500LL * NS_PER_MILLISECOND), S("1m30.5s"), "fraction of a second");
    test_check_str(test, fmt_duration(a, -3600LL * NS_PER_SECOND), S("-1h0m0s"), "negative hours");
    int64_t ns = 0;
    test_check(test, fmt_parse_duration(S("1m30s"), &ns) && ns == 90LL * NS_PER_SECOND, "parse 1m30s");
    test_check(test, fmt_parse_duration(S("1.5s"), &ns) && ns == 1500LL * NS_PER_MILLISECOND, "parse 1.5s");
    test_check(test, fmt_parse_duration(S("250ms"), &ns) && ns == 250LL * NS_PER_MILLISECOND, "ms is not m");
    test_check(test, !fmt_parse_duration(S("5"), &ns), "a number needs a unit");
    test_check(test, !fmt_parse_duration(S("5x"), &ns), "unknown unit");

    test_check(test, fmt_parse_duration(fmt_duration(a, INT64_MAX), &ns) && ns == INT64_MAX, "INT64_MAX round-trips");
    test_check(test, fmt_parse_duration(fmt_duration(a, INT64_MIN), &ns) && ns == INT64_MIN, "INT64_MIN round-trips");
    test_check(test, fmt_parse_duration(S("9007199254740993ns"), &ns) && ns == 9007199254740993LL, "2^53 + 1 is exact");
    test_check(test, fmt_parse_duration(S("9220000000000000000ns"), &ns) && ns == 9220000000000000000LL,
               "values near the limit");
    test_check(test, !fmt_parse_duration(S("9223372036854775808ns"), &ns), "one past INT64_MAX");
    test_check(test, !fmt_parse_duration(S("-9223372036854775809ns"), &ns), "one past INT64_MIN");
    test_check(test, !fmt_parse_duration(S("99999999999999999999h"), &ns), "a number past uint64_t");
    test_check(test, fmt_parse_duration(S("1.0000000009s"), &ns) && ns == NS_PER_SECOND, "fractions of a ns truncate");
    test_check(test, fmt_parse_duration(S("0.000000000017m"), &ns) && ns == 1, "every fraction digit counts");
    test_check(test, fmt_parse_duration(S("-1.5h"), &ns) && ns == -5400LL * NS_PER_SECOND, "negative fractional hours");
}

static void fmt_times_and_numbers(Test *test)
{
    Arena *a = test->arena;
    int64_t moment = 1710320400LL * NS_PER_SECOND + 123LL * NS_PER_MILLISECOND;
    test_check_str(test, fmt_rfc3339(a, moment, 0, false), S("2024-03-13T09:00:00Z"), "utc");
    test_check_str(test, fmt_rfc3339(a, moment, 3600, true), S("2024-03-13T10:00:00.123+01:00"), "offset, millis");
    test_check_str(test, fmt_rfc3339(a, -NS_PER_SECOND, -5400, false), S("1969-12-31T22:29:59-01:30"),
                   "before the epoch with a negative offset");
    test_check_str(test, fmt_thousands(a, -1234567), S("-1,234,567"), "thousands");
    test_check_str(test, fmt_thousands(a, 999), S("999"), "no separator below a thousand");
    test_check_str(test, fmt_bytes(a, 512), S("512 B"), "bytes");
    test_check_str(test, fmt_bytes(a, 1536 * 1024), S("1.5 MB"), "megabytes");
}

static void civil_dates_convert(Test *test)
{
    struct {
        CivilDate date;
        int64_t days;
    } cases[] = {
        { { 1970, 1, 1 }, 0 },          { { 1969, 12, 31 }, -1 },      { { 2000, 2, 29 }, 11016 },
        { { 2000, 3, 1 }, 11017 },      { { 2024, 2, 29 }, 19782 },    { { 1600, 3, 1 }, -135080 },
        { { 1, 1, 1 }, -719162 },       { { 9999, 12, 31 }, 2932896 },
    };
    for (size_t i = 0; i < countof(cases); i++) {
        CivilDate date = civil_from_days(cases[i].days);
        test_check(test, civil_to_days(cases[i].date) == cases[i].days, "civil_to_days counts days");
        test_check(test,
                   date.year == cases[i].date.year && date.month == cases[i].date.month &&
                       date.day == cases[i].date.day,
                   "civil_from_days finds the date");
    }
    bool round_trips = true;
    for (int64_t days = -1000000; days <= 1000000; days += 7) {
        round_trips = round_trips && civil_to_days(civil_from_days(days)) == days;
    }
    test_check(test, round_trips, "civil dates round-trip across four millennia either side of 1970");
}

static void sha256_known_vectors(Test *test)
{
    uint8_t digest[SHA256_LEN];
    sha256(S(""), digest);
    test_check_str(test, hex_encode(test->arena, digest, SHA256_LEN),
                   S("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "empty input");
    sha256(S("abc"), digest);
    test_check_str(test, hex_encode(test->arena, digest, SHA256_LEN),
                   S("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "abc");
    Sha256 hash;
    sha256_init(&hash);
    String million = str_repeat(test->arena, S("a"), 1000000);
    for (size_t at = 0; at < million.len; at += 997) {
        sha256_update(&hash, million.data + at, min_size(997, million.len - at));
    }
    sha256_final(&hash, digest);
    test_check_str(test, hex_encode(test->arena, digest, SHA256_LEN),
                   S("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
                   "a million a's in uneven pieces");
    hmac_sha256(S("Jefe"), S("what do ya want for nothing?"), digest);
    test_check_str(test, hex_encode(test->arena, digest, SHA256_LEN),
                   S("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "RFC 4231 HMAC case 2");
}

static int compare_ints(const void *context, const void *a, const void *b)
{
    unused(context);
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

typedef struct {
    int key;
    int order;
} Keyed;

static int compare_keys(const void *context, const void *a, const void *b)
{
    return compare_ints(context, &((const Keyed *)a)->key, &((const Keyed *)b)->key);
}

static void sort_orders_and_selects(Test *test)
{
    int *values = arena_push(test->arena, 1000 * sizeof *values);
    for (int i = 0; i < 1000; i++) {
        values[i] = (i * 7919) % 1000;
    }
    test_check(test, sort_top(test->arena, values, 1000, sizeof *values, 5, compare_ints, nullptr) == 5,
               "top returns k");
    test_check(test, values[0] == 0 && values[4] == 4, "top puts the smallest first, in order");
    sort_stable(test->arena, values, 1000, sizeof *values, compare_ints, nullptr);
    bool every_value_once = true;
    for (int i = 0; i < 1000; i++) {
        every_value_once = every_value_once && values[i] == i;
    }
    test_check(test, every_value_once, "top keeps every element and sort_stable orders them");

    Keyed *keyed = arena_push(test->arena, 100 * sizeof *keyed);
    for (int i = 0; i < 100; i++) {
        keyed[i] = (Keyed){ .key = i % 3, .order = i };
    }
    sort_stable(test->arena, keyed, 100, sizeof *keyed, compare_keys, nullptr);
    bool stable = true;
    for (int i = 1; i < 100; i++) {
        stable = stable && (keyed[i - 1].key < keyed[i].key || keyed[i - 1].order < keyed[i].order);
    }
    test_check(test, stable, "equal keys keep their order");
}

static void table_aligns_columns(Test *test)
{
    Table table = table_create(test->arena);
    table_row(&table, 3, (String[]){ S("NAME"), S("ROLE"), S("LAG") });
    table_row(&table, 3, (String[]){ S("blåbær"), S("leader"), S("0") });
    table_row(&table, 3, (String[]){ S("db-2"), S("replica"), S("12") });
    table_row(&table, 1, (String[]){ S("a single cell ends the block") });
    table_row(&table, 2, (String[]){ S("x"), S("y") });
    test_check_str(test, table_format(&table),
                   S("NAME    ROLE     LAG\n"
                     "blåbær  leader   0\n"
                     "db-2    replica  12\n"
                     "a single cell ends the block\n"
                     "x  y\n"),
                   "columns align by code points within each block");
}

static void sigv4_signs_like_aws(Test *test)
{
    Arena *a = test->arena;
    test_check_str(test, sigv4_date(a, 1440938160), S("20150830T123600Z"), "the X-Amz-Date form");
    SigV4Request list_users = {
        .method = S("GET"),
        .path = S("/"),
        .query = S("Action=ListUsers&Version=2010-05-08"),
        .host = S("iam.amazonaws.com"),
        .amz_date = S("20150830T123600Z"),
        .headers = strlist_of(a, 1, (const char *const[]){ "Content-Type: application/x-www-form-urlencoded; charset=utf-8" }),
        .payload_hash = sigv4_payload_hash(a, S("")),
    };
    SigV4Credentials example = {
        .access_key = S("AKIDEXAMPLE"),
        .secret_key = S("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"),
        .region = S("us-east-1"),
        .service = S("iam"),
    };
    test_check_str(test, sigv4_authorization(a, list_users, example),
                   S("AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/iam/aws4_request, "
                     "SignedHeaders=content-type;host;x-amz-date, "
                     "Signature=5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7"),
                   "the example from the AWS documentation");

    String body_hash = sigv4_payload_hash(a, S("hello"));
    SigV4Request put = {
        .method = S("PUT"),
        .path = S("/bucket/a%20b.txt"),
        .host = S("localhost:9000"),
        .amz_date = S("20261010T120000Z"),
        .headers = strlist_of(a, 3,
                              (const char *const[]){ str_cstr(a, str_concat(a, S("X-Amz-Content-SHA256:"), body_hash)),
                                                     "x-amz-meta-x: 1", "x-amz-meta:  a   b " }),
        .payload_hash = body_hash,
    };
    SigV4Credentials store = { S("access"), S("secret"), S("us-east-1"), S("s3") };
    test_check_str(test, sigv4_authorization(a, put, store),
                   S("AWS4-HMAC-SHA256 Credential=access/20261010/us-east-1/s3/aws4_request, "
                     "SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-meta;x-amz-meta-x, "
                     "Signature=9611270eeec626080b357f5fceed3809edef0fe6974d1912f4df91224ded51aa"),
                   "header names sort by name and values are trimmed");

    put.headers = strlist_of(a, 2, (const char *const[]){ "x-amz-meta-tag: a", "X-Amz-Meta-Tag: b" });
    String repeated = sigv4_authorization(a, put, store);
    put.headers = strlist_of(a, 1, (const char *const[]){ "x-amz-meta-tag: a,b" });
    test_check_str(test, repeated, sigv4_authorization(a, put, store), "a repeated header signs as one, values joined");
    test_check(test, str_contains(repeated, S("SignedHeaders=host;x-amz-date;x-amz-meta-tag,")),
               "a repeated header is signed once");
}

const TestCase TEXT_TESTS[] = {
    { "utf8_roundtrip_and_validation", utf8_roundtrip_and_validation },
    { "utf8_converts_latin1", utf8_converts_latin1 },
    { "glob_matches_paths", glob_matches_paths },
    { "path_lexical_operations", path_lexical_operations },
    { "fmt_durations", fmt_durations },
    { "fmt_times_and_numbers", fmt_times_and_numbers },
    { "civil_dates_convert", civil_dates_convert },
    { "sha256_known_vectors", sha256_known_vectors },
    { "sort_orders_and_selects", sort_orders_and_selects },
    { "table_aligns_columns", table_aligns_columns },
    { "sigv4_signs_like_aws", sigv4_signs_like_aws },
    { nullptr, nullptr },
};
