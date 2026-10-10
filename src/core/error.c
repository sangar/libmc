#include "mc/core/error.h"

#include "mc/core/base.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *error_name(Error error)
{
    switch (error) {
    case ERR_OK: return "ok";
    case ERR_INVALID_ARGUMENT: return "invalid argument";
    case ERR_OUT_OF_MEMORY: return "out of memory";
    case ERR_NOT_FOUND: return "not found";
    case ERR_IO: return "io error";
    case ERR_PLATFORM: return "platform error";
    case ERR_PARSE: return "parse error";
    case ERR_UNSUPPORTED: return "unsupported";
    case ERR_TIMEOUT: return "timeout";
    case ERR_CANCELLED: return "cancelled";
    case ERR_NETWORK: return "network error";
    }
    return "unknown error";
}

Error err_set(Err *err, Error code, const char *format, ...)
{
    if (err == nullptr) {
        return code;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(err->msg, sizeof err->msg, format, args);
    va_end(args);
    return code;
}

// append copies as much of text as fits after msg[0..at), keeps the terminator, and returns the new length.
static size_t append(Err *err, size_t at, const char *text)
{
    size_t room = sizeof err->msg - 1 - at;
    size_t len = min_size(strlen(text), room);
    memcpy(err->msg + at, text, len);
    err->msg[at + len] = '\0';
    return at + len;
}

Error err_wrap(Err *err, Error code, const char *format, ...)
{
    if (err == nullptr) {
        return code;
    }
    char previous[sizeof err->msg];
    memcpy(previous, err->msg, sizeof previous);
    va_list args;
    va_start(args, format);
    int written = vsnprintf(err->msg, sizeof err->msg, format, args);
    va_end(args);
    size_t at = written < 0 ? 0 : min_size((size_t)written, sizeof err->msg - 1);
    if (previous[0] != '\0') {
        append(err, append(err, at, ": "), previous);
    }
    return code;
}
