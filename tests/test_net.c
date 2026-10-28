#include "test.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>


#include "mc/net/ftp.h"
#include "mc/net/http.h"
#include "mc/net/http_server.h"
#include "mc/net/smtp.h"
#include "mc/net/tls.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

// The tests run the clients against a server on 127.0.0.1 in a thread of this
// process, over plain HTTP or TLS with the certificates in tests/fixtures/tls,
// which a test CA there signed for localhost and 127.0.0.1. Over TLS it can
// offer HTTP/2, which it answers through nghttp2. It can also take mail, and
// serve files over FTP, with TLS through STARTTLS or AUTH TLS or from the
// start. Set LIBMC_LIVE_TESTS=1 to also call real hosts; where the system has no bundle of trusted roots, as in
// a bare ubuntu:24.04, that checks the roots built into libmc.

static const char CA_FILE[] = "tests/fixtures/tls/ca.pem";
static const char SERVER_CERT_FILE[] = "tests/fixtures/tls/server.pem";
static const char SERVER_KEY_FILE[] = "tests/fixtures/tls/server-key.pem";

enum { ACCEPT_POLL_MS = 50, REQUEST_LIMIT = 64 * 1024, SLOW_ROUTE_MS = 1000 };

// Connection is one accepted socket, read and written in the clear or through TLS.
typedef struct Connection {
    int fd;
    TlsConnection *tls;
} Connection;

static int connection_receive(Connection *connection, char *buffer, size_t capacity)
{
    size_t got = 0;
    Error e = connection->tls != nullptr ? tls_read(connection->tls, buffer, capacity, &got, nullptr)
                                         : net_receive(connection->fd, buffer, capacity, &got, nullptr);
    return e == ERR_OK ? (int)got : 0;
}

static void connection_send(Connection *connection, String data)
{
    unused(connection->tls != nullptr ? tls_write(connection->tls, data, nullptr) : net_send(connection->fd, data, nullptr));
}

// read_request reads the head and a Content-Length body, and returns them as one text.
static String read_request(Arena *arena, Connection *connection)
{
    char *buffer = arena_push(arena, REQUEST_LIMIT);
    size_t received = 0;
    size_t head_end = 0;
    size_t wanted = REQUEST_LIMIT;
    while (received < wanted) {
        int got = connection_receive(connection, buffer + received, REQUEST_LIMIT - received);
        if (got <= 0) {
            break;
        }
        received += (size_t)got;
        String text = { buffer, received };
        if (head_end == 0 && str_find(text, S("\r\n\r\n"), &head_end)) {
            head_end += 4;
            wanted = head_end;
            size_t at = 0;
            uint64_t length = 0;
            if (str_find(text, S("Content-Length: "), &at) &&
                str_parse_u64(str_trim(str_first_line(str_slice(text, at + 16, received))), &length)) {
                wanted = head_end + (size_t)length;
            }
        }
    }
    return (String){ buffer, received };
}

static String format_response(Arena *arena, int status, const char *headers, String body)
{
    String head = str_format(arena, "HTTP/1.1 %d Test\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n", status,
                             body.len, headers);
    return str_concat(arena, head, body);
}

// GZIP_HELLO is "hello, gzip" and GZIP_LARGE 64 KiB of "x", both gzipped.
static const unsigned char GZIP_HELLO[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x48,
    0xaf, 0xca, 0x2c, 0x00, 0x00, 0x4a, 0x9b, 0xb1, 0x5c, 0x0b, 0x00, 0x00, 0x00,
};
static const unsigned char GZIP_LARGE[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xed, 0xc1, 0x01, 0x01, 0x00, 0x00, 0x00, 0x80,
    0x90, 0xdb, 0xcd, 0xef, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x9a, 0x58,
    0xb4, 0x2c, 0x00, 0x00, 0x01, 0x00,
};

