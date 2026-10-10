#pragma once

#include <stdio.h>

#include "mc/core/error.h"
#include "mc/text/str.h"

// Logger writes one structured line per call, as Go's slog does: the time,
// level and message, then key=value attributes, as text or as JSON. Threads
// may share one logger.
typedef struct Logger Logger;

typedef enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;
typedef enum LogFormat { LOG_TEXT, LOG_JSON } LogFormat;

typedef enum LogAttrKind { LOG_ATTR_STRING, LOG_ATTR_INT, LOG_ATTR_DURATION } LogAttrKind;

typedef struct LogAttr {
    const char *key;
    LogAttrKind kind;
    String text;
    int64_t number;
} LogAttr;

LogAttr log_string(const char *key, String value);
LogAttr log_int(const char *key, int64_t value);
// log_duration writes nanoseconds in Go's notation, such as 1.5s.
LogAttr log_duration(const char *key, int64_t nanoseconds);
// log_err is the attribute err=<detail>.
LogAttr log_err(const Err *err);

// logger_create writes to out, which stays the caller's to close, and drops
// lines below level.
Logger *logger_create(LogLevel level, LogFormat format, FILE *out);
void logger_destroy(Logger *logger);

// log_parse_level reads debug, info, warn or error; log_parse_format reads text or json.
bool log_parse_level(String name, LogLevel *level);
bool log_parse_format(String name, LogFormat *format);

// Each writes message with count attributes from attrs, which may be nullptr when count is 0.
void log_debug(Logger *logger, const char *message, size_t count, const LogAttr *attrs);
void log_info(Logger *logger, const char *message, size_t count, const LogAttr *attrs);
void log_warn(Logger *logger, const char *message, size_t count, const LogAttr *attrs);
void log_error(Logger *logger, const char *message, size_t count, const LogAttr *attrs);
