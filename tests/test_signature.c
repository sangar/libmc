#include "test.h"

#include <string.h>

#include "mc/crypto/sha256.h"
#include "mc/encoding/base64.h"
#include "mc/net/signature.h"

// The keys were generated once with the openssl CLI, and each RSA key signed
// SIGNATURE_MESSAGE with `openssl dgst -sha256 -sign`; the private keys were
// not kept. Each is base64 of DER.
static const char SIGNATURE_MESSAGE[] = "From: a@example.com\r\nSubject: DKIM test\r\n";

static const char RSA2048_SPKI[] =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA2k1CJ6YkP4WOwIPBenQa9/a8TCMan+HS8OChOJeRazJqpxQ1SWK98brQ"
    "f5VCialKeFzzSLzfXeYcOb8arRPFH9e/PcAdT1aaksm8j0pjy2ekY1He0ZQKfc8Qu8d8b6z2a+65dQdIsb9w/lXv+FhZkJmT1ucz"
    "XT5Vb0/mVsl+ASig+7qPRlZDizadjp5JR7nrr1bnCMPHNwYuukyeJO2485IeQcBoh5MtGqNQh7ie925lsc5KkU1ahuCwY6oyP+vf"
    "xOwfY3dG84Eo/lfKTMa70b0qFHqDKIZxjTkYDx+wpFMqrbalAIrvEvk9TP6jhkTPOZPbLEvTC+0UNhXQvriIKwIDAQAB";

static const char RSA2048_PKCS1[] =
    "MIIBCgKCAQEA2k1CJ6YkP4WOwIPBenQa9/a8TCMan+HS8OChOJeRazJqpxQ1SWK98brQf5VCialKeFzzSLzfXeYcOb8arRPFH9e/"
    "PcAdT1aaksm8j0pjy2ekY1He0ZQKfc8Qu8d8b6z2a+65dQdIsb9w/lXv+FhZkJmT1uczXT5Vb0/mVsl+ASig+7qPRlZDizadjp5J"
    "R7nrr1bnCMPHNwYuukyeJO2485IeQcBoh5MtGqNQh7ie925lsc5KkU1ahuCwY6oyP+vfxOwfY3dG84Eo/lfKTMa70b0qFHqDKIZx"
    "jTkYDx+wpFMqrbalAIrvEvk9TP6jhkTPOZPbLEvTC+0UNhXQvriIKwIDAQAB";

static const char RSA2048_SIGNATURE[] =
    "Qy9YA8+j8XLHike+oFpEpCLj5cy2sE948moSgvGje1rzIis7M5FL027Mh/YDBu8Hk/3MQXZnAcPPIQuB2uZsNBFUdYhR751Pe5pH"
    "g/M2Y/3lTaLaxc1nrB96FlTe9avqFS4lln14KxniMKjFT0QUDjVXLgnb93TNpQdpY/0EfgUloOX3OWqyt11ylKBpSD+KSt6UUik4"
    "k9V27ouNMSmzNOi2ptjJuI2Kb3547rQ6OFGnqwR3N/oUpUGBYbQTI4A4eDb1p0Lqy5CellpP2kl9zYuaGFZkMVdqe6tpvD9DCvxY"
    "zK+vt21sq4caCiGXuKdZW6e4hCxZBGsqZxabOHqrBQ==";

static const char RSA1024_SPKI[] =
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCjQO9kvFsWUhvsYZ8aSIReSBEgjnfwMMSu2mR85gJWOJAR8QdXcsGv87JSLxvT"
    "x6FLQRLTH9+uBAEv+vJBzf2lhM8x8h8FPnDk0L6gHjPO+PpsArUdqwOIPj1kOkJI6I4tC/XzdUEFsMtSg+LGh9tNmGmD+KE/n5XV"
    "/Y7xgPKH0QIDAQAB";

static const char RSA1024_SIGNATURE[] =
    "AlgPN5RVVgmPq/+dXyZLAibVdeo7Si2hQdQ3yOXFYA0FVfaWJg2hkuxTNrSJ+d4Ov9InqZqoPXOa5GI3oXtWG8BVbq47lSPF4Dk0"
    "WT8LjpZmiHxss14ijuyymKFOiCHOKBkQT1hPVWhXd4VuQafR5RMWkzJhFN1h65LrPtmxCiQ=";

