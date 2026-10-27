#pragma once

#include <stddef.h>

#include "mc/core/arena.h"
#include "mc/core/error.h"
#include "mc/text/str.h"

// TLS on mbedTLS, mostly for servers. A TlsServer holds the certificate a
// server presents: an HttpServer, when HttpServerConfig's tls field takes it,
// or one that reads and writes in turn through TlsConnection. A client that
// speaks a protocol of its own over TLS, as a test does to a server, uses
// tls_connect; for HTTPS there is http.h.
typedef struct TlsServer TlsServer;

typedef struct TlsServerConfig {
    String certificate_pem; // the certificate, then any intermediates
    String key_pem;         // its key
    // protocols are offered by ALPN, the server's preference first; none
    // offers h2 and http/1.1, as an HttpServer speaks, and acme-tls/1, by
    // which an ACME client answers TLS-ALPN-01 challenges.
    const String *protocols;
    size_t protocol_count;
} TlsServerConfig;

// tls_server_create copies config. It offers TLS 1.2 and 1.3. A certificate
// or key that does not parse, or a key that is not the certificate's, is
// ERR_PLATFORM. The caller owns the server.
[[nodiscard]] Error tls_server_create(const TlsServerConfig *config, TlsServer **server, Err *err);

// tls_server_set_certificate replaces what the server presents from the
// next handshake on, as when a certificate is renewed; connections keep the
// one they started with. It may run while the server serves.
[[nodiscard]] Error tls_server_set_certificate(TlsServer *server, String certificate_pem, String key_pem, Err *err);

// tls_server_destroy releases the server once nothing serves with it.
void tls_server_destroy(TlsServer *server);

// tls_self_signed makes a P-256 key and a certificate for host signed by it,
// valid from now for days, both PEM in arena: for development and tests, and
// for a server to start with until a CA's certificate comes.
[[nodiscard]] Error tls_self_signed(Arena *arena, String host, unsigned days, String *certificate_pem, String *key_pem,
                                    Err *err);

// TlsConnection is one side of TLS over a blocking socket, for a server that
// reads and writes in turn, such as one for mail or files, for STARTTLS on a
// connection that began in the clear, and for a client.
typedef struct TlsConnection TlsConnection;

typedef struct TlsClientConfig {
    String host;   // the name the server's certificate must hold, sent by SNI
    String ca_pem; // the certificates the server's must chain to
    const String *protocols; // offered by ALPN, preferred first; none offers none
    size_t protocol_count;
} TlsClientConfig;

// tls_accept runs the handshake on fd, a connected socket the caller keeps
// owning and closes after tls_close; it waits as long as the socket's
// timeouts allow. A failed handshake is ERR_NETWORK. The caller owns the
// connection.
[[nodiscard]] Error tls_accept(TlsServer *server, int fd, TlsConnection **connection, Err *err);

// tls_connect runs a client's handshake on fd, as tls_accept does a
// server's, offering TLS 1.2 and 1.3. A certificate that does not chain to
// config's or does not name its host fails the handshake.
[[nodiscard]] Error tls_connect(const TlsClientConfig *config, int fd, TlsConnection **connection, Err *err);

// tls_protocol is the protocol ALPN chose, empty when the client named none.
String tls_protocol(const TlsConnection *connection);

// tls_read waits for up to capacity bytes; *got is 0 once the peer closed.
[[nodiscard]] Error tls_read(TlsConnection *connection, void *buffer, size_t capacity, size_t *got, Err *err);

// tls_write sends all of data.
[[nodiscard]] Error tls_write(TlsConnection *connection, String data, Err *err);

// tls_shutdown tells the peer nothing more will be sent, so a caller can
// read on until the peer closes too, as some clients want before the socket
// goes.
[[nodiscard]] Error tls_shutdown(TlsConnection *connection, Err *err);

// tls_close tells the peer nothing more will be sent, if it has not been
// told, and releases the connection, not its socket.
void tls_close(TlsConnection *connection);
