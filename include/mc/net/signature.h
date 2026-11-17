#pragma once

#include <stdint.h>

#include "mc/core/error.h"
#include "mc/crypto/sha256.h"
#include "mc/text/str.h"

// Public-key signatures checked on mbedTLS's PSA crypto, as DKIM checks the
// signature on a mail (RFC 6376).

// signature_verify_rsa_sha256 says in *valid whether signature is an
// RSASSA-PKCS1-v1_5 signature (RFC 8017 8.2) of digest, a SHA-256 hash, by
// public_key, and in *bits the key's size in bits, so that a caller can
// refuse short keys, as DKIM refuses those under 1024 bits (RFC 8301 3.2).
// public_key is DER, either a SubjectPublicKeyInfo or a bare PKCS #1
// RSAPublicKey (RFC 8017 A.1.1), since a DKIM key record holds either
// (RFC 6376 3.6.1). A key that does not parse, is not RSA, or is one mbedTLS
// cannot use, such as one over 4096 bits, is ERR_PARSE. A signature that does
// not verify is not an error: it is ERR_OK with *valid false.
[[nodiscard]] Error signature_verify_rsa_sha256(String public_key, const uint8_t digest[SHA256_LEN], String signature,
                                                size_t *bits, bool *valid, Err *err);
