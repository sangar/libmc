#pragma once

#include <stddef.h>

#include "mc/net/tls.h"

// TlsSession is one connection's TLS over a non-blocking socket an event
// loop owns, as the HTTP server drives it.
typedef struct TlsSession TlsSession;

// TlsResult is how far a call that does not wait got. A session may need to
// write to finish a receive, or read to finish a send, while it handshakes.
typedef enum TlsResult {
    TLS_DONE,       // some bytes moved
    TLS_WANT_READ,  // call again once the socket is readable
    TLS_WANT_WRITE, // call again once the socket is writable
    TLS_CLOSED,     // the peer closed, or the connection failed and is over
} TlsResult;

// tls_session_create starts the server side of TLS on fd, a connected
// non-blocking socket the caller keeps owning and closes after
// tls_session_destroy. The session lives in arena.
TlsSession *tls_session_create(Arena *arena, TlsServer *server, int fd);
// tls_session_destroy sends close_notify if the socket takes it now, and
// releases what the session holds outside its arena.
void tls_session_destroy(TlsSession *session);

// tls_try_receive reads up to capacity bytes of plaintext that have arrived.
TlsResult tls_try_receive(TlsSession *session, char *buffer, size_t capacity, size_t *got);
// tls_try_send sends what of data the connection takes now. After
// TLS_WANT_READ or TLS_WANT_WRITE it must be called again with the same data.
TlsResult tls_try_send(TlsSession *session, String data, size_t *sent);