static const char P256_SPKI[] =
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEHNgVTPZxve3nD5dfYnphtcXqEUw2F/Ze0iQkgFwisk8NcwjUIT1j+owDb8Uxxbgp"
    "Vb5Mjv/9834FlZqGJajMpA==";

typedef struct SignatureCheck {
    Error error;
    size_t bits;
    bool valid;
} SignatureCheck;

static String signature_test_bytes(Test *test, const char *base64)
{
    String bytes = { 0 };
    test_check(test, base64_decode(test->arena, S(base64), BASE64_STANDARD, &bytes), "a test vector decodes");
    return bytes;
}

static SignatureCheck signature_test_verify(Test *test, const char *key, const uint8_t digest[SHA256_LEN],
                                            String signature)
{
    SignatureCheck check = { 0 };
    check.error = signature_verify_rsa_sha256(signature_test_bytes(test, key), digest, signature, &check.bits,
                                              &check.valid, nullptr);
    return check;
}

static void signature_verifies_rsa_sha256(Test *test)
{
    uint8_t digest[SHA256_LEN];
    sha256(S(SIGNATURE_MESSAGE), digest);
    String signature = signature_test_bytes(test, RSA2048_SIGNATURE);
    SignatureCheck spki = signature_test_verify(test, RSA2048_SPKI, digest, signature);
    test_check(test, spki.error == ERR_OK && spki.valid && spki.bits == 2048, "a 2048-bit SubjectPublicKeyInfo");
    SignatureCheck bare = signature_test_verify(test, RSA2048_PKCS1, digest, signature);
    test_check(test, bare.error == ERR_OK && bare.valid && bare.bits == 2048, "a bare 2048-bit RSAPublicKey");
    SignatureCheck short_key =
        signature_test_verify(test, RSA1024_SPKI, digest, signature_test_bytes(test, RSA1024_SIGNATURE));
    test_check(test, short_key.error == ERR_OK && short_key.valid && short_key.bits == 1024, "a 1024-bit key");
}

static void signature_refuses_what_the_key_did_not_sign(Test *test)
{
    uint8_t digest[SHA256_LEN];
    sha256(S(SIGNATURE_MESSAGE), digest);
    uint8_t other_digest[SHA256_LEN];
    sha256(S("Subject: another message\r\n"), other_digest);
    String signature = signature_test_bytes(test, RSA2048_SIGNATURE);
    SignatureCheck other = signature_test_verify(test, RSA2048_SPKI, other_digest, signature);
    test_check(test, other.error == ERR_OK && !other.valid && other.bits == 2048, "a signature of another digest");
    char *flipped = arena_push(test->arena, signature.len);
    memcpy(flipped, signature.data, signature.len);
    flipped[100] ^= 1;
    SignatureCheck changed =
        signature_test_verify(test, RSA2048_SPKI, digest, (String){ .data = flipped, .len = signature.len });
    test_check(test, changed.error == ERR_OK && !changed.valid, "a signature with a flipped bit");
    SignatureCheck truncated =
        signature_test_verify(test, RSA2048_SPKI, digest, str_slice(signature, 0, signature.len - 1));
    test_check(test, truncated.error == ERR_OK && !truncated.valid, "a truncated signature");
}

static void signature_refuses_keys_that_are_not_rsa(Test *test)
{
    uint8_t digest[SHA256_LEN];
    sha256(S(SIGNATURE_MESSAGE), digest);
    String signature = signature_test_bytes(test, RSA2048_SIGNATURE);
    size_t bits;
    bool valid;
    test_check(test, signature_verify_rsa_sha256(S("not a key"), digest, signature, &bits, &valid, nullptr) == ERR_PARSE,
               "a key that does not parse");
    SignatureCheck p256 = signature_test_verify(test, P256_SPKI, digest, signature);
    test_check(test, p256.error == ERR_PARSE && !p256.valid, "a P-256 key");
}

const TestCase SIGNATURE_TESTS[] = {
    { "signature_verifies_rsa_sha256", signature_verifies_rsa_sha256 },
    { "signature_refuses_what_the_key_did_not_sign", signature_refuses_what_the_key_did_not_sign },
    { "signature_refuses_keys_that_are_not_rsa", signature_refuses_keys_that_are_not_rsa },
    { nullptr, nullptr },
};
