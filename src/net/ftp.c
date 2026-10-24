#include "mc/net/ftp.h"

#include <curl/curl.h>

#include "mc/platform/platform.h"
#include "roots.h"

enum {
    DEFAULT_CONNECT_TIMEOUT_MS = 10 * 1000,
    DEFAULT_STALL_TIMEOUT_MS = 60 * 1000,
    MAX_LISTING_BYTES = 16 * 1024 * 1024,
    INITIAL_LISTING_CAPACITY = 4096,
};

struct FtpClient {
    CURL *curl;
    // arena holds the copy of the configuration.
    Arena *arena;
    // url is the server without a trailing slash.
    String url;
    const char *username;
    const char *password;
    // ca_file is null where the system has no bundle; Mozilla's roots, built in, are used instead.
    const char *ca_file;
    int64_t connect_timeout_ms;
    int64_t stall_timeout_ms;
    bool allow_plain_ftp;
};

Error ftp_client_create(const FtpClientConfig *config, FtpClient **client, Err *err)
{
    String ca_file = roots_file(config->ca_file);
    if (ca_file.len > 0 && !file_exists(ca_file)) {
        return err_set(err, ERR_NOT_FOUND, "ftp: no bundle of trusted roots at %.*s", (int)ca_file.len, ca_file.data);
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return err_set(err, ERR_PLATFORM, "ftp: curl_global_init failed");
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        curl_global_cleanup();
        return err_set(err, ERR_OUT_OF_MEMORY, "ftp: curl_easy_init failed");
    }
    Arena *arena = arena_create(0);
    FtpClient *created = arena_push(arena, sizeof *created);
    *created = (FtpClient){
        .curl = curl,
        .arena = arena,
        .url = str_copy(arena, str_trim_suffix(config->url, S("/"))),
        .username = config->username.len > 0 ? str_cstr(arena, config->username) : nullptr,
        .password = str_cstr(arena, config->password),
        .ca_file = ca_file.len > 0 ? str_cstr(arena, ca_file) : nullptr,
        .connect_timeout_ms = config->connect_timeout_ms > 0 ? config->connect_timeout_ms : DEFAULT_CONNECT_TIMEOUT_MS,
        .stall_timeout_ms = config->stall_timeout_ms > 0 ? config->stall_timeout_ms : DEFAULT_STALL_TIMEOUT_MS,
        .allow_plain_ftp = config->allow_plain_ftp,
    };
    *client = created;
    return ERR_OK;
}

void ftp_client_destroy(FtpClient *client)
{
    curl_easy_cleanup(client->curl);
    curl_global_cleanup();
    arena_destroy(client->arena);
}

// Transfer is one command's data: a local file to write, or to read from
// offset, or a listing in memory. failed records a failure on our side, which
// curl sees only as an aborted transfer, with its detail already in err.
typedef struct Transfer {
    int fd;
    int64_t offset;
    StringBuilder listing;
    Cancel *cancel;
    Err *err;
    Error failed;
    char error_buffer[CURL_ERROR_SIZE];
} Transfer;

static size_t write_file(char *data, size_t size, size_t count, void *argument)
{
    Transfer *transfer = argument;
    size_t length = size * count;
    transfer->failed = file_write(transfer->fd, (String){ data, length }, transfer->err);
    return transfer->failed == ERR_OK ? length : 0;
}

static size_t read_file(char *buffer, size_t size, size_t count, void *argument)
{
    Transfer *transfer = argument;
    size_t got = 0;
    transfer->failed = file_pread(transfer->fd, buffer, size * count, transfer->offset, &got, transfer->err);
    transfer->offset += (int64_t)got;
    return transfer->failed == ERR_OK ? got : CURL_READFUNC_ABORT;
}

static int check_cancel(void *argument, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total,
                        curl_off_t uploaded)
{
    return cancel_requested(argument) ? 1 : 0;
}

static size_t write_listing(char *data, size_t size, size_t count, void *argument)
{
    Transfer *transfer = argument;
    size_t length = size * count;
    if (transfer->listing.len + length > MAX_LISTING_BYTES) {
        transfer->failed =
            err_set(transfer->err, ERR_UNSUPPORTED, "ftp: listing larger than %d bytes", MAX_LISTING_BYTES);
        return 0;
    }
    str_builder_append(&transfer->listing, (String){ data, length });
    return length;
}

// remote_url is path on the server, percent-encoded. curl starts from the
// directory the server starts in, and from the root when the path begins
// with /; it lists a directory when the URL ends with /.
static const char *remote_url(Arena *arena, const FtpClient *client, String path, bool directory)
{
    String url = str_format(arena, "%.*s/%s", (int)client->url.len, client->url.data,
                            str_percent_encode(arena, path, true).data);
    if (directory && !str_ends_with(url, S("/"))) {
        url = str_concat(arena, url, S("/"));
    }
    return str_cstr(arena, url);
}