// route answers the request: /echo sends the request back as the body, the others test one behaviour each.
static String route(Arena *arena, String request)
{
    String line = str_first_line(request);
    String method = { 0 };
    String rest = { 0 };
    String path = { 0 };
    str_cut(line, ' ', &method, &rest);
    str_cut(rest, ' ', &path, &rest);
    if (str_equal(path, S("/echo"))) {
        return format_response(arena, 200, "Content-Type: text/plain\r\nX-Test: yes\r\n", request);
    }
    if (str_equal(path, S("/found"))) {
        return format_response(arena, 302, "Location: /echo\r\n", S(""));
    }
    if (str_equal(path, S("/see-other"))) {
        return format_response(arena, 303, "Location: /echo\r\n", S(""));
    }
    if (str_equal(path, S("/temporary"))) {
        return format_response(arena, 307, "Location: /echo\r\n", S(""));
    }
    if (str_equal(path, S("/loop"))) {
        return format_response(arena, 302, "Location: /loop\r\n", S(""));
    }
    if (str_equal(path, S("/elsewhere"))) {
        return format_response(arena, 302, "Location: http://elsewhere.invalid/echo\r\n", S(""));
    }
    if (str_equal(path, S("/unavailable"))) {
        return format_response(arena, 503, "Retry-After: 120\r\n", S("try later"));
    }
    if (str_equal(path, S("/large"))) {
        return format_response(arena, 200, "", str_repeat(arena, S("x"), 4096));
    }
    if (str_equal(path, S("/slow"))) {
        clock_sleep_ms(SLOW_ROUTE_MS);
        return format_response(arena, 200, "", S("late"));
    }
    if (str_equal(path, S("/large-chunked"))) {
        String chunk = str_repeat(arena, S("x"), 1024);
        return str_format(arena, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                                 "400\r\n%.*s\r\n400\r\n%.*s\r\n0\r\n\r\n",
                          (int)chunk.len, chunk.data, (int)chunk.len, chunk.data);
    }
    if (str_equal(path, S("/gzip"))) {
        return format_response(arena, 200, "Content-Encoding: gzip\r\n", (String){ (const char *)GZIP_HELLO, sizeof GZIP_HELLO });
    }
    if (str_equal(path, S("/gzip-large"))) {
        return format_response(arena, 200, "Content-Encoding: gzip\r\n", (String){ (const char *)GZIP_LARGE, sizeof GZIP_LARGE });
    }
    if (str_equal(path, S("/chunked"))) {
        return S("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                 "6\r\nhello \r\n5\r\nworld\r\n0\r\n\r\n");
    }
    return format_response(arena, 404, "", S("no such route"));
}

// SmtpServer takes one mail session at a time: over TLS from the start when
// implicit_tls, else in the clear, offering STARTTLS when starttls. It keeps
// what it received in arena, for a test to read once server_stop has joined it.
typedef struct SmtpServer {
    bool implicit_tls;
    bool starttls;
    Arena *arena;
    String auth;
    String mail_from;
    StringList recipients;
    String data;
} SmtpServer;

// FtpServer takes one session at a time: over TLS from the start when
// implicit_tls, else in the clear, offering AUTH TLS when auth_tls. Data goes
// over passive connections, through TLS after PROT P. It keeps files in arena
// by their path from the root, where a session starts, and is slow to send
// those with "slow" in their path.
typedef struct FtpServer {
    bool implicit_tls;
    bool auth_tls;
    Arena *arena;
    StringList paths;
    StringList contents;
    String user;
    bool user_over_tls;
    bool data_in_clear;
    size_t sessions;
} FtpServer;

typedef struct Server {
    int listener;
    String address;
    TlsServer *tls;
    // smtp makes the server take mail instead of answering HTTP, and ftp serve files.
    SmtpServer *smtp;
    FtpServer *ftp;
    atomic_bool stopping;
    Thread thread;
} Server;

// start_tls runs the server's side of a TLS handshake; the connection then reads and writes through it.
static bool start_tls(TlsServer *tls, Connection *connection)
{
    return tls_accept(tls, connection->fd, &connection->tls, nullptr) == ERR_OK;
}

static void end_tls(Connection *connection)
{
    if (connection->tls != nullptr) {
        tls_close(connection->tls);
        connection->tls = nullptr;
    }
}

static void serve(Server *server, int client)
{
    Arena *arena = arena_create(0);
    Connection connection = { .fd = client };
    bool ready = server->tls == nullptr || start_tls(server->tls, &connection);
    if (ready) {
        connection_send(&connection, route(arena, read_request(arena, &connection)));
    }
    end_tls(&connection);
    net_close(client);
    arena_destroy(arena);
}

// LineReader reads CRLF-terminated lines from a connection.
typedef struct LineReader {
    Connection *connection;
    char buffer[4096];
    size_t start;
    size_t end;
} LineReader;

static bool read_line(LineReader *reader, Arena *arena, String *line)
{
    StringBuilder text = str_builder_create(arena, 128);
    for (;;) {
        while (reader->start < reader->end) {
            char c = reader->buffer[reader->start++];
            if (c == '\n') {
                *line = str_trim_suffix(str_builder_finish(&text), S("\r"));
                return true;
            }
            str_builder_append_char(&text, c);
        }
        int got = connection_receive(reader->connection, reader->buffer, sizeof reader->buffer);
        if (got <= 0) {
            return false;
        }
        reader->start = 0;
        reader->end = (size_t)got;
    }
}

static void reply(Connection *connection, const char *line)
{
    connection_send(connection, S(line));
}

// read_data reads a message up to the line ".", undoing the dot stuffing.
static String read_data(LineReader *reader, Arena *arena)
{
    StringBuilder data = str_builder_create(arena, 4096);
    String line = { 0 };
    while (read_line(reader, arena, &line) && !str_equal(line, S("."))) {
        str_builder_append(&data, str_trim_prefix(line, S(".")));
        str_builder_append(&data, S("\r\n"));
    }
    return str_builder_finish(&data);
}

// serve_smtp answers one session; AUTH is offered over TLS only, and a recipient containing "refused" is refused.
static void serve_smtp(Server *server, int client)
{
    SmtpServer *smtp = server->smtp;
    Arena *arena = smtp->arena;
    Connection connection = { .fd = client };
    LineReader reader = { .connection = &connection };
    bool ready = !smtp->implicit_tls || start_tls(server->tls, &connection);
    if (ready) {
        reply(&connection, "220 libmc-test ESMTP\r\n");
    }
    String line = { 0 };
    while (ready && read_line(&reader, arena, &line)) {
        bool secure = connection.tls != nullptr;
        if (str_starts_with(line, S("EHLO "))) {
            reply(&connection, "250-libmc-test\r\n");
            if (secure) {
                reply(&connection, "250-AUTH PLAIN\r\n");
            } else if (smtp->starttls) {
                reply(&connection, "250-STARTTLS\r\n");
            }
            reply(&connection, "250 8BITMIME\r\n");
        } else if (str_equal(line, S("STARTTLS")) && !secure && smtp->starttls) {
            reply(&connection, "220 2.0.0 ready\r\n");
            ready = start_tls(server->tls, &connection);
            reader.start = reader.end = 0;
        } else if (str_starts_with(line, S("AUTH PLAIN")) && secure) {
            smtp->auth = str_trim(str_trim_prefix(line, S("AUTH PLAIN")));
            if (smtp->auth.len == 0) {
                reply(&connection, "334 \r\n");
                ready = read_line(&reader, arena, &smtp->auth);
            }
            reply(&connection, "235 2.7.0 accepted\r\n");
        } else if (str_starts_with(line, S("MAIL FROM:"))) {
            smtp->mail_from = str_trim_prefix(line, S("MAIL FROM:"));
            reply(&connection, "250 2.1.0 ok\r\n");
        } else if (str_starts_with(line, S("RCPT TO:"))) {
            String recipient = str_trim_prefix(line, S("RCPT TO:"));
            if (str_contains(recipient, S("refused"))) {
                reply(&connection, "550 5.1.1 no such user\r\n");
            } else {
                strlist_push(arena, &smtp->recipients, recipient);
                reply(&connection, "250 2.1.5 ok\r\n");
            }
        } else if (str_equal(line, S("DATA"))) {
            reply(&connection, "354 end with a line holding a dot\r\n");
            smtp->data = read_data(&reader, arena);
            reply(&connection, "250 2.0.0 queued\r\n");
        } else if (str_equal(line, S("QUIT"))) {
            reply(&connection, "221 2.0.0 bye\r\n");
            break;
        } else {
            reply(&connection, "502 5.5.2 not implemented\r\n");
        }
    }
    end_tls(&connection);
    net_close(client);
}

// FtpSession is one control connection and the passive port it has open for data.
typedef struct FtpSession {
    Server *server;
    FtpServer *ftp;
    Connection control;
    LineReader reader;
    String directory;
    int passive;
    bool protect;
} FtpSession;

// ftp_path is name from directory, or from the root when it begins with /.
static String ftp_path(Arena *arena, String directory, String name)
{
    if (str_starts_with(name, S("/"))) {
        return str_trim_prefix(name, S("/"));
    }
    if (directory.len == 0) {
        return name;
    }
    return str_format(arena, "%.*s/%.*s", (int)directory.len, directory.data, (int)name.len, name.data);
}

static bool ftp_find(const FtpServer *ftp, String path, size_t *index)
{
    for (size_t i = 0; i < ftp->paths.count; i++) {
        if (str_equal(ftp->paths.items[i], path)) {
            *index = i;
            return true;
        }
    }
    return false;
}

static void ftp_store(FtpServer *ftp, String path, String contents)
{
    size_t index = 0;
    if (ftp_find(ftp, path, &index)) {
        ftp->contents.items[index] = contents;
        return;
    }
    strlist_push(ftp->arena, &ftp->paths, path);
    strlist_push(ftp->arena, &ftp->contents, contents);
}

static void ftp_listen(FtpSession *session)
{
    Arena *arena = session->ftp->arena;
    String address = { 0 };
    String host = { 0 };
    String port = { 0 };
    if (session->passive >= 0) {
        net_close(session->passive);
        session->passive = -1;
    }
    if (net_listen(S("127.0.0.1:0"), &session->passive, nullptr) != ERR_OK ||
        net_local_address(arena, session->passive, &address, nullptr) != ERR_OK) {
        reply(&session->control, "425 cannot open a data connection\r\n");
        return;
    }
    str_cut(address, ':', &host, &port);
    connection_send(&session->control,
                    str_format(arena, "229 Entering Extended Passive Mode (|||%.*s|)\r\n", (int)port.len, port.data));
}

// ftp_open_data accepts the client's connection to the passive port, through TLS after PROT P.
static bool ftp_open_data(FtpSession *session, Connection *data)
{
    *data = (Connection){ .fd = -1 };
    if (session->passive < 0 || !net_accept(session->passive, 5000, &data->fd)) {
        return false;
    }
    (void)net_set_timeouts(data->fd, 5000, nullptr);
    session->ftp->data_in_clear |= !session->protect;
    return !session->protect || start_tls(session->server->tls, data);
}

// ftp_close_data waits for the client's close_notify after sending its own,
// since curl ends a download that way and fails it when the connection is
// reset, as it is when the server closes before that close_notify arrives.
static void ftp_close_data(FtpSession *session, Connection *data)
{
    if (data->tls != nullptr && tls_shutdown(data->tls, nullptr) == ERR_OK) {
        char rest[256];
        while (connection_receive(data, rest, sizeof rest) > 0) {
        }
    }
    end_tls(data);
    if (data->fd >= 0) {
        net_close(data->fd);
    }
    net_close(session->passive);
    session->passive = -1;
}

static void ftp_send_file(FtpSession *session, String path)
{
    size_t index = 0;
    if (!ftp_find(session->ftp, path, &index)) {
        reply(&session->control, "550 no such file\r\n");
        return;
    }
    reply(&session->control, "150 sending\r\n");
    Connection data;
    if (ftp_open_data(session, &data)) {
        if (str_contains(path, S("slow"))) {
            clock_sleep_ms(SLOW_ROUTE_MS);
        }
        connection_send(&data, session->ftp->contents.items[index]);
    }
    ftp_close_data(session, &data);
    reply(&session->control, "226 sent\r\n");
}

static void ftp_receive_file(FtpSession *session, String path)
{
    reply(&session->control, "150 receiving\r\n");
    Connection data;
    StringBuilder contents = str_builder_create(session->ftp->arena, 256);
    if (ftp_open_data(session, &data)) {
        char buffer[4096];
        int got = 0;
        while ((got = connection_receive(&data, buffer, sizeof buffer)) > 0) {
            str_builder_append(&contents, (String){ buffer, (size_t)got });
        }
    }
    ftp_close_data(session, &data);
    ftp_store(session->ftp, path, str_builder_finish(&contents));
    reply(&session->control, "226 stored\r\n");
}

// ftp_send_names lists the files in the session's directory.
static void ftp_send_names(FtpSession *session)
{
    FtpServer *ftp = session->ftp;
    reply(&session->control, "150 listing\r\n");
    Connection data;
    if (ftp_open_data(session, &data)) {
        for (size_t i = 0; i < ftp->paths.count; i++) {
            String path = ftp->paths.items[i];
            size_t slash = 0;
            String directory = str_find_last_char(path, '/', &slash) ? str_slice(path, 0, slash) : S("");
            String name = directory.len > 0 ? str_slice(path, slash + 1, path.len) : path;
            if (str_equal(directory, session->directory)) {
                connection_send(&data, str_concat(ftp->arena, name, S("\r\n")));
            }
        }
    }
    ftp_close_data(session, &data);
    reply(&session->control, "226 listed\r\n");
}

// ftp_answer answers one command and reports whether the session goes on.
static bool ftp_answer(FtpSession *session, String command, String argument)
{
    FtpServer *ftp = session->ftp;
    Connection *control = &session->control;
    bool secure = control->tls != nullptr;
    String path = ftp_path(ftp->arena, session->directory, argument);
    if (str_equal(command, S("AUTH")) && ftp->auth_tls && !secure) {
        reply(control, "234 go ahead\r\n");
        session->reader.start = session->reader.end = 0;
        return start_tls(session->server->tls, control);
    }
    if (str_equal(command, S("USER"))) {
        ftp->user = argument;
        ftp->user_over_tls = secure;
        reply(control, "331 password, please\r\n");
    } else if (str_equal(command, S("PASS"))) {
        reply(control, "230 signed in\r\n");
    } else if (str_equal(command, S("PBSZ")) || str_equal(command, S("TYPE"))) {
        reply(control, "200 ok\r\n");
    } else if (str_equal(command, S("PROT"))) {
        session->protect = str_equal(argument, S("P"));
        reply(control, "200 ok\r\n");
    } else if (str_equal(command, S("PWD"))) {
        connection_send(control, str_format(ftp->arena, "257 \"/%.*s\"\r\n", (int)session->directory.len,
                                            session->directory.data));
    } else if (str_equal(command, S("CWD"))) {
        session->directory = str_trim_suffix(path, S("/"));
        reply(control, "250 ok\r\n");
    } else if (str_equal(command, S("EPSV"))) {
        ftp_listen(session);
    } else if (str_equal(command, S("SIZE"))) {
        size_t index = 0;
        if (ftp_find(ftp, path, &index)) {
            connection_send(control, str_format(ftp->arena, "213 %zu\r\n", ftp->contents.items[index].len));
        } else {
            reply(control, "550 no such file\r\n");
        }
    } else if (str_equal(command, S("RETR"))) {
        ftp_send_file(session, path);
    } else if (str_equal(command, S("STOR"))) {
        ftp_receive_file(session, path);
    } else if (str_equal(command, S("NLST"))) {
        ftp_send_names(session);
    } else if (str_equal(command, S("QUIT"))) {
        reply(control, "221 bye\r\n");
        return false;
    } else {
        reply(control, "502 not implemented\r\n");
    }
    return true;
}

static void serve_ftp(Server *server, int client)
{
    FtpServer *ftp = server->ftp;
    FtpSession session = { .server = server, .ftp = ftp, .control = { .fd = client }, .passive = -1 };
    session.reader.connection = &session.control;
    ftp->sessions++;
    bool ready = !ftp->implicit_tls || start_tls(server->tls, &session.control);
    if (ready) {
        reply(&session.control, "220 libmc-test FTP\r\n");
    }
    String line = { 0 };
    while (ready && read_line(&session.reader, ftp->arena, &line)) {
        String command = { 0 };
        String argument = { 0 };
        str_cut(line, ' ', &command, &argument);
        ready = ftp_answer(&session, command, argument);
    }
    if (session.passive >= 0) {
        net_close(session.passive);
    }
    end_tls(&session.control);
    net_close(client);
}

static void *accept_loop(void *argument)
{
    Server *server = argument;
    while (!atomic_load(&server->stopping)) {
        int client = -1;
        if (net_accept(server->listener, ACCEPT_POLL_MS, &client)) {
            (void)net_set_timeouts(client, 5000, nullptr);
            if (server->smtp != nullptr) {
                serve_smtp(server, client);
            } else if (server->ftp != nullptr) {
                serve_ftp(server, client);
            } else {
                serve(server, client);
            }
        }
    }
    return nullptr;
}

static const String HTTP2_ONLY[] = { { "h2", 2 } };
static const String HTTP1_ONLY[] = { { "http/1.1", 8 } };

// test_tls_server loads the test CA's certificate for the server. It offers
// only HTTP/2 with http2, else only HTTP/1.1, which the test server speaks.
static TlsServer *test_tls_server(Test *test, bool http2)
{
    TlsServerConfig config = {
        .protocols = http2 ? HTTP2_ONLY : HTTP1_ONLY,
        .protocol_count = 1,
    };
    TlsServer *tls = nullptr;
    Err err = { 0 };
    bool loaded = file_read_all(test->arena, S(SERVER_CERT_FILE), &config.certificate_pem, &err) == ERR_OK &&
                  file_read_all(test->arena, S(SERVER_KEY_FILE), &config.key_pem, &err) == ERR_OK &&
                  tls_server_create(&config, &tls, &err) == ERR_OK;
    test_check(test, loaded, err.msg);
    return tls;
}

static bool server_listen(Test *test, Server *server)
{
    atomic_init(&server->stopping, false);
    return net_listen(S("127.0.0.1:0"), &server->listener, nullptr) == ERR_OK &&
           net_local_address(test->arena, server->listener, &server->address, nullptr) == ERR_OK &&
           thread_start(&server->thread, accept_loop, server, nullptr) == ERR_OK;
}

static bool server_start(Test *test, Server *server, TlsServer *tls)
{
    *server = (Server){ .tls = tls };
    return server_listen(test, server);
}

static bool smtp_server_start(Test *test, Server *server, TlsServer *tls, SmtpServer *smtp)
{
    *server = (Server){ .tls = tls, .smtp = smtp };
    return server_listen(test, server);
}

static bool ftp_server_start(Test *test, Server *server, TlsServer *tls, FtpServer *ftp)
{
    *server = (Server){ .tls = tls, .ftp = ftp };
    return server_listen(test, server);
}

static void server_stop(Server *server)
{
    atomic_store(&server->stopping, true);
    thread_join(&server->thread);
    net_close(server->listener);
}

static const String LOCAL_HOSTS[] = { { "127.0.0.1", 9 }, { "localhost", 9 } };

// local_url is path on the server under the name its certificate holds, localhost.
static String local_url(Test *test, const Server *server, const char *scheme, const char *path)
{
    String host = { 0 };
    String port = { 0 };
    str_cut(server->address, ':', &host, &port);
    return str_format(test->arena, "%s://localhost:%.*s%s", scheme, (int)port.len, port.data, path);
}

static HttpClient *local_client(Test *test, size_t max_response_bytes, int64_t timeout_ms)
{
    HttpClientConfig config = {
        .ca_file = S(CA_FILE),
        .user_agent = S("libmc-test/1.0"),
        .allowed_hosts = LOCAL_HOSTS,
        .allowed_host_count = countof(LOCAL_HOSTS),
        .timeout_ms = timeout_ms,
        .max_response_bytes = max_response_bytes,
        .allow_plain_http = true,
    };
    HttpClient *client = nullptr;
    Err err = { 0 };
    test_check(test, http_client_create(&config, &client, &err) == ERR_OK, err.msg);
    return client;
}

[[nodiscard]] static Error send_to(Test *test, HttpClient *client, const Server *server, const char *method, const char *path,
                  String body, HttpResponse *response, Err *err)
{
    const HttpHeader headers[] = { { S("Accept"), S("application/json") } };
    HttpRequest request = {
        .method = S(method),
        .url = str_format(test->arena, "http://%.*s%s", (int)server->address.len, server->address.data, path),
        .headers = headers,
        .header_count = countof(headers),
        .body = body,
    };
    return http_send(client, test->arena, &request, response, err);
}

static void http_sends_requests_and_reads_responses(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/echo", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200, "answers 200");
    test_check(test, str_starts_with(response.body, S("GET /echo HTTP/1.1")), "sends the request line");
    test_check(test, str_contains(response.body, S("User-Agent: libmc-test/1.0")), "sends the user agent");
    test_check(test, str_contains(response.body, S("Accept: application/json")), "sends the request's headers");
    String value = { 0 };
    test_check(test, http_response_header(&response, S("x-test"), &value) && str_equal(value, S("yes")),
               "finds a response header ignoring case");

    test_check(test, send_to(test, client, &server, "POST", "/echo", S("{\"a\":1}"), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_ends_with(response.body, S("\r\n\r\n{\"a\":1}")), "sends a POST body");
    test_check(test, send_to(test, client, &server, "GET", "/chunked", S(""), &response, &err) == ERR_OK, err.msg);
    test_check_str(test, response.body, S("hello world"), "decodes a chunked body");
    test_check(test, send_to(test, client, &server, "GET", "/unavailable", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 503 && http_response_header(&response, S("Retry-After"), &value) &&
                         str_equal(value, S("120")),
               "a 503 is an answer with its Retry-After");
    http_client_destroy(client);
    server_stop(&server);
}

static void http_follows_redirects_within_the_rules(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/found", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200 && str_ends_with(response.url, S("/echo")), "follows a 302");
    test_check(test, send_to(test, client, &server, "POST", "/see-other", S("body"), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("GET /echo")) && !str_contains(response.body, S("body")),
               "a 303 continues as GET without the body");
    test_check(test, send_to(test, client, &server, "POST", "/temporary", S("body"), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("POST /echo")) && str_ends_with(response.body, S("body")),
               "a 307 repeats the method and body");
    test_check(test, send_to(test, client, &server, "GET", "/loop", S(""), &response, &err) == ERR_NETWORK,
               "too many redirects");
    test_check(test, send_to(test, client, &server, "GET", "/elsewhere", S(""), &response, &err) == ERR_INVALID_ARGUMENT,
               "a redirect to a host that is not allowed");
    test_check(test, str_contains(S(err.msg), S("elsewhere.invalid")), "names the host");
    http_client_destroy(client);
    server_stop(&server);
}

static void http_enforces_limits(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 1024, 200);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/large", S(""), &response, &err) == ERR_UNSUPPORTED,
               "a body over the limit, by its Content-Length");
    test_check(test, send_to(test, client, &server, "GET", "/large-chunked", S(""), &response, &err) == ERR_UNSUPPORTED,
               "a chunked body that grows over the limit");
    test_check(test, send_to(test, client, &server, "HEAD", "/large", S(""), &response, &err) == ERR_OK &&
                         response.status == 200,
               "a HEAD whose Content-Length is over the limit");
    test_check(test, send_to(test, client, &server, "GET", "/slow", S(""), &response, &err) == ERR_TIMEOUT,
               "a response slower than the timeout");
    test_check(test, send_to(test, client, &server, "BREW", "/echo", S(""), &response, &err) == ERR_INVALID_ARGUMENT,
               "an unsupported method");
    HttpRequest elsewhere = { .url = S("http://example.com/") };
    test_check(test, http_send(client, test->arena, &elsewhere, &response, &err) == ERR_INVALID_ARGUMENT,
               "a host that is not allowed");
    HttpRequest malformed = { .url = S("http://[::1") };
    test_check(test, http_send(client, test->arena, &malformed, &response, &err) == ERR_INVALID_ARGUMENT,
               "a malformed URL");
    const HttpHeader injected[] = { { S("X-A"), S("1\r\nX-B: 2") } };
    HttpRequest injection = { .url = S("http://127.0.0.1/"), .headers = injected, .header_count = 1 };
    test_check(test, http_send(client, test->arena, &injection, &response, &err) == ERR_INVALID_ARGUMENT,
               "a header value with a line break");
    http_client_destroy(client);
    server_stop(&server);

    HttpClientConfig https_only = { .ca_file = S(CA_FILE) };
    test_check(test, http_client_create(&https_only, &client, &err) == ERR_OK, err.msg);
    HttpRequest plain = { .url = S("http://127.0.0.1/") };
    test_check(test, http_send(client, test->arena, &plain, &response, &err) == ERR_INVALID_ARGUMENT,
               "plain http is refused unless allowed");
    http_client_destroy(client);
    HttpClientConfig missing = { .ca_file = S("tests/fixtures/tls/missing.pem") };
    test_check(test, http_client_create(&missing, &client, &err) == ERR_NOT_FOUND, "a missing CA bundle");
}

static void http_verifies_tls_certificates(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    Server server;
    test_check(test, server_start(test, &server, tls), "starts the TLS server");
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    HttpRequest trusted = { .url = local_url(test, &server, "https", "/echo") };
    test_check(test, http_send(client, test->arena, &trusted, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200 && str_starts_with(response.body, S("GET /echo")),
               "talks TLS to a server the CA vouches for");
    http_client_destroy(client);

    // The server's own certificate is no trust anchor for itself: the CA that signed it is missing.
    HttpClientConfig untrusted = {
        .ca_file = S(SERVER_CERT_FILE),
        .allowed_hosts = LOCAL_HOSTS,
        .allowed_host_count = countof(LOCAL_HOSTS),
    };
    test_check(test, http_client_create(&untrusted, &client, &err) == ERR_OK, err.msg);
    test_check(test, http_send(client, test->arena, &trusted, &response, &err) == ERR_NETWORK,
               "refuses a server whose issuer is not trusted");
    http_client_destroy(client);
    server_stop(&server);
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
}

// http_leaves_crypto_to_others_in_the_process checks that a TLS server,
// whose key lives in mbedTLS's process-wide store, still serves after the
// last HTTP client is gone.
static void http_leaves_crypto_to_others_in_the_process(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    HttpClient *client = local_client(test, 0, 0);
    if (client != nullptr) {
        http_client_destroy(client);
    }
    Server server;
    test_check(test, tls != nullptr && server_start(test, &server, tls), "starts the TLS server");
    client = local_client(test, 0, 0);
    HttpRequest request = { .url = local_url(test, &server, "https", "/echo") };
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200, "the server's key still signs");
    http_client_destroy(client);
    server_stop(&server);
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
}

static const char HTTP2_BODY[] = "served over HTTP/2";

[[nodiscard]] static Error answer_http2(void *context, Arena *arena, const HttpServerRequest *request,
                                        HttpServerResponse *response, Err *err)
{
    response->body = S(HTTP2_BODY);
    return ERR_OK;
}

// http_speaks_http2_when_offered runs libmc's HTTP server offering only h2,
// so the request fails unless the client speaks HTTP/2.
static void http_speaks_http2_when_offered(Test *test)
{
    TlsServer *tls = test_tls_server(test, true);
    HttpServerConfig config = { .address = S("127.0.0.1:0"), .handler = answer_http2, .tls = tls };
    HttpServer *server = nullptr;
    Err err = { 0 };
    test_check(test, tls != nullptr && http_server_create(&config, &server, &err) == ERR_OK, err.msg);
    if (server == nullptr) {
        if (tls != nullptr) {
            tls_server_destroy(tls);
        }
        return;
    }
    HttpClient *client = local_client(test, 0, 0);
    HttpResponse response = { 0 };
    HttpRequest request = { .url = str_format(test->arena, "https://localhost:%u/", http_server_port(server)) };
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200, "answers 200");
    test_check_str(test, response.body, S(HTTP2_BODY), "reads a response over HTTP/2");
    http_client_destroy(client);
    http_server_destroy(server);
    tls_server_destroy(tls);
}

