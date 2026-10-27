#include "mc/net/acme.h"

#include "mc/concurrency/cancel.h"
#include "mc/crypto/sha256.h"
#include "mc/encoding/base64.h"
#include "mc/encoding/json.h"
#include "mc/net/http.h"
#include "mc/platform/platform.h"
#include "tls_acme.h"

enum {
    MAX_POLLS = 60,
    POLL_INTERVAL_MS = 500,
    MAX_CHALLENGES = 100,
    RETRY_MS = 60000,
    MAX_SLEEP_MS = 12 * 3600 * 1000, // checks at least this often, in case the clock jumps
};

static const char LETS_ENCRYPT[] = "https://acme-v02.api.letsencrypt.org/directory";
static const char CHALLENGE_PATH[] = "/.well-known/acme-challenge/";
static const char BAD_NONCE[] = "urn:ietf:params:acme:error:badNonce";

// Challenge is an HTTP-01 token and the key authorization that answers it.
typedef struct Challenge {
    String token;
    String answer;
} Challenge;

struct Acme {
    Arena *arena;
    HttpClient *client;
    TlsKey *account_key;
    String jwk;        // the account key, with the members RFC 7638 hashes, in its order
    String thumbprint; // base64url SHA-256 of jwk
    String directory_url;
    String *domains;
    size_t domain_count;
    String storage_dir;
    Logger *logger;
    TlsServer *challenge_tls; // answers TLS-ALPN-01 challenges; nullptr answers HTTP-01 on port 80
    Mutex mutex; // guards the challenges, which the HTTP handler reads on worker threads
    Challenge challenges[MAX_CHALLENGES];
    size_t challenge_count;
    Mutex order_mutex; // one acme_certificate at a time, as the HTTP client takes one thread
    Cancel stop;
    Thread renewer;
    bool renewing;
    TlsServer *tls; // what the renewer gives new certificates to
};

// Session is one order's conversation with the CA, in its arena.
typedef struct Session {
    Acme *acme;
    Arena *arena;
    String nonce;
    String account_url; // the kid that signs requests once the account exists
    String new_nonce_url;
    String new_account_url;
    String new_order_url;
} Session;

static String base64url(Arena *arena, String data)
{
    return base64_encode(arena, data, BASE64_URL);
}

[[nodiscard]] static Error account_jwk(Acme *acme, Err *err)
{
    String x = { 0 };
    String y = { 0 };
    Error e = tls_key_public_point(acme->arena, acme->account_key, &x, &y, err);
    if (e != ERR_OK) {
        return e;
    }
    acme->jwk = str_format(acme->arena, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}",
                           base64url(acme->arena, x).data, base64url(acme->arena, y).data);
    uint8_t digest[SHA256_LEN];
    sha256(acme->jwk, digest);
    acme->thumbprint = base64url(acme->arena, (String){ (const char *)digest, sizeof digest });
    return ERR_OK;
}

static String stored_path(Arena *arena, const Acme *acme, const char *name)
{
    return str_format(arena, "%.*s/%s", (int)acme->storage_dir.len, acme->storage_dir.data, name);
}

// load_account_key reads the stored account key, or makes and stores one,
// with storage_dir locked so processes sharing it agree on one.
[[nodiscard]] static Error load_account_key(Acme *acme, Err *err)
{
    Arena *scratch = arena_create(0);
    String path = stored_path(scratch, acme, "account.key.pem");
    String pem = { 0 };
    int lock = -1;
    Error e = dir_create_all(acme->storage_dir, 0700, err);
    if (e == ERR_OK) {
        e = file_lock(stored_path(scratch, acme, "lock"), &lock, err);
    }
    if (e == ERR_OK) {
        e = file_exists(path) ? file_read_all(scratch, path, &pem, err) : tls_key_generate(scratch, &pem, err);
    }
    if (e == ERR_OK && !file_exists(path)) {
        e = file_write_atomic(path, pem, 0600, err);
    }
    if (e == ERR_OK) {
        e = tls_key_load(pem, &acme->account_key, err);
    }
    if (lock >= 0) {
        file_close(lock);
    }
    arena_destroy(scratch);
    return e;
}

