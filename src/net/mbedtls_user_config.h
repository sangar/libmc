#pragma once

// libmc's changes to TF-PSA-Crypto's default configuration, read through
// TF_PSA_CRYPTO_USER_CONFIG_FILE by mbedTLS, curl and src/net alike so that
// every translation unit sees the same structures.

// PSA crypto keeps process-wide state, its random generator and key store,
// and HTTP clients run on several threads at once.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

// The self-tests are never run, and the counter ECP keeps for them is
// written by every handshake on any thread without a lock.
#undef MBEDTLS_SELF_TEST
