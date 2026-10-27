#pragma once

#include "mc/text/str.h"

typedef enum Key {
    KEY_CHAR,
    KEY_ENTER,
    KEY_TAB,
    KEY_BACKTAB,
    KEY_BACKSPACE,
    KEY_ESCAPE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_MOUSE,
    KEY_PASTE,
    KEY_UNKNOWN,
} Key;

typedef enum MouseAction {
    MOUSE_PRESS,
    MOUSE_RELEASE,
    MOUSE_DRAG,
    MOUSE_WHEEL_UP,
    MOUSE_WHEEL_DOWN,
} MouseAction;

// Event is one key press, mouse action or paste. raw is the bytes it came
// from, a view into the decoded input, for passing it on to a program.
typedef struct Event {
    Key key;
    uint32_t codepoint; // for KEY_CHAR, lower case for a control key
    bool ctrl;
    bool alt;
    MouseAction mouse;
    int mouse_button; // 0 left, 1 middle, 2 right
    int x;            // 0-based column of a mouse event
    int y;
    String raw;
} Event;

// input_decode decodes the event at the start of bytes and stores how many
// bytes it took. It returns false when bytes hold only the start of an event
// and more may follow; with complete set, nothing more will come, so a lone
// escape is the escape key. bytes must not be empty.
bool input_decode(String bytes, bool complete, Event *event, size_t *used);

// A paste longer than what is read at once arrives in parts: input_decode
// makes the first part a KEY_PASTE without its end, and the bytes that follow
// are more of it until its end marker.

// input_paste_text is the text of a KEY_PASTE without its markers.
String input_paste_text(String raw);
// input_paste_continues reports whether a KEY_PASTE was cut short.
bool input_paste_continues(Event event);
// input_paste_rest takes what continues a paste: the text up to the end
// marker, or all of bytes but a tail that could start the end marker. It
// stores that text and whether the paste ended, and returns the bytes taken.
size_t input_paste_rest(String bytes, String *text, bool *ended);
