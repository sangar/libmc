#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mc/net/tls.h"

// What ACME needs from TLS: a server answering TLS-ALPN-01 challenges, and
// the ECDSA P-256 keys that sign requests and that certificates are for.

// tls_server_add_challenge answers a TLS-ALPN-01 challenge (RFC 8737) for
// host until tls_server_clear_challenges: a handshake for host that
// negotiates acme-tls/1, as the CA's does, gets a self-signed certificate
// carrying the SHA-256 of key_authorization, and the connection closes after
// it. A handshake for acme-tls/1 with no challenge for its name fails.
[[nodiscard]] Error tls_server_add_challenge(TlsServer *server, String host, String key_authorization, Err *err);
void tls_server_clear_challenges(TlsServer *server);

typedef struct TlsKey TlsKey;

// tls_key_generate makes a P-256 key, as PEM in arena.
[[nodiscard]] Error tls_key_generate(Arena *arena, String *pem, Err *err);

// tls_key_load reads a PEM P-256 private key. The caller owns the key.
[[nodiscard]] Error tls_key_load(String pem, TlsKey **key, Err *err);
void tls_key_destroy(TlsKey *key);

// tls_key_public_point is the public key's coordinates, 32 bytes each, as a
// JWK holds them, in arena.
[[nodiscard]] Error tls_key_public_point(Arena *arena, const TlsKey *key, String *x, String *y, Err *err);

// tls_key_sign signs the SHA-256 of data with ECDSA as JWS's ES256 writes it:
// r and s, 32 bytes each, in arena.
[[nodiscard]] Error tls_key_sign(Arena *arena, TlsKey *key, String data, String *signature, Err *err);

// tls_key_request is a DER certificate signing request for names, the first
// also the subject's common name, signed by key, in arena.
[[nodiscard]] Error tls_key_request(Arena *arena, TlsKey *key, const String *names, size_t name_count, String *der,
                                    Err *err);

// tls_certificate_matches reports whether the first certificate in
// chain_pem holds key's public key and names host among its DNS names.
bool tls_certificate_matches(String chain_pem, const TlsKey *key, String host);

// tls_certificate_validity reads when the first certificate in chain_pem
// starts and stops being valid, as Unix seconds.
[[nodiscard]] Error tls_certificate_validity(String chain_pem, int64_t *not_before, int64_t *not_after, Err *err);