Error acme_create(const AcmeConfig *config, Acme **acme, Err *err)
{
    Arena *arena = arena_create(0);
    Acme *created = arena_push(arena, sizeof *created);
    *created = (Acme){
        .arena = arena,
        .directory_url = str_copy(arena, config->directory_url.len > 0 ? config->directory_url : S(LETS_ENCRYPT)),
        .domains = arena_push(arena, config->domain_count * sizeof(String)),
        .domain_count = config->domain_count,
        .storage_dir = str_copy(arena, config->storage_dir),
        .logger = config->logger,
        .challenge_tls = config->challenge_tls,
    };
    for (size_t i = 0; i < config->domain_count; i++) {
        created->domains[i] = str_copy(arena, config->domains[i]);
    }
    HttpClientConfig client_config = { .ca_file = config->ca_file, .user_agent = S("libmc-acme/1") };
    Error e = config->domain_count > 0 ? ERR_OK : err_set(err, ERR_INVALID_ARGUMENT, "acme: no domains");
    if (e == ERR_OK && config->storage_dir.len == 0) {
        e = err_set(err, ERR_INVALID_ARGUMENT, "acme: no storage directory");
    }
    if (e == ERR_OK) {
        e = load_account_key(created, err);
    }
    if (e == ERR_OK) {
        e = account_jwk(created, err);
    }
    if (e == ERR_OK) {
        e = http_client_create(&client_config, &created->client, err);
    }
    if (e != ERR_OK) {
        if (created->account_key != nullptr) {
            tls_key_destroy(created->account_key);
        }
        arena_destroy(arena);
        return e;
    }
    mutex_init(&created->mutex);
    mutex_init(&created->order_mutex);
    cancel_init(&created->stop);
    *acme = created;
    return ERR_OK;
}

void acme_destroy(Acme *acme)
{
    if (acme->renewing) {
        cancel_request(&acme->stop);
        thread_join(&acme->renewer);
    }
    cancel_destroy(&acme->stop);
    http_client_destroy(acme->client);
    tls_key_destroy(acme->account_key);
    mutex_destroy(&acme->order_mutex);
    mutex_destroy(&acme->mutex);
    arena_destroy(acme->arena);
}

// problem is the CA's reason for refusing a request, from its RFC 7807 body.
[[nodiscard]] static Error problem(Session *session, String url, const HttpResponse *response, Err *err)
{
    Node *root = nullptr;
    String detail = json_parse(session->arena, response->body, &root, nullptr) == ERR_OK
                        ? node_get_string(root, S("detail"), S(""))
                        : S("");
    return err_set(err, ERR_NETWORK, "acme: %.*s: %d %.*s", (int)url.len, url.data, response->status, (int)detail.len,
                   detail.data);
}

static bool bad_nonce(Session *session, const HttpResponse *response)
{
    Node *root = nullptr;
    return response->status == 400 && json_parse(session->arena, response->body, &root, nullptr) == ERR_OK &&
           str_equal(node_get_string(root, S("type"), S("")), S(BAD_NONCE));
}

static void take_nonce(Session *session, const HttpResponse *response)
{
    String nonce = { 0 };
    if (http_response_header(response, S("Replay-Nonce"), &nonce)) {
        session->nonce = str_copy(session->arena, nonce);
    }
}

[[nodiscard]] static Error fetch_nonce(Session *session, Err *err)
{
    HttpRequest request = { .method = S("HEAD"), .url = session->new_nonce_url };
    HttpResponse response = { 0 };
    Error e = http_send(session->acme->client, session->arena, &request, &response, err);
    if (e != ERR_OK) {
        return e;
    }
    take_nonce(session, &response);
    return session->nonce.len > 0 ? ERR_OK : err_set(err, ERR_NETWORK, "acme: no nonce from the CA");
}

// jws signs payload for url as RFC 8555 section 6.2 asks: with the account's
// kid once it exists, with its key before.
[[nodiscard]] static Error jws(Session *session, String url, String payload, String *body, Err *err)
{
    Arena *arena = session->arena;
    String signer = session->account_url.len > 0 ? str_format(arena, "\"kid\":%s", json_quote(arena, session->account_url).data)
                                                 : str_format(arena, "\"jwk\":%s", session->acme->jwk.data);
    String header = str_format(arena, "{\"alg\":\"ES256\",%s,\"nonce\":%s,\"url\":%s}", signer.data,
                               json_quote(arena, session->nonce).data, json_quote(arena, url).data);
    String encoded_header = base64url(arena, header);
    String encoded_payload = base64url(arena, payload);
    String signing_input = str_format(arena, "%s.%s", encoded_header.data, encoded_payload.data);
    String signature = { 0 };
    Error e = tls_key_sign(arena, session->acme->account_key, signing_input, &signature, err);
    if (e != ERR_OK) {
        return e;
    }
    *body = str_format(arena, "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}", encoded_header.data,
                       encoded_payload.data, base64url(arena, signature).data);
    return ERR_OK;
}

