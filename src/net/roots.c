#include "roots.h"

#include <curl/curl.h>

#include "cacert.h"
#include "mc/net/http.h"
#include "mc/platform/platform.h"

// The systems' bundles of trusted roots, in the order they are looked for:
// Debian and Ubuntu, Fedora and RHEL, then macOS and Alpine.
static const char *const CA_FILES[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
};

String http_default_ca_file(void)
{
    for (size_t i = 0; i < countof(CA_FILES); i++) {
        if (file_exists(S(CA_FILES[i]))) {
            return S(CA_FILES[i]);
        }
    }
    return (String){ 0 };
}

String roots_file(String ca_file)
{
    return ca_file.len > 0 ? ca_file : http_default_ca_file();
}

void roots_set(void *curl, const char *ca_file)
{
    if (ca_file != nullptr) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    } else {
        struct curl_blob roots = { .data = (void *)CACERT_PEM, .len = CACERT_PEM_SIZE, .flags = CURL_BLOB_NOCOPY };
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &roots);
    }
}
