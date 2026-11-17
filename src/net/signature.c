#include "mc/net/signature.h"

#include <mbedtls/pk.h>
#include <psa/crypto.h>

[[nodiscard]] static Error verify_hash(const mbedtls_pk_context *key, const uint8_t digest[SHA256_LEN], String signature,
                                       bool *valid, Err *err)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    int status = mbedtls_pk_get_psa_attributes(key, PSA_KEY_USAGE_VERIFY_HASH, &attributes);
    status = status != 0 ? status : mbedtls_pk_import_into_psa(key, &attributes, &id);
    psa_reset_key_attributes(&attributes);
    if (status != 0) {
        return err_set(err, ERR_PLATFORM, "signature: cannot import the key: -0x%04x", (unsigned)-status);
    }
    psa_status_t verified = psa_verify_hash(id, PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_256), digest, SHA256_LEN,
                                            (const uint8_t *)signature.data, signature.len);
    psa_destroy_key(id);
    if (verified != PSA_SUCCESS && verified != PSA_ERROR_INVALID_SIGNATURE) {
        return err_set(err, ERR_PLATFORM, "signature: cannot verify: PSA status %d", (int)verified);
    }
    *valid = verified == PSA_SUCCESS;
    return ERR_OK;
}

Error signature_verify_rsa_sha256(String public_key, const uint8_t digest[SHA256_LEN], String signature, size_t *bits,
                                  bool *valid, Err *err)
{
    *bits = 0;
    *valid = false;
    if (psa_crypto_init() != PSA_SUCCESS) {
        return err_set(err, ERR_PLATFORM, "signature: cannot start PSA crypto");
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int status = mbedtls_pk_parse_public_key(&key, (const unsigned char *)public_key.data, public_key.len);
    Error e = ERR_OK;
    if (status != 0) {
        e = err_set(err, ERR_PARSE, "signature: the public key does not parse: -0x%04x", (unsigned)-status);
    } else if (mbedtls_pk_get_key_type(&key) != PSA_KEY_TYPE_RSA_PUBLIC_KEY) {
        e = err_set(err, ERR_PARSE, "signature: the public key is not RSA");
    } else {
        *bits = mbedtls_pk_get_bitlen(&key);
        e = verify_hash(&key, digest, signature, valid, err);
    }
    mbedtls_pk_free(&key);
    return e;
}
