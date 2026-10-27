#pragma once

#include "mc/core/error.h"

// curl_global_start readies curl for the process, as every client does
// before its first handle; module names the caller in the error. It has no
// counterpart: curl's teardown also frees mbedTLS's process-wide crypto
// state, where others in the process, such as a TLS server, still hold keys.
[[nodiscard]] Error curl_global_start(const char *module, Err *err);