static void http_decodes_compressed_responses(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 1024, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    test_check(test, send_to(test, client, &server, "GET", "/echo", S(""), &response, &err) == ERR_OK, err.msg);
    test_check(test, str_contains(response.body, S("Accept-Encoding: ")) && str_contains(response.body, S("gzip")),
               "asks for gzip");
    test_check(test, send_to(test, client, &server, "GET", "/gzip", S(""), &response, &err) == ERR_OK, err.msg);
    test_check_str(test, response.body, S("hello, gzip"), "decodes a gzip body");
    test_check(test, send_to(test, client, &server, "GET", "/gzip-large", S(""), &response, &err) == ERR_UNSUPPORTED,
               "a small gzip body that decodes to more than the limit");
    http_client_destroy(client);
    server_stop(&server);
}

static String file_contents(Test *test, String path)
{
    String contents = { 0 };
    return file_read_all(test->arena, path, &contents, nullptr) == ERR_OK ? contents : S("<unreadable>");
}

static void http_streams_bodies_through_files(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 1024, 0);
    HttpResponse response = { 0 };
    Err err = { 0 };
    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-http-"), &dir, &err) == ERR_OK, err.msg);
    String source_path = path_join(test->arena, dir, S("source"));
    test_check(test, file_write_all(source_path, S("0123456789"), 0600, &err) == ERR_OK, err.msg);
    int source = -1;
    test_check(test, file_open_read(source_path, &source, &err) == ERR_OK, err.msg);

    HttpFileBody part = { .fd = source, .offset = 2, .length = 5 };
    HttpRequest put = { .method = S("PUT"), .url = str_format(test->arena, "http://%.*s/echo", (int)server.address.len,
                                                             server.address.data),
                        .body_file = &part };
    test_check(test, http_send(client, test->arena, &put, &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("PUT /echo")), "sends the method");
    test_check(test, str_contains(response.body, S("Content-Length: 5")), "declares the range's length");
    test_check(test, str_ends_with(response.body, S("\r\n\r\n23456")), "sends the range of the file");

    put.method = S("POST");
    put.url = str_format(test->arena, "http://%.*s/temporary", (int)server.address.len, server.address.data);
    test_check(test, http_send(client, test->arena, &put, &response, &err) == ERR_OK, err.msg);
    test_check(test, str_starts_with(response.body, S("POST /echo")) && str_ends_with(response.body, S("23456")),
               "a 307 sends the file again");

    String sink_path = path_join(test->arena, dir, S("sink"));
    int sink = -1;
    test_check(test, file_create(sink_path, 0600, &sink, &err) == ERR_OK, err.msg);
    HttpDownload download = { .fd = sink };
    HttpRequest get = { .url = str_format(test->arena, "http://%.*s/large", (int)server.address.len, server.address.data),
                        .download = &download };
    test_check(test, http_send(client, test->arena, &get, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 200 && response.body.len == 0 && response.downloaded == 4096,
               "a download past max_response_bytes goes to the file");
    test_check_str(test, file_contents(test, sink_path), str_repeat(test->arena, S("x"), 4096), "the file holds the body");

    test_check(test, file_truncate(sink, 0, &err) == ERR_OK && file_seek(sink, 0, &err) == ERR_OK, err.msg);
    get.url = str_format(test->arena, "http://%.*s/unavailable", (int)server.address.len, server.address.data);
    test_check(test, http_send(client, test->arena, &get, &response, &err) == ERR_OK, err.msg);
    test_check(test, response.status == 503 && str_equal(response.body, S("try later")) && response.downloaded == 0,
               "an error status keeps its body in memory");
    test_check_str(test, file_contents(test, sink_path), S(""), "and leaves the file alone");

    file_close(sink);
    file_close(source);
    (void)file_remove(sink_path, nullptr);
    (void)file_remove(source_path, nullptr);
    (void)dir_remove(dir, nullptr);
    http_client_destroy(client);
    server_stop(&server);
}

