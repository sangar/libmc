#pragma once

#include "mc/core/base.h"
#include "mc/text/str.h"

// Outbox holds the bytes waiting to go to one peer, so whoever produces them
// never waits for the peer: any thread puts, one thread takes and sends. Its
// bytes live in two arenas that take turns, so it holds at most twice what
// waited at once.
typedef struct Outbox Outbox;

// outbox_create makes an outbox that holds at most limit bytes waiting.
Outbox *outbox_create(size_t limit);
// outbox_destroy needs every thread to be done with the outbox.
void outbox_destroy(Outbox *outbox);

// outbox_put queues bytes and returns true. It returns false once the outbox
// is closed, and closes it when more than limit bytes would be waiting, as
// for a peer that has stopped reading.
bool outbox_put(Outbox *outbox, String bytes);
// outbox_put_parts queues count parts together, so no other put comes
// between them, or none of them, as outbox_put would.
bool outbox_put_parts(Outbox *outbox, const String *parts, size_t count);
// outbox_take waits for bytes and returns all that are waiting; they stay
// valid until the next take. It returns empty once the outbox is closed and
// what was put before is taken.
String outbox_take(Outbox *outbox);
void outbox_close(Outbox *outbox);