// post sends payload to url signed, or an empty payload as RFC 8555's
// POST-as-GET, retrying once with a fresh nonce when the CA asks.
[[nodiscard]] static Error post(Session *session, String url, String payload, HttpResponse *response, Err *err)
{
    static const HttpHeader JOSE = { { "Content-Type", 12 }, { "application/jose+json", 21 } };
    for (int attempt = 0;; attempt++) {
        String body = { 0 };
        Error e = jws(session, url, payload, &body, err);
        HttpRequest request = { .method = S("POST"), .url = url, .headers = &JOSE, .header_count = 1, .body = body };
        if (e == ERR_OK) {
            e = http_send(session->acme->client, session->arena, &request, response, err);
        }
        if (e != ERR_OK) {
            return e;
        }
        take_nonce(session, response);
        if (attempt == 0 && bad_nonce(session, response)) {
            continue;
        }
        return response->status < 400 ? ERR_OK : problem(session, url, response, err);
    }
}

[[nodiscard]] static Error post_json(Session *session, String url, String payload, Node **root, HttpResponse *response,
                                     Err *err)
{
    Error e = post(session, url, payload, response, err);
    return e == ERR_OK ? json_parse(session->arena, response->body, root, err) : e;
}

[[nodiscard]] static Error read_directory(Session *session, Err *err)
{
    HttpRequest request = { .url = session->acme->directory_url };
    HttpResponse response = { 0 };
    Node *root = nullptr;
    Error e = http_send(session->acme->client, session->arena, &request, &response, err);
    if (e == ERR_OK && response.status != 200) {
        e = problem(session, request.url, &response, err);
    }
    if (e == ERR_OK) {
        e = json_parse(session->arena, response.body, &root, err);
    }
    if (e != ERR_OK) {
        return e;
    }
    session->new_nonce_url = node_get_string(root, S("newNonce"), S(""));
    session->new_account_url = node_get_string(root, S("newAccount"), S(""));
    session->new_order_url = node_get_string(root, S("newOrder"), S(""));
    if (session->new_nonce_url.len == 0 || session->new_account_url.len == 0 || session->new_order_url.len == 0) {
        return err_set(err, ERR_NETWORK, "acme: the directory lacks newNonce, newAccount or newOrder");
    }
    return fetch_nonce(session, err);
}

[[nodiscard]] static Error register_account(Session *session, Err *err)
{
    HttpResponse response = { 0 };
    Error e = post(session, session->new_account_url, S("{\"termsOfServiceAgreed\":true}"), &response, err);
    if (e != ERR_OK) {
        return e;
    }
    String location = { 0 };
    if (!http_response_header(&response, S("Location"), &location)) {
        return err_set(err, ERR_NETWORK, "acme: the account has no URL");
    }
    session->account_url = str_copy(session->arena, location);
    return ERR_OK;
}

static void challenge_add(Acme *acme, String token, String answer)
{
    mutex_lock(&acme->mutex);
    if (acme->challenge_count < MAX_CHALLENGES) {
        acme->challenges[acme->challenge_count++] = (Challenge){ token, answer };
    }
    mutex_unlock(&acme->mutex);
}

static void challenges_clear(Acme *acme)
{
    mutex_lock(&acme->mutex);
    acme->challenge_count = 0;
    mutex_unlock(&acme->mutex);
    if (acme->challenge_tls != nullptr) {
        tls_server_clear_challenges(acme->challenge_tls);
    }
}

// prepare_answer sets up what proves control of the authorization's domain:
// a challenge certificate on the TLS server for tls-alpn-01, or the key
// authorization port 80 serves for http-01.
[[nodiscard]] static Error prepare_answer(Session *session, const Node *authorization, String token, Err *err)
{
    Acme *acme = session->acme;
    String answer = str_format(session->arena, "%.*s.%s", (int)token.len, token.data, acme->thumbprint.data);
    if (acme->challenge_tls == nullptr) {
        challenge_add(acme, token, answer);
        return ERR_OK;
    }
    String domain = node_get_string(node_get(authorization, S("identifier")), S("value"), S(""));
    return tls_server_add_challenge(acme->challenge_tls, domain, answer, err);
}