typedef struct DelayedCancel {
    Cancel *cancel;
    int64_t delay_ms;
} DelayedCancel;

static void *cancel_later(void *argument)
{
    DelayedCancel *delayed = argument;
    clock_sleep_ms(delayed->delay_ms);
    cancel_request(delayed->cancel);
    return nullptr;
}

static void http_cancels_and_times_out_per_request(Test *test)
{
    Server server;
    test_check(test, server_start(test, &server, nullptr), "starts the server");
    HttpClient *client = local_client(test, 0, 200);
    HttpResponse response = { 0 };
    Err err = { 0 };
    String slow = str_format(test->arena, "http://%.*s/slow", (int)server.address.len, server.address.data);
    HttpRequest request = { .url = slow, .timeout_ms = 5000 };
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK && response.status == 200,
               "a request's own timeout outlasts the client's");
    request.timeout_ms = -1;
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_OK, "a request without a timeout");

    Cancel cancel;
    cancel_init(&cancel);
    request.cancel = &cancel;
    DelayedCancel delayed = { .cancel = &cancel, .delay_ms = 100 };
    Thread canceller;
    test_check(test, thread_start(&canceller, cancel_later, &delayed, nullptr) == ERR_OK, "starts the canceller");
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_CANCELLED, "cancelled mid-request");
    thread_join(&canceller);
    test_check(test, http_send(client, test->arena, &request, &response, &err) == ERR_CANCELLED,
               "a request on a cancelled token does not start");
    cancel_destroy(&cancel);

    // The server gzips /gzip whatever is asked; a caller that sets Accept-Encoding gets those bytes as sent.
    const HttpHeader identity[] = { { S("Accept-Encoding"), S("identity") } };
    HttpRequest raw = { .url = str_format(test->arena, "http://%.*s/gzip", (int)server.address.len, server.address.data),
                        .headers = identity,
                        .header_count = countof(identity) };
    test_check(test, http_send(client, test->arena, &raw, &response, &err) == ERR_OK, err.msg);
    test_check(test, str_equal(response.body, (String){ (const char *)GZIP_HELLO, sizeof GZIP_HELLO }),
               "a body the caller did not ask to decode stays encoded");
    http_client_destroy(client);
    server_stop(&server);
}

