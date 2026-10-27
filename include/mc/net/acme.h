#pragma once

#include <stddef.h>

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/log/log.h"
#include "mc/net/http_server.h"
#include "mc/text/str.h"
#include "mc/net/tls.h"

// An ACME client (RFC 8555) that gets certificates for a server's domains
// from a CA such as Let's Encrypt, proving control of each domain with the
// HTTP-01 challenge on port 80 or the TLS-ALPN-01 challenge on port 443, and
// renews them while the server runs.

typedef struct AcmeConfig {
    String directory_url; // empty is Let's Encrypt's production directory
    // ca_file is a PEM bundle of the roots the directory's TLS certificate is
    // checked against; empty uses the system's.
    String ca_file;
    // storage_dir keeps the account key and the certificate between runs. It
    // is created with mode 0700 when missing; processes may share it.
    String storage_dir;
    const String *domains;
    size_t domain_count;
    Logger *logger; // renewals and their failures; nullptr logs nothing
    // challenge_tls, when set, answers TLS-ALPN-01 challenges (RFC 8737) and
    // must serve port 443 and offer acme-tls/1, as it does unless told
    // otherwise, for setups that cannot open port 80; otherwise the
    // challenges are HTTP-01, which acme_http_handler answers on port 80.
    TlsServer *challenge_tls;
} AcmeConfig;

typedef struct Acme Acme;

// acme_create copies config, and reads the account key from storage_dir or
// makes and stores one. The caller owns the client.
[[nodiscard]] Error acme_create(const AcmeConfig *config, Acme **acme, Err *err);
// acme_destroy stops the renewals acme_keep_renewed started.
void acme_destroy(Acme *acme);

// acme_certificate gives the stored certificate while it names every domain
// and less than two thirds of its lifetime have passed. Otherwise it orders a
// new one, waiting while the CA checks the challenges, and stores it. *chain_pem is the certificate and its
// intermediates, *key_pem its key, both in arena. It locks storage_dir while
// it works, so processes sharing it order once. A refused order or challenge
// is ERR_NETWORK with the CA's reason in err.
[[nodiscard]] Error acme_certificate(Acme *acme, Arena *arena, String *chain_pem, String *key_pem, Err *err);

// acme_keep_renewed renews the certificate tls presents, on a thread of its
// own, until acme_destroy: when acme_certificate gives a new one, tls
// presents it from the next handshake on. A failed renewal is logged and
// tried again a minute later. tls must outlive acme.
[[nodiscard]] Error acme_keep_renewed(Acme *acme, TlsServer *tls, Err *err);

// acme_http_handler serves port 80 with context an Acme: it answers HTTP-01
// challenges under /.well-known/acme-challenge/, redirects requests for the
// domains to https with 301, and refuses any other host with 421.
[[nodiscard]] Error acme_http_handler(void *context, Arena *arena, const HttpServerRequest *request,
                                      HttpServerResponse *response, Err *err);