// challenge_answer copies the answer to token into arena, if a challenge is open for it.
static bool challenge_answer(Acme *acme, Arena *arena, String token, String *answer)
{
    bool found = false;
    mutex_lock(&acme->mutex);
    for (size_t i = 0; i < acme->challenge_count && !found; i++) {
        found = str_equal(acme->challenges[i].token, token);
        *answer = found ? str_copy(arena, acme->challenges[i].answer) : *answer;
    }
    mutex_unlock(&acme->mutex);
    return found;
}

// failure is why the CA refused an authorization: its challenge's error, if any.
static String failure(const Node *authorization)
{
    const Node *challenges = node_get(authorization, S("challenges"));
    for (size_t i = 0; challenges != nullptr && i < challenges->count; i++) {
        const Node *error = node_get(challenges->items[i], S("error"));
        if (error != nullptr) {
            return node_get_string(error, S("detail"), S(""));
        }
    }
    return S("");
}

// poll asks for url until its status is wanted, waiting while it is pending or processing.
[[nodiscard]] static Error poll(Session *session, String url, String wanted, Node **root, Err *err)
{
    for (size_t i = 0; i < MAX_POLLS; i++) {
        HttpResponse response = { 0 };
        Error e = post_json(session, url, S(""), root, &response, err);
        if (e != ERR_OK) {
            return e;
        }
        String status = node_get_string(*root, S("status"), S(""));
        if (str_equal(status, wanted)) {
            return ERR_OK;
        }
        if (!str_equal(status, S("pending")) && !str_equal(status, S("processing"))) {
            String reason = failure(*root);
            return err_set(err, ERR_NETWORK, "acme: %.*s is %.*s: %.*s", (int)url.len, url.data, (int)status.len,
                           status.data, (int)reason.len, reason.data);
        }
        clock_sleep_ms(POLL_INTERVAL_MS);
    }
    return err_set(err, ERR_TIMEOUT, "acme: %.*s did not become %.*s", (int)url.len, url.data, (int)wanted.len,
                   wanted.data);
}

[[nodiscard]] static Error authorize(Session *session, String url, Err *err)
{
    HttpResponse response = { 0 };
    Node *authorization = nullptr;
    Error e = post_json(session, url, S(""), &authorization, &response, err);
    if (e != ERR_OK || str_equal(node_get_string(authorization, S("status"), S("")), S("valid"))) {
        return e;
    }
    String type = session->acme->challenge_tls != nullptr ? S("tls-alpn-01") : S("http-01");
    const Node *challenges = node_get(authorization, S("challenges"));
    for (size_t i = 0; challenges != nullptr && i < challenges->count; i++) {
        const Node *challenge = challenges->items[i];
        if (!str_equal(node_get_string(challenge, S("type"), S("")), type)) {
            continue;
        }
        e = prepare_answer(session, authorization, node_get_string(challenge, S("token"), S("")), err);
        if (e == ERR_OK) {
            e = post(session, node_get_string(challenge, S("url"), S("")), S("{}"), &response, err);
        }
        return e == ERR_OK ? poll(session, url, S("valid"), &authorization, err) : e;
    }
    return err_set(err, ERR_UNSUPPORTED, "acme: %.*s offers no %.*s challenge", (int)url.len, url.data, (int)type.len,
                   type.data);
}

static String identifiers(Session *session)
{
    StringBuilder text = str_builder_create(session->arena, 128);
    str_builder_append(&text, S("{\"identifiers\":["));
    for (size_t i = 0; i < session->acme->domain_count; i++) {
        str_builder_append_format(&text, "%s{\"type\":\"dns\",\"value\":%s}", i == 0 ? "" : ",",
                                  json_quote(session->arena, session->acme->domains[i]).data);
    }
    str_builder_append(&text, S("]}"));
    return str_builder_finish(&text);
}