// http_reaches_live_hosts runs only with LIBMC_LIVE_TESTS=1, since the other tests never use the network.
static void http_reaches_live_hosts(Test *test)
{
    String live = { 0 };
    if (!env_get(test->arena, S("LIBMC_LIVE_TESTS"), &live) || !str_equal(live, S("1"))) {
        return;
    }
    HttpClientConfig config = { .user_agent = S("libmc-test/1.0 (https://github.com/sangar/libmc)") };
    HttpClient *client = nullptr;
    Err err = { 0 };
    test_check(test, http_client_create(&config, &client, &err) == ERR_OK, err.msg);
    const char *const urls[] = {
        "https://api.nb.no/catalog/v1/items?q=isbn:9788205379576",
        "https://www.wikidata.org/entity/Q42",
        "https://openlibrary.org/isbn/9780140328721.json",
    };
    for (size_t i = 0; i < countof(urls); i++) {
        HttpResponse response = { 0 };
        HttpRequest request = { .url = S(urls[i]) };
        Error e = http_send(client, test->arena, &request, &response, &err);
        printf("  live %s: %s, status %d, %zu bytes, %.*s\n", urls[i], error_name(e), response.status,
               response.body.len, (int)response.url.len, response.url.data);
        test_check(test, e == ERR_OK && response.status == 200, urls[i]);
    }
    HttpResponse response = { 0 };
    HttpRequest wrong_host = { .url = S("https://wrong.host.badssl.com/") };
    test_check(test, http_send(client, test->arena, &wrong_host, &response, &err) == ERR_NETWORK,
               "refuses a certificate for another host");
    http_client_destroy(client);
}

