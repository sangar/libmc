#include "mc/log/log.h"

#include "mc/encoding/json.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"

enum { LINE_ARENA_BLOCK = 4096 };

struct Logger {
    Arena *arena; // holds the logger, then each line while it is written
    ArenaMark line_start;
    Mutex mutex;
    LogLevel level;
    LogFormat format;
    FILE *out;
};

static const char *const LEVEL_NAMES[] = { "DEBUG", "INFO", "WARN", "ERROR" };

LogAttr log_string(const char *key, String value)
{
    return (LogAttr){ .key = key, .kind = LOG_ATTR_STRING, .text = value };
}

LogAttr log_int(const char *key, int64_t value)
{
    return (LogAttr){ .key = key, .kind = LOG_ATTR_INT, .number = value };
}

LogAttr log_duration(const char *key, int64_t nanoseconds)
{
    return (LogAttr){ .key = key, .kind = LOG_ATTR_DURATION, .number = nanoseconds };
}

LogAttr log_err(const Err *err)
{
    return log_string("err", S(err->msg));
}

Logger *logger_create(LogLevel level, LogFormat format, FILE *out)
{
    Arena *arena = arena_create(LINE_ARENA_BLOCK);
    Logger *logger = arena_push(arena, sizeof *logger);
    *logger = (Logger){ .arena = arena, .level = level, .format = format, .out = out };
    logger->line_start = arena_mark(arena);
    mutex_init(&logger->mutex);
    return logger;
}

void logger_destroy(Logger *logger)
{
    if (logger != nullptr) {
        mutex_destroy(&logger->mutex);
        arena_destroy(logger->arena);
    }
}

bool log_parse_level(String name, LogLevel *level)
{
    for (size_t i = 0; i < countof(LEVEL_NAMES); i++) {
        if (str_equal_ignore_case(name, S(LEVEL_NAMES[i]))) {
            *level = (LogLevel)i;
            return true;
        }
    }
    return false;
}

bool log_parse_format(String name, LogFormat *format)
{
    if (str_equal(name, S("text"))) {
        *format = LOG_TEXT;
        return true;
    }
    if (str_equal(name, S("json"))) {
        *format = LOG_JSON;
        return true;
    }
    return false;
}

static String attr_text(Arena *arena, const LogAttr *attr)
{
    switch (attr->kind) {
    case LOG_ATTR_STRING: return attr->text;
    case LOG_ATTR_INT: return str_format(arena, "%lld", (long long)attr->number);
    case LOG_ATTR_DURATION: return fmt_duration(arena, attr->number);
    }
    return S("");
}

// Text keys and values are quoted only when a reader could not otherwise tell where they end.
static void append_text(StringBuilder *line, String text)
{
    bool plain = text.len > 0;
    for (size_t i = 0; plain && i < text.len; i++) {
        unsigned char c = (unsigned char)text.data[i];
        plain = c > ' ' && c != '"' && c != '=' && c != '\\' && c != 0x7f;
    }
    if (plain) {
        str_builder_append(line, text);
    } else {
        json_append_quoted(line, text);
    }
}

static void format_text(StringBuilder *line, String time, LogLevel level, const char *message, size_t count,
                        const LogAttr *attrs)
{
    str_builder_append_format(line, "time=%.*s level=%s msg=", (int)time.len, time.data, LEVEL_NAMES[level]);
    append_text(line, S(message));
    for (size_t i = 0; i < count; i++) {
        str_builder_append_char(line, ' ');
        append_text(line, S(attrs[i].key));
        str_builder_append_char(line, '=');
        append_text(line, attr_text(line->arena, &attrs[i]));
    }
}

static void format_json(StringBuilder *line, String time, LogLevel level, const char *message, size_t count,
                        const LogAttr *attrs)
{
    str_builder_append_format(line, "{\"time\":\"%.*s\",\"level\":\"%s\",\"msg\":", (int)time.len, time.data,
                              LEVEL_NAMES[level]);
    json_append_quoted(line, S(message));
    for (size_t i = 0; i < count; i++) {
        str_builder_append_char(line, ',');
        json_append_quoted(line, S(attrs[i].key));
        str_builder_append_char(line, ':');
        if (attrs[i].kind == LOG_ATTR_INT) {
            str_builder_append(line, attr_text(line->arena, &attrs[i]));
        } else {
            json_append_quoted(line, attr_text(line->arena, &attrs[i]));
        }
    }
    str_builder_append_char(line, '}');
}

static void log_write(Logger *logger, LogLevel level, const char *message, size_t count, const LogAttr *attrs)
{
    if (level < logger->level) {
        return;
    }
    int64_t now = clock_wall_ns();
    mutex_lock(&logger->mutex);
    String time = fmt_rfc3339(logger->arena, now, clock_local_offset(now / NS_PER_SECOND), true);
    StringBuilder line = str_builder_create(logger->arena, 256);
    if (logger->format == LOG_JSON) {
        format_json(&line, time, level, message, count, attrs);
    } else {
        format_text(&line, time, level, message, count, attrs);
    }
    str_builder_append_char(&line, '\n');
    String text = str_builder_finish(&line);
    fwrite(text.data, 1, text.len, logger->out);
    fflush(logger->out);
    arena_release(logger->line_start);
    mutex_unlock(&logger->mutex);
}

void log_debug(Logger *logger, const char *message, size_t count, const LogAttr *attrs)
{
    log_write(logger, LOG_DEBUG, message, count, attrs);
}

void log_info(Logger *logger, const char *message, size_t count, const LogAttr *attrs)
{
    log_write(logger, LOG_INFO, message, count, attrs);
}

void log_warn(Logger *logger, const char *message, size_t count, const LogAttr *attrs)
{
    log_write(logger, LOG_WARN, message, count, attrs);
}

void log_error(Logger *logger, const char *message, size_t count, const LogAttr *attrs)
{
    log_write(logger, LOG_ERROR, message, count, attrs);
}