// order places the order and proves every domain, and gives where to finalize it.
[[nodiscard]] static Error order(Session *session, String *order_url, String *finalize_url, Err *err)
{
    HttpResponse response = { 0 };
    Node *placed = nullptr;
    Error e = post_json(session, session->new_order_url, identifiers(session), &placed, &response, err);
    if (e != ERR_OK) {
        return e;
    }
    if (!http_response_header(&response, S("Location"), order_url)) {
        return err_set(err, ERR_NETWORK, "acme: the order has no URL");
    }
    *order_url = str_copy(session->arena, *order_url);
    *finalize_url = node_get_string(placed, S("finalize"), S(""));
    const Node *authorizations = node_get(placed, S("authorizations"));
    for (size_t i = 0; e == ERR_OK && authorizations != nullptr && i < authorizations->count; i++) {
        e = authorize(session, authorizations->items[i]->text, err);
    }
    return e;
}

// finalize sends a request for a certificate for a new key, and downloads it once issued.
[[nodiscard]] static Error finalize(Session *session, String order_url, String finalize_url, String *chain_pem,
                                    String *key_pem, Err *err)
{
    TlsKey *key = nullptr;
    Error e = tls_key_generate(session->arena, key_pem, err);
    if (e == ERR_OK) {
        e = tls_key_load(*key_pem, &key, err);
    }
    String request = { 0 };
    if (e == ERR_OK) {
        e = tls_key_request(session->arena, key, session->acme->domains, session->acme->domain_count, &request, err);
    }
    HttpResponse response = { 0 };
    Node *issued = nullptr;
    if (e == ERR_OK) {
        String payload = str_format(session->arena, "{\"csr\":\"%s\"}", base64url(session->arena, request).data);
        e = post(session, finalize_url, payload, &response, err);
    }
    if (e == ERR_OK) {
        e = poll(session, order_url, S("valid"), &issued, err);
    }
    if (e == ERR_OK) {
        e = post(session, node_get_string(issued, S("certificate"), S("")), S(""), &response, err);
    }
    for (size_t i = 0; e == ERR_OK && i < session->acme->domain_count; i++) {
        if (!tls_certificate_matches(response.body, key, session->acme->domains[i])) {
            e = err_set(err, ERR_NETWORK, "acme: the certificate is not for the new key and %.*s",
                        (int)session->acme->domains[i].len, session->acme->domains[i].data);
        }
    }
    if (key != nullptr) {
        tls_key_destroy(key);
    }
    *chain_pem = response.body;
    return e;
}

// obtain orders a certificate for every domain.
[[nodiscard]] static Error obtain(Acme *acme, Arena *arena, String *chain_pem, String *key_pem, Err *err)
{
    Session session = { .acme = acme, .arena = arena };
    String order_url = { 0 };
    String finalize_url = { 0 };
    Error e = read_directory(&session, err);
    if (e == ERR_OK) {
        e = register_account(&session, err);
    }
    if (e == ERR_OK) {
        e = order(&session, &order_url, &finalize_url, err);
    }
    if (e == ERR_OK) {
        e = finalize(&session, order_url, finalize_url, chain_pem, key_pem, err);
    }
    challenges_clear(acme);
    return e;
}

// renew_at is when chain_pem is due for renewal, in Unix seconds: once two
// thirds of its lifetime have passed, as Let's Encrypt advises.
[[nodiscard]] static Error renew_at(String chain_pem, int64_t *at, Err *err)
{
    int64_t not_before = 0;
    int64_t not_after = 0;
    Error e = tls_certificate_validity(chain_pem, &not_before, &not_after, err);
    *at = not_after - (not_after - not_before) / 3;
    return e;
}

static int64_t now_seconds(void)
{
    return clock_wall_ns() / 1000000000;
}

// read_stored gives the stored certificate and key when they still serve:
// the key matches, every domain is named, and renewal is not due.
static bool read_stored(Acme *acme, Arena *arena, String *chain_pem, String *key_pem)
{
    String chain_path = stored_path(arena, acme, "certificate.pem");
    String key_path = stored_path(arena, acme, "certificate.key.pem");
    TlsKey *key = nullptr;
    int64_t due = 0;
    bool usable = file_exists(chain_path) && file_exists(key_path) &&
                  file_read_all(arena, chain_path, chain_pem, nullptr) == ERR_OK &&
                  file_read_all(arena, key_path, key_pem, nullptr) == ERR_OK &&
                  renew_at(*chain_pem, &due, nullptr) == ERR_OK && now_seconds() < due &&
                  tls_key_load(*key_pem, &key, nullptr) == ERR_OK;
    for (size_t i = 0; usable && i < acme->domain_count; i++) {
        usable = tls_certificate_matches(*chain_pem, key, acme->domains[i]);
    }
    if (key != nullptr) {
        tls_key_destroy(key);
    }
    return usable;
}