static MailMessage short_message(const MailAddress *to)
{
    return (MailMessage){
        .from = { .address = S("nora@example.no") },
        .to = to,
        .to_count = 1,
        .subject = S("Et dukkehjem"),
        .text = S("Hei"),
    };
}

static void smtp_sends_mail_with_starttls(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    SmtpServer smtp = { .starttls = true, .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, tls, &smtp), "starts the mail server");
    SmtpConfig config = {
        .url = local_url(test, &server, "smtp", ""),
        .username = S("nora"),
        .password = S("dukkehjem"),
        .ca_file = S(CA_FILE),
    };
    const MailAddress to[] = { { S("Torvald Helmer"), S("torvald@example.no") }, { .address = S("kristine@example.no") } };
    const MailAddress cc[] = { { S("Dr. Rank"), S("rank@example.no") } };
    const MailAddress bcc[] = { { .address = S("krogstad@example.no") } };
    const MailAttachment attachments[] = {
        { S("brev.txt"), S("text/plain"), S("Hello, PDF") },
        { .filename = S("søknad.pdf"), .data = S("%PDF") },
    };
    MailMessage message = {
        .from = { S("Nora Helmer"), S("nora@example.no") },
        .to = to,
        .to_count = countof(to),
        .cc = cc,
        .cc_count = countof(cc),
        .bcc = bcc,
        .bcc_count = countof(bcc),
        .subject = S("Søknad om ferie"),
        .text = S("Hei\nlinje to"),
        .html = S("<p>Hei</p>"),
        .attachments = attachments,
        .attachment_count = countof(attachments),
    };
    Err err = { 0 };
    test_check(test, smtp_send(&config, &message, &err) == ERR_OK, err.msg);
    server_stop(&server);
    String data = smtp.data;
    test_check_str(test, smtp.auth, S("AG5vcmEAZHVra2VoamVt"), "signs in with PLAIN after STARTTLS");
    test_check_str(test, smtp.mail_from, S("<nora@example.no>"), "the envelope sender");
    test_check_str(test, str_join(test->arena, smtp.recipients, S(" ")),
                   S("<torvald@example.no> <kristine@example.no> <rank@example.no> <krogstad@example.no>"),
                   "every recipient, bcc included");
    test_check(test, str_contains(data, S("From: \"Nora Helmer\" <nora@example.no>\r\n")), "the From header");
    test_check(test, str_contains(data, S("To: \"Torvald Helmer\" <torvald@example.no>,\r\n kristine@example.no\r\n")),
               "the To header, folded between mailboxes");
    test_check(test, str_contains(data, S("Cc: \"Dr. Rank\" <rank@example.no>\r\n")), "the Cc header");
    test_check(test, !str_contains(data, S("krogstad")), "bcc is not named in the message");
    test_check(test, str_contains(data, S("Subject: =?UTF-8?Q?S=C3=B8knad_om_ferie?=\r\n")), "a UTF-8 subject");
    test_check(test, str_contains(data, S("Date: ")) && str_contains(data, S("Message-ID: <")), "Date and Message-ID");
    test_check(test, str_contains(data, S("multipart/alternative")) && str_contains(data, S("Hei\r\nlinje to\r\n")) &&
                         str_contains(data, S("<p>Hei</p>")),
               "the text and HTML as alternatives");
    test_check(test, str_contains(data, S("filename=\"brev.txt\"")) && str_contains(data, S("SGVsbG8sIFBERg==")),
               "an attachment in base64");
    test_check(test, str_contains(data, S("filename*=UTF-8''s%C3%B8knad.pdf")), "a UTF-8 file name");
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
    arena_destroy(smtp.arena);
}

