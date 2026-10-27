#include "curl_global.h"

#include <curl/curl.h>

Error curl_global_start(const char *module, Err *err)
{
    // curl counts these calls and serialises them, so every client may make one.
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return err_set(err, ERR_PLATFORM, "%s: curl_global_init failed", module);
    }
    return ERR_OK;
}
