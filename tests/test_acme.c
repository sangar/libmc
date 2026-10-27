#include "test.h"

#include "mc/net/acme.h"
#include "mc/net/tls.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

// The tests need no CA: they check what port 80 answers and how a stored
// certificate is used. Ordering from a CA, by both challenges, is checked
// against Let's Encrypt's test CA, Pebble, in httpbed.

static const String DOMAINS[] = { { "localhost", 9 } };

// make_acme points a client at directory_url, which nothing need answer,
// with storage in a temporary directory, holding a self-signed certificate
// for host when host is not empty.
static Acme *make_acme(Test *test, String directory_url, String host, String *storage, String *certificate)
{
    Err err = { 0 };
    String key = { 0 };
    Acme *acme = nullptr;
    bool made = dir_create_temp(test->arena, S("libmc-acme-"), storage, &err) == ERR_OK;
    if (made && host.len > 0) {
        made = tls_self_signed(test->arena, host, 3, certificate, &key, &err) == ERR_OK &&
               file_write_all(path_join(test->arena, *storage, S("certificate.key.pem")), key, 0600, &err) == ERR_OK &&
               file_write_all(path_join(test->arena, *storage, S("certificate.pem")), *certificate, 0600, &err) == ERR_OK;
    }
    AcmeConfig config = {
        .directory_url = directory_url,
        .storage_dir = *storage,
        .domains = DOMAINS,
        .domain_count = countof(DOMAINS),
    };
    made = made && acme_create(&config, &acme, &err) == ERR_OK;
    test_check(test, made, err.msg);
    return acme;
}

static void acme_stop(Acme *acme, String storage)
{
    if (acme != nullptr) {
        acme_destroy(acme);
    }
    if (storage.len > 0) {
        unused(dir_remove_all(storage, nullptr));
    }
}

static String exchange(Test *test, Acme *acme, const char *request)
{
    HttpServerConfig config = { .handler = acme_http_handler, .context = acme };
    StringList pieces = { 0 };
    strlist_push(test->arena, &pieces, S(request));
    String answer = { 0 };
    test_check(test, http_server_exchange(test->arena, &config, pieces, &answer, nullptr) == ERR_OK, "exchange");
    return answer;
}

static void acme_port_80_redirects_its_domains_only(Test *test)
{
    String storage = { 0 };
    String unused_certificate = { 0 };
    Acme *acme = make_acme(test, S(""), S(""), &storage, &unused_certificate);
    if (acme != nullptr) {
        String redirected = exchange(test, acme, "GET /a?b HTTP/1.1\r\nHost: LocalHost:80\r\n\r\n");
        test_check(test, str_starts_with(redirected, S("HTTP/1.1 301 ")) &&
                             str_contains(redirected, S("Location: https://LocalHost/a?b\r\n")),
                   "a domain's request goes to https");
        test_check(test, str_starts_with(exchange(test, acme, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"),
                                         S("HTTP/1.1 421 ")),
                   "another host is refused");
        test_check(test, str_starts_with(exchange(test, acme, "GET /.well-known/acme-challenge/nope HTTP/1.1\r\nHost: localhost\r\n\r\n"),
                                         S("HTTP/1.1 404 ")),
                   "a challenge nobody asked for is not found");
    }
    acme_stop(acme, storage);
}

static void acme_keeps_a_stored_certificate(Test *test)
{
    String storage = { 0 };
    String stored = { 0 };
    Acme *acme = make_acme(test, S("https://localhost:1/dir"), S("localhost"), &storage, &stored);
    String chain = { 0 };
    String key = { 0 };
    Err err = { 0 };
    if (acme != nullptr) {
        test_check(test, acme_certificate(acme, test->arena, &chain, &key, &err) == ERR_OK, err.msg);
        test_check_str(test, chain, stored, "a stored certificate far from expiry needs no CA");
    }
    acme_stop(acme, storage);
}

static void acme_orders_when_the_stored_certificate_names_another_domain(Test *test)
{
    String storage = { 0 };
    String stored = { 0 };
    Acme *acme = make_acme(test, S("https://localhost:1/dir"), S("example.com"), &storage, &stored);
    String chain = { 0 };
    String key = { 0 };
    if (acme != nullptr) {
        test_check(test, acme_certificate(acme, test->arena, &chain, &key, nullptr) == ERR_NETWORK,
                   "it asks the CA, which is not there");
    }
    acme_stop(acme, storage);
}

const TestCase ACME_TESTS[] = {
    { "acme_port_80_redirects_its_domains_only", acme_port_80_redirects_its_domains_only },
    { "acme_keeps_a_stored_certificate", acme_keeps_a_stored_certificate },
    { "acme_orders_when_the_stored_certificate_names_another_domain",
      acme_orders_when_the_stored_certificate_names_another_domain },
    { nullptr, nullptr },
};