static void smtp_sends_mail_over_tls_from_the_start(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    SmtpServer smtp = { .implicit_tls = true, .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, tls, &smtp), "starts the mail server");
    SmtpConfig config = { .url = local_url(test, &server, "smtps", ""), .ca_file = S(CA_FILE) };
    const MailAddress to[] = { { .address = S("torvald@example.no") } };
    MailMessage message = short_message(to);
    Err err = { 0 };
    test_check(test, smtp_send(&config, &message, &err) == ERR_OK, err.msg);
    server_stop(&server);
    test_check(test, str_contains(smtp.data, S("Subject: Et dukkehjem\r\n")), "delivers the message");
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
    arena_destroy(smtp.arena);
}

static void smtp_refuses_servers_without_starttls(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    SmtpServer smtp = { .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, tls, &smtp), "starts the mail server");
    SmtpConfig config = {
        .url = local_url(test, &server, "smtp", ""),
        .username = S("nora"),
        .password = S("dukkehjem"),
        .ca_file = S(CA_FILE),
    };
    const MailAddress to[] = { { .address = S("torvald@example.no") } };
    MailMessage message = short_message(to);
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_NETWORK, "a server that offers no STARTTLS");
    server_stop(&server);
    test_check(test, smtp.auth.len == 0 && smtp.mail_from.len == 0, "sends neither the password nor the mail");
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
    arena_destroy(smtp.arena);
}

static void smtp_reports_a_refused_recipient(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    SmtpServer smtp = { .starttls = true, .arena = arena_create(0) };
    Server server;
    test_check(test, smtp_server_start(test, &server, tls, &smtp), "starts the mail server");
    SmtpConfig config = { .url = local_url(test, &server, "smtp", ""), .ca_file = S(CA_FILE) };
    const MailAddress to[] = { { .address = S("refused@example.no") } };
    MailMessage message = short_message(to);
    Err err = { 0 };
    test_check(test, smtp_send(&config, &message, &err) == ERR_NETWORK, "the server refuses the recipient");
    test_check(test, str_contains(S(err.msg), S("550")), err.msg);
    server_stop(&server);
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
    arena_destroy(smtp.arena);
}

static void smtp_rejects_malformed_messages(Test *test)
{
    SmtpConfig config = { .url = S("smtp://localhost:1"), .ca_file = S(CA_FILE) };
    const MailAddress to[] = { { .address = S("torvald@example.no") } };
    MailMessage message = short_message(to);
    message.subject = S("Hei\r\nBcc: krogstad@example.no");
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_INVALID_ARGUMENT, "a line break in the subject");
    message = short_message(to);
    message.from.address = S("nora");
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_INVALID_ARGUMENT, "an address without @");
    message = short_message(to);
    message.to_count = 0;
    test_check(test, smtp_send(&config, &message, nullptr) == ERR_INVALID_ARGUMENT, "no recipients");
    message = short_message(to);
    SmtpConfig http = { .url = S("http://localhost:1/"), .ca_file = S(CA_FILE) };
    test_check(test, smtp_send(&http, &message, nullptr) == ERR_INVALID_ARGUMENT, "a URL that is not SMTP");
    SmtpConfig missing = { .url = S("smtp://localhost:1"), .ca_file = S("tests/fixtures/tls/missing.pem") };
    test_check(test, smtp_send(&missing, &message, nullptr) == ERR_NOT_FOUND, "a missing CA bundle");
}

// FTP_FILE has line breaks of both kinds and a NUL, which only a binary transfer keeps.
static const char FTP_FILE[] = "linje 1\r\nlinje 2\n\0slutt";

static FtpClient *ftp_client(Test *test, const Server *server, const char *scheme, bool allow_plain_ftp)
{
    FtpClientConfig config = {
        .url = local_url(test, server, scheme, ""),
        .username = S("nora"),
        .password = S("dukkehjem"),
        .ca_file = S(CA_FILE),
        .allow_plain_ftp = allow_plain_ftp,
    };
    FtpClient *client = nullptr;
    Err err = { 0 };
    test_check(test, ftp_client_create(&config, &client, &err) == ERR_OK, err.msg);
    return client;
}

static void ftp_moves_files_over_auth_tls(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    FtpServer ftp = { .auth_tls = true, .arena = arena_create(0) };
    Server server;
    test_check(test, ftp_server_start(test, &server, tls, &ftp), "starts the FTP server");
    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-ftp-"), &dir, nullptr) == ERR_OK, "creates a directory");
    String original = path_join(test->arena, dir, S("original"));
    String copy = path_join(test->arena, dir, S("copy"));
    String contents = { FTP_FILE, sizeof FTP_FILE - 1 };
    test_check(test, file_write_all(original, contents, 0644, nullptr) == ERR_OK, "writes the file to upload");
    FtpClient *client = ftp_client(test, &server, "ftp", false);
    Err err = { 0 };
    int fd = -1;
    test_check(test, file_open_read(original, &fd, nullptr) == ERR_OK, "opens the file to upload");
    test_check(test, ftp_upload(client, nullptr, S("inbox/rapport å.txt"), fd, &err) == ERR_OK, err.msg);
    file_close(fd);
    StringList names = { 0 };
    test_check(test, ftp_list(client, nullptr, test->arena, S("inbox"), &names, &err) == ERR_OK, err.msg);
    test_check_str(test, str_join(test->arena, names, S(",")), S("rapport å.txt"), "lists the uploaded file");
    test_check(test, file_create(copy, 0644, &fd, nullptr) == ERR_OK, "creates the copy");
    test_check(test, ftp_download(client, nullptr, S("inbox/missing.txt"), fd, nullptr) == ERR_NOT_FOUND,
               "a file that is not on the server");
    test_check(test, ftp_download(client, nullptr, S("/inbox/rapport å.txt"), fd, &err) == ERR_OK, err.msg);
    file_close(fd);
    String downloaded = { 0 };
    test_check(test, file_read_all(test->arena, copy, &downloaded, nullptr) == ERR_OK, "reads the download");
    test_check_str(test, downloaded, contents, "the file comes back byte for byte");
    ftp_client_destroy(client);
    server_stop(&server);
    test_check_str(test, ftp.user, S("nora"), "signs in");
    test_check(test, ftp.user_over_tls, "signs in over TLS");
    test_check(test, !ftp.data_in_clear, "moves files over TLS");
    test_check(test, ftp.sessions == 1, "keeps one connection for every call");
    (void)dir_remove_all(dir, nullptr);
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
    arena_destroy(ftp.arena);
}

