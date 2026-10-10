#pragma once

// Error is the one result type for everything that can fail. A fallible
// function returns it and hands results back through out-parameters.
typedef enum Error {
    ERR_OK = 0,
    ERR_INVALID_ARGUMENT,
    ERR_OUT_OF_MEMORY,
    ERR_NOT_FOUND,
    ERR_IO,
    ERR_PLATFORM,
    ERR_PARSE,
    ERR_UNSUPPORTED,
    ERR_TIMEOUT,
    ERR_CANCELLED,
    ERR_NETWORK,
} Error;

// Err receives the human-readable detail of a failure. Fallible functions
// take it as their last parameter; nullptr discards the detail.
typedef struct Err {
    char msg[512];
} Err;

const char *error_name(Error error);

// err_set records the detail when err is given and returns code, so a
// caller can write `return err_set(err, ERR_IO, ...)`.
[[nodiscard]] [[gnu::format(printf, 3, 4)]] Error err_set(Err *err, Error code, const char *format, ...);

// err_wrap prefixes the detail already in err: "<format>: <previous detail>".
[[nodiscard]] [[gnu::format(printf, 3, 4)]] Error err_wrap(Err *err, Error code, const char *format, ...);