// store writes the key before the chain, so a stored chain always has its key.
[[nodiscard]] static Error store(Acme *acme, Arena *arena, String chain_pem, String key_pem, Err *err)
{
    Error e = file_write_atomic(stored_path(arena, acme, "certificate.key.pem"), key_pem, 0600, err);
    return e == ERR_OK ? file_write_atomic(stored_path(arena, acme, "certificate.pem"), chain_pem, 0600, err) : e;
}

Error acme_certificate(Acme *acme, Arena *arena, String *chain_pem, String *key_pem, Err *err)
{
    mutex_lock(&acme->order_mutex);
    int lock = -1;
    Error e = file_lock(stored_path(arena, acme, "lock"), &lock, err);
    if (e == ERR_OK && !read_stored(acme, arena, chain_pem, key_pem)) {
        e = obtain(acme, arena, chain_pem, key_pem, err);
        if (e == ERR_OK) {
            e = store(acme, arena, *chain_pem, *key_pem, err);
        }
    }
    if (lock >= 0) {
        file_close(lock);
    }
    mutex_unlock(&acme->order_mutex);
    return e;
}

// renew_once gives tls the certificate acme_certificate gives, and reports
// how long to wait before the next try.
static int64_t renew_once(Acme *acme)
{
    Arena *arena = arena_create(0);
    String chain = { 0 };
    String key = { 0 };
    int64_t due = 0;
    Err err = { 0 };
    Error e = acme_certificate(acme, arena, &chain, &key, &err);
    if (e == ERR_OK) {
        e = tls_server_set_certificate(acme->tls, chain, key, &err);
    }
    if (e == ERR_OK) {
        e = renew_at(chain, &due, &err);
    }
    int64_t wait_ms = RETRY_MS;
    if (e == ERR_OK) {
        int64_t seconds = due - now_seconds();
        wait_ms = seconds <= 0 ? 0 : seconds * 1000 < MAX_SLEEP_MS ? seconds * 1000 : MAX_SLEEP_MS;
    } else if (acme->logger != nullptr) {
        LogAttr attrs[] = { log_err(&err) };
        log_error(acme->logger, "acme: renewal failed", countof(attrs), attrs);
    }
    arena_destroy(arena);
    return wait_ms;
}

static void *keep_renewed(void *argument)
{
    Acme *acme = argument;
    for (int64_t wait_ms = renew_once(acme); cancel_sleep(&acme->stop, wait_ms); wait_ms = renew_once(acme)) {
    }
    return nullptr;
}

Error acme_keep_renewed(Acme *acme, TlsServer *tls, Err *err)
{
    acme->tls = tls;
    Error e = thread_start(&acme->renewer, keep_renewed, acme, err);
    acme->renewing = e == ERR_OK;
    return e;
}

// request_host is the Host field without a port.
static String request_host(const HttpServerRequest *request)
{
    for (size_t i = 0; i < request->header_count; i++) {
        if (str_equal_ignore_case(request->headers[i].name, S("Host"))) {
            String host = { 0 };
            String port = { 0 };
            return str_cut(request->headers[i].value, ':', &host, &port) ? host : request->headers[i].value;
        }
    }
    return S("");
}

static bool serves(const Acme *acme, String host)
{
    for (size_t i = 0; i < acme->domain_count; i++) {
        if (str_equal_ignore_case(acme->domains[i], host)) {
            return true;
        }
    }
    return false;
}

Error acme_http_handler(void *context, Arena *arena, const HttpServerRequest *request, HttpServerResponse *response,
                        Err *err)
{
    Acme *acme = context;
    if (str_starts_with(request->path, S(CHALLENGE_PATH))) {
        String token = str_slice(request->path, sizeof CHALLENGE_PATH - 1, request->path.len);
        response->status = challenge_answer(acme, arena, token, &response->body) ? 200 : 404;
        return ERR_OK;
    }
    String host = request_host(request);
    if (!serves(acme, host)) {
        response->status = 421;
        return ERR_OK;
    }
    response->headers = arena_push(arena, sizeof *response->headers);
    response->headers[0] = (HttpHeader){ S("Location"), str_format(arena, "https://%.*s%.*s", (int)host.len, host.data,
                                                                   (int)request->target.len, request->target.data) };
    response->header_count = 1;
    response->status = 301;
    return ERR_OK;
}