static void ftp_downloads_over_tls_from_the_start(Test *test)
{
    TlsServer *tls = test_tls_server(test, false);
    FtpServer ftp = { .implicit_tls = true, .arena = arena_create(0) };
    ftp_store(&ftp, S("velkommen.txt"), S("Hei"));
    Server server;
    test_check(test, ftp_server_start(test, &server, tls, &ftp), "starts the FTP server");
    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-ftp-"), &dir, nullptr) == ERR_OK, "creates a directory");
    String copy = path_join(test->arena, dir, S("velkommen.txt"));
    int fd = -1;
    test_check(test, file_create(copy, 0644, &fd, nullptr) == ERR_OK, "creates the copy");
    FtpClient *client = ftp_client(test, &server, "ftps", false);
    Err err = { 0 };
    test_check(test, ftp_download(client, nullptr, S("velkommen.txt"), fd, &err) == ERR_OK, err.msg);
    file_close(fd);
    ftp_client_destroy(client);
    server_stop(&server);
    String downloaded = { 0 };
    test_check(test, file_read_all(test->arena, copy, &downloaded, nullptr) == ERR_OK, "reads the download");
    test_check_str(test, downloaded, S("Hei"), "downloads the file");
    (void)dir_remove_all(dir, nullptr);
    if (tls != nullptr) {
        tls_server_destroy(tls);
    }
    arena_destroy(ftp.arena);
}

static void ftp_refuses_servers_without_tls_unless_allowed(Test *test)
{
    FtpServer ftp = { .arena = arena_create(0) };
    ftp_store(&ftp, S("velkommen.txt"), S("Hei"));
    Server server;
    test_check(test, ftp_server_start(test, &server, nullptr, &ftp), "starts the FTP server");
    FtpClient *client = ftp_client(test, &server, "ftp", false);
    StringList names = { 0 };
    test_check(test, ftp_list(client, nullptr, test->arena, S(""), &names, nullptr) == ERR_NETWORK,
               "a server without TLS");
    ftp_client_destroy(client);
    test_check(test, ftp.user.len == 0, "signs in to no server without TLS");
    client = ftp_client(test, &server, "ftp", true);
    Err err = { 0 };
    test_check(test, ftp_list(client, nullptr, test->arena, S(""), &names, &err) == ERR_OK, err.msg);
    test_check_str(test, str_join(test->arena, names, S(",")), S("velkommen.txt"), "lists in the clear when allowed");
    ftp_client_destroy(client);
    server_stop(&server);
    arena_destroy(ftp.arena);
}

static void ftp_stops_when_cancelled(Test *test)
{
    FtpServer ftp = { .arena = arena_create(0) };
    ftp_store(&ftp, S("slow.txt"), S("late"));
    Server server;
    test_check(test, ftp_server_start(test, &server, nullptr, &ftp), "starts the FTP server");
    String dir = { 0 };
    test_check(test, dir_create_temp(test->arena, S("mc-ftp-"), &dir, nullptr) == ERR_OK, "creates a directory");
    int fd = -1;
    test_check(test, file_create(path_join(test->arena, dir, S("slow.txt")), 0644, &fd, nullptr) == ERR_OK,
               "creates the copy");
    FtpClient *client = ftp_client(test, &server, "ftp", true);
    Cancel cancel;
    cancel_init(&cancel);
    DelayedCancel delayed = { .cancel = &cancel, .delay_ms = 100 };
    Thread canceller;
    test_check(test, thread_start(&canceller, cancel_later, &delayed, nullptr) == ERR_OK, "starts the canceller");
    test_check(test, ftp_download(client, &cancel, S("slow.txt"), fd, nullptr) == ERR_CANCELLED,
               "cancelled mid-transfer");
    thread_join(&canceller);
    test_check(test, ftp_download(client, &cancel, S("slow.txt"), fd, nullptr) == ERR_CANCELLED,
               "a call on a cancelled token does not start");
    cancel_destroy(&cancel);
    file_close(fd);
    ftp_client_destroy(client);
    server_stop(&server);
    (void)dir_remove_all(dir, nullptr);
    arena_destroy(ftp.arena);
}

static void ftp_rejects_bad_arguments(Test *test)
{
    FtpClientConfig config = { .url = S("ftp://localhost:1"), .ca_file = S(CA_FILE) };
    FtpClient *client = nullptr;
    test_check(test, ftp_client_create(&config, &client, nullptr) == ERR_OK, "creates a client");
    test_check(test, ftp_download(client, nullptr, S("a.txt\r\nDELE b.txt"), -1, nullptr) == ERR_INVALID_ARGUMENT,
               "a line break in a path");
    test_check(test, ftp_upload(client, nullptr, S("inbox/"), -1, nullptr) == ERR_INVALID_ARGUMENT,
               "a remote path that names no file");
    ftp_client_destroy(client);
    config.url = S("http://localhost:1");
    test_check(test, ftp_client_create(&config, &client, nullptr) == ERR_OK, "creates a client");
    StringList names = { 0 };
    test_check(test, ftp_list(client, nullptr, test->arena, S(""), &names, nullptr) == ERR_INVALID_ARGUMENT,
               "a URL that is not FTP");
    ftp_client_destroy(client);
    config.ca_file = S("tests/fixtures/tls/missing.pem");
    test_check(test, ftp_client_create(&config, &client, nullptr) == ERR_NOT_FOUND, "a missing CA bundle");
}

const TestCase HTTP_TESTS[] = {
    { "http_sends_requests_and_reads_responses", http_sends_requests_and_reads_responses },
    { "http_follows_redirects_within_the_rules", http_follows_redirects_within_the_rules },
    { "http_enforces_limits", http_enforces_limits },
    { "http_verifies_tls_certificates", http_verifies_tls_certificates },
    { "http_speaks_http2_when_offered", http_speaks_http2_when_offered },
    { "http_leaves_crypto_to_others_in_the_process", http_leaves_crypto_to_others_in_the_process },
    { "http_decodes_compressed_responses", http_decodes_compressed_responses },
    { "http_streams_bodies_through_files", http_streams_bodies_through_files },
    { "http_cancels_and_times_out_per_request", http_cancels_and_times_out_per_request },
    { "http_reaches_live_hosts", http_reaches_live_hosts },
    { nullptr, nullptr },
};

const TestCase SMTP_TESTS[] = {
    { "smtp_sends_mail_with_starttls", smtp_sends_mail_with_starttls },
    { "smtp_sends_mail_over_tls_from_the_start", smtp_sends_mail_over_tls_from_the_start },
    { "smtp_refuses_servers_without_starttls", smtp_refuses_servers_without_starttls },
    { "smtp_reports_a_refused_recipient", smtp_reports_a_refused_recipient },
    { "smtp_rejects_malformed_messages", smtp_rejects_malformed_messages },
    { nullptr, nullptr },
};

const TestCase FTP_TESTS[] = {
    { "ftp_moves_files_over_auth_tls", ftp_moves_files_over_auth_tls },
    { "ftp_downloads_over_tls_from_the_start", ftp_downloads_over_tls_from_the_start },
    { "ftp_refuses_servers_without_tls_unless_allowed", ftp_refuses_servers_without_tls_unless_allowed },
    { "ftp_stops_when_cancelled", ftp_stops_when_cancelled },
    { "ftp_rejects_bad_arguments", ftp_rejects_bad_arguments },
    { nullptr, nullptr },
};