// check_call refuses a call on a cancelled token, a path with a line break,
// which would end the command it is sent in, and, where a file is wanted, a
// path that names a directory.
[[nodiscard]] static Error check_call(Cancel *cancel, String path, bool file, Err *err)
{
    if (cancel != nullptr && cancel_requested(cancel)) {
        return err_set(err, ERR_CANCELLED, "ftp: cancelled");
    }
    if (str_contains_any(path, S("\r\n"))) {
        return err_set(err, ERR_INVALID_ARGUMENT, "ftp: a path holds a line break");
    }
    if (file && (path.len == 0 || str_ends_with(path, S("/")))) {
        return err_set(err, ERR_INVALID_ARGUMENT, "ftp: %.*s names no file", (int)path.len, path.data);
    }
    return ERR_OK;
}

// set_options resets the handle, which keeps its connection, for one command on url.
static void set_options(FtpClient *client, const char *url, Transfer *transfer)
{
    CURL *curl = client->curl;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "ftp,ftps");
    curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)(client->allow_plain_ftp ? CURLUSESSL_TRY : CURLUSESSL_ALL));
    roots_set(curl, client->ca_file);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)client->connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SERVER_RESPONSE_TIMEOUT_MS, (long)client->stall_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)((client->stall_timeout_ms + 999) / 1000));
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, transfer->error_buffer);
    if (transfer->cancel != nullptr) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, transfer->cancel);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    if (client->username != nullptr) {
        curl_easy_setopt(curl, CURLOPT_USERNAME, client->username);
        curl_easy_setopt(curl, CURLOPT_PASSWORD, client->password);
    }
}

[[nodiscard]] static Error perform(FtpClient *client, Transfer *transfer, Err *err)
{
    CURLcode code = curl_easy_perform(client->curl);
    if (code == CURLE_OK) {
        return ERR_OK;
    }
    if (transfer->failed != ERR_OK) {
        return transfer->failed;
    }
    if (transfer->cancel != nullptr && cancel_requested(transfer->cancel)) {
        return err_set(err, ERR_CANCELLED, "ftp: cancelled");
    }
    const char *message = transfer->error_buffer[0] != '\0' ? transfer->error_buffer : curl_easy_strerror(code);
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
        return err_set(err, ERR_TIMEOUT, "ftp: %s", message);
    case CURLE_URL_MALFORMAT:
    case CURLE_UNSUPPORTED_PROTOCOL:
        return err_set(err, ERR_INVALID_ARGUMENT, "ftp: %s", message);
    case CURLE_REMOTE_FILE_NOT_FOUND:
        return err_set(err, ERR_NOT_FOUND, "ftp: %s", message);
    case CURLE_OUT_OF_MEMORY:
        return err_set(err, ERR_OUT_OF_MEMORY, "ftp: %s", message);
    default:
        return err_set(err, ERR_NETWORK, "ftp: %s", message);
    }
}

Error ftp_download(FtpClient *client, Cancel *cancel, String remote_path, int fd, Err *err)
{
    Error e = check_call(cancel, remote_path, true, err);
    if (e == ERR_OK) {
        e = file_truncate(fd, 0, err);
    }
    if (e == ERR_OK) {
        e = file_seek(fd, 0, err);
    }
    if (e != ERR_OK) {
        return e;
    }
    Arena *scratch = arena_create(0);
    Transfer transfer = { .fd = fd, .cancel = cancel, .err = err };
    set_options(client, remote_url(scratch, client, remote_path, false), &transfer);
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &transfer);
    e = perform(client, &transfer, err);
    arena_destroy(scratch);
    return e;
}

Error ftp_upload(FtpClient *client, Cancel *cancel, String remote_path, int fd, Err *err)
{
    Error e = check_call(cancel, remote_path, true, err);
    if (e != ERR_OK) {
        return e;
    }
    Arena *scratch = arena_create(0);
    Transfer transfer = { .fd = fd, .cancel = cancel, .err = err };
    set_options(client, remote_url(scratch, client, remote_path, false), &transfer);
    curl_easy_setopt(client->curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(client->curl, CURLOPT_READFUNCTION, read_file);
    curl_easy_setopt(client->curl, CURLOPT_READDATA, &transfer);
    e = perform(client, &transfer, err);
    arena_destroy(scratch);
    return e;
}

Error ftp_list(FtpClient *client, Cancel *cancel, Arena *arena, String remote_directory, StringList *names, Err *err)
{
    Error e = check_call(cancel, remote_directory, false, err);
    if (e != ERR_OK) {
        return e;
    }
    Arena *scratch = arena_create(0);
    Transfer transfer = {
        .listing = str_builder_create(scratch, INITIAL_LISTING_CAPACITY),
        .cancel = cancel,
        .err = err,
    };
    set_options(client, remote_url(scratch, client, remote_directory, true), &transfer);
    curl_easy_setopt(client->curl, CURLOPT_DIRLISTONLY, 1L);
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_listing);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &transfer);
    e = perform(client, &transfer, err);
    if (e == ERR_OK) {
        *names = (StringList){ 0 };
        StringList lines = str_split(scratch, str_builder_finish(&transfer.listing), '\n');
        for (size_t i = 0; i < lines.count; i++) {
            String name = str_trim_suffix(lines.items[i], S("\r"));
            if (name.len > 0) {
                strlist_push(arena, names, str_copy(arena, name));
            }
        }
    }
    arena_destroy(scratch);
    return e;
}
