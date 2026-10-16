#include "test.h"

#include <stdio.h>

#include "mc/log/log.h"
#include "mc/log/logfile.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

// logged writes through a logger into a temporary file and returns what it holds.
static String logged(Test *test, LogLevel level, LogFormat format, void (*write)(Logger *logger))
{
    FILE *file = tmpfile();
    if (file == nullptr) {
        return S("");
    }
    Logger *logger = logger_create(level, format, file);
    write(logger);
    logger_destroy(logger);
    long size = ftell(file);
    rewind(file);
    char *text = arena_push(test->arena, (size_t)size + 1);
    size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);
    return (String){ text, read };
}

static void write_lines(Logger *logger)
{
    Err err = { 0 };
    unused(err_set(&err, ERR_IO, "disk full"));
    const LogAttr attrs[] = {
        log_string("path", S("a b")), log_int("bytes", 42), log_duration("took", 1500 * NS_PER_MILLISECOND), log_err(&err),
    };
    log_debug(logger, "hidden", 0, nullptr);
    log_info(logger, "synced", countof(attrs), attrs);
    log_warn(logger, "say \"hi\"", 0, nullptr);
}

static void logger_writes_text(Test *test)
{
    StringList lines = strlist_from_lines(test->arena, logged(test, LOG_INFO, LOG_TEXT, write_lines));
    test_check(test, lines.count == 3 && lines.items[2].len == 0, "debug is dropped below info");
    String first = lines.count > 0 ? lines.items[0] : S("");
    size_t level_at = 0;
    test_check(test, str_starts_with(first, S("time=")) && str_find(first, S(" level="), &level_at),
               "the line starts with the time");
    test_check_str(test, str_slice(first, level_at + 1, first.len),
                   S("level=INFO msg=synced path=\"a b\" bytes=42 took=1.5s err=\"disk full\""),
                   "values with spaces are quoted");
    String second = lines.count > 1 ? lines.items[1] : S("");
    test_check(test, str_ends_with(second, S("level=WARN msg=\"say \\\"hi\\\"\"")), "quotes are escaped");
}

static void write_odd_keys(Logger *logger)
{
    const LogAttr attrs[] = {
        log_string("display\nname", S("Alice")), log_string("first name", S("Bob")),
        log_string("a=b", S("c")), log_string("say \"hi\"", S("d")),
    };
    log_info(logger, "hello", countof(attrs), attrs);
}

static void logger_quotes_text_keys(Test *test)
{
    StringList lines = strlist_from_lines(test->arena, logged(test, LOG_INFO, LOG_TEXT, write_odd_keys));
    test_check(test, lines.count == 2 && lines.items[1].len == 0, "one line per call");
    String line = lines.count > 0 ? lines.items[0] : S("");
    test_check(test,
               str_ends_with(line, S(" msg=hello \"display\\nname\"=Alice \"first name\"=Bob \"a=b\"=c "
                                     "\"say \\\"hi\\\"\"=d")),
               "keys are quoted like values");
}

static void logger_writes_json(Test *test)
{
    StringList lines = strlist_from_lines(test->arena, logged(test, LOG_DEBUG, LOG_JSON, write_lines));
    test_check(test, lines.count == 4, "debug is written at debug level");
    String info = lines.count > 1 ? lines.items[1] : S("");
    test_check(test, str_starts_with(info, S("{\"time\":\"")), "the object starts with the time");
    test_check(test,
               str_ends_with(info, S("\"level\":\"INFO\",\"msg\":\"synced\",\"path\":\"a b\",\"bytes\":42,"
                                     "\"took\":\"1.5s\",\"err\":\"disk full\"}")),
               "integers stay numbers");
}

static void logger_parses_settings(Test *test)
{
    LogLevel level = LOG_INFO;
    LogFormat format = LOG_TEXT;
    test_check(test, log_parse_level(S("warn"), &level) && level == LOG_WARN, "a level by name");
    test_check(test, !log_parse_level(S("loud"), &level) && level == LOG_WARN, "an unknown level");
    test_check(test, log_parse_format(S("json"), &format) && format == LOG_JSON, "a format by name");
    test_check(test, !log_parse_format(S("xml"), &format), "an unknown format");
}

static void logfile_rotates_at_its_limit(Test *test)
{
    Arena *a = test->arena;
    String dir;
    test_check(test, dir_create_temp(a, S("mc-test-"), &dir, nullptr) == ERR_OK, "a temp dir");
    String path = path_join(a, dir, S("logs/app.log"));
    // Each line takes more than half the limit, whatever the zone makes its time.
    String padding = str_repeat(a, S("."), 60);
    LogFile *file = logfile_create(path, 120);
    Err err = { 0 };
    test_check(test, logfile_write(file, a, str_concat(a, S("first"), padding), clock_wall_ns(), &err) == ERR_OK,
               err.msg);
    test_check(test, logfile_write(file, a, str_concat(a, S("second"), padding), clock_wall_ns(), &err) == ERR_OK,
               "a line past the limit");
    logfile_destroy(file);
    String old = S("");
    String now = S("");
    test_check(test, file_read_all(a, str_concat(a, path, S(".1")), &old, nullptr) == ERR_OK, "the old file moved aside");
    test_check(test, file_read_all(a, path, &now, nullptr) == ERR_OK, "a new one started, directory and all");
    test_check(test, str_contains(old, S(" first.")) && !str_contains(old, S("second")), "the oldest line moved");
    test_check(test, str_contains(now, S(" second.")) && !str_contains(now, S("first")), "the newest stayed");
    test_check(test, str_starts_with(now, S("20")) && str_contains(now, S("T")), "each line after its time");
    file = logfile_create(path, 1000);
    test_check(test, logfile_write(file, a, S("third"), clock_wall_ns(), &err) == ERR_OK, "open it again");
    logfile_destroy(file);
    test_check(test, file_read_all(a, path, &now, nullptr) == ERR_OK && str_contains(now, S(" second.")) &&
                         str_ends_with(now, S(" third\n")),
               "it appends to what is there");
    test_check(test, dir_remove_all(dir, nullptr) == ERR_OK, "clean up");
}

const TestCase LOG_TESTS[] = {
    { "logfile_rotates_at_its_limit", logfile_rotates_at_its_limit },
    { "logger_writes_text", logger_writes_text },
    { "logger_quotes_text_keys", logger_quotes_text_keys },
    { "logger_writes_json", logger_writes_json },
    { "logger_parses_settings", logger_parses_settings },
    { nullptr, nullptr },
};
