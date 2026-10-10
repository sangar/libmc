#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// Terminal is the controlling terminal, /dev/tty, in raw mode: keys arrive a
// byte at a time without echo, and output goes out as written. Working on the
// terminal rather than on stdin and stdout keeps an interactive picker usable
// inside a command substitution such as vim "$(picker)".
typedef struct Terminal Terminal;

// terminal_is_terminal reports whether fd is a terminal, such as stdout that
// is not redirected.
bool terminal_is_terminal(int fd);

// terminal_open opens the controlling terminal in raw mode. Window size
// changes are reported through terminal_wait; for that the calling thread
// blocks their signal, so open the terminal before starting threads.
[[nodiscard]] Error terminal_open(Terminal **terminal, Err *err);
// terminal_close restores the mode the terminal had and closes it.
void terminal_close(Terminal *terminal);

typedef struct TerminalSize {
    int columns;
    int rows;
} TerminalSize;

// terminal_size is 80 by 24 when the system does not know the size.
TerminalSize terminal_size(Terminal *terminal);
[[nodiscard]] Error terminal_write(Terminal *terminal, String text, Err *err);

// TerminalReady says what ended a terminal_wait; several can be set at once.
typedef struct TerminalReady {
    bool input;
    bool woken;
    bool resized;
} TerminalReady;

// terminal_wait waits up to timeout_ms, without limit when negative, for
// input, a change of window size or a terminal_wake. Nothing set is a timeout.
TerminalReady terminal_wait(Terminal *terminal, int timeout_ms);
// terminal_read_byte waits up to timeout_ms for a byte of input; false on
// timeout or once the terminal has gone away.
bool terminal_read_byte(Terminal *terminal, int timeout_ms, unsigned char *byte);
// terminal_wake makes the current or next terminal_wait return with woken
// set. Any thread may call it.
void terminal_wake(Terminal *terminal);
