#include "mc/term/input.h"

#include "mc/text/utf8.h"

enum { ESC = 0x1b, DEL = 0x7f, PARAM_LIMIT = 4 };

static const String PASTE_START = { "\x1b[200~", 6 };
static const String PASTE_END = { "\x1b[201~", 6 };

static Event plain(Key key, String raw)
{
    return (Event) { .key = key, .raw = raw };
}

// CSI parameters: up to PARAM_LIMIT numbers separated by ';', 0 when empty.
typedef struct {
    int values[PARAM_LIMIT];
    size_t count;
    bool private_marker; // a leading '<', as SGR mouse reports have
} Params;

static Params parse_params(String text)
{
    Params params = { 0 };
    size_t start = 0;
    if (text.len > 0 && text.data[0] == '<') {
        params.private_marker = true;
        start = 1;
    }
    params.count = 1;
    for (size_t i = start; i < text.len; i++) {
        char c = text.data[i];
        if (c == ';' && params.count < PARAM_LIMIT) {
            params.count++;
        } else if (c >= '0' && c <= '9') {
            int *value = &params.values[params.count - 1];
            *value = *value < 100000 ? *value * 10 + (c - '0') : *value;
        }
    }
    return params;
}

static Event mouse_event(Params params, char final, String raw)
{
    Event event = plain(KEY_MOUSE, raw);
    int code = params.values[0];
    event.x = params.values[1] - 1;
    event.y = params.values[2] - 1;
    event.ctrl = (code & 16) != 0;
    event.alt = (code & 8) != 0;
    event.mouse_button = code & 3;
    if ((code & 64) != 0) {
        event.mouse = (code & 1) != 0 ? MOUSE_WHEEL_DOWN : MOUSE_WHEEL_UP;
    } else if ((code & 32) != 0) {
        event.mouse = MOUSE_DRAG;
    } else {
        event.mouse = final == 'M' ? MOUSE_PRESS : MOUSE_RELEASE;
    }
    return event;
}

static Key tilde_key(int number)
{
    switch (number) {
    case 1:
    case 7:
        return KEY_HOME;
    case 2:
        return KEY_INSERT;
    case 3:
        return KEY_DELETE;
    case 4:
    case 8:
        return KEY_END;
    case 5:
        return KEY_PAGE_UP;
    case 6:
        return KEY_PAGE_DOWN;
    default:
        return KEY_UNKNOWN;
    }
}

static Key letter_key(char final)
{
    switch (final) {
    case 'A':
        return KEY_UP;
    case 'B':
        return KEY_DOWN;
    case 'C':
        return KEY_RIGHT;
    case 'D':
        return KEY_LEFT;
    case 'H':
        return KEY_HOME;
    case 'F':
        return KEY_END;
    case 'Z':
        return KEY_BACKTAB;
    default:
        return KEY_UNKNOWN;
    }
}

static Event csi_event(String body, char final, String raw)
{
    Params params = parse_params(body);
    if (params.private_marker && (final == 'M' || final == 'm')) {
        return mouse_event(params, final, raw);
    }
    Event event = plain(final == '~' ? tilde_key(params.values[0]) : letter_key(final), raw);
    // A second parameter is 1 plus the modifier bits: 1 shift, 2 alt, 4 ctrl.
    int modifiers = params.count > 1 && params.values[1] > 0 ? params.values[1] - 1 : 0;
    event.alt = (modifiers & 2) != 0;
    event.ctrl = (modifiers & 4) != 0;
    return event;
}

static bool decode_paste(String bytes, bool complete, Event *event, size_t *used)
{
    size_t end;
    if (!str_find(bytes, PASTE_END, &end)) {
        if (!complete) {
            return false;
        }
        end = bytes.len - PASTE_END.len;
    }
    *used = end + PASTE_END.len;
    *event = plain(KEY_PASTE, str_slice(bytes, 0, *used));
    return true;
}

// decode_escape decodes what starts with ESC.
static bool decode_escape(String bytes, bool complete, Event *event, size_t *used)
{
    if (bytes.len == 1) {
        *used = 1;
        *event = plain(KEY_ESCAPE, bytes);
        return complete;
    }
    char second = bytes.data[1];
    if (str_starts_with(bytes, PASTE_START)) {
        return decode_paste(bytes, complete, event, used);
    }
    if (second == '[' || second == 'O') {
        // ESC [ parameters final, or ESC O final for keypad keys.
        for (size_t i = 2; i < bytes.len; i++) {
            unsigned char c = (unsigned char)bytes.data[i];
            if (c >= 0x40 && c <= 0x7e) {
                *used = i + 1;
                *event = csi_event(str_slice(bytes, 2, i), (char)c, str_slice(bytes, 0, *used));
                return true;
            }
        }
        if (!complete) {
            return false;
        }
        *used = bytes.len;
        *event = plain(KEY_UNKNOWN, bytes);
        return true;
    }
    // ESC followed by a key is that key with alt.
    size_t rest_used;
    if (!input_decode(str_slice(bytes, 1, bytes.len), complete, event, &rest_used)) {
        return false;
    }
    *used = rest_used + 1;
    event->alt = true;
    event->raw = str_slice(bytes, 0, *used);
    return true;
}

String input_paste_text(String raw)
{
    return str_trim_suffix(str_trim_prefix(raw, PASTE_START), PASTE_END);
}

bool input_paste_continues(Event event)
{
    return event.key == KEY_PASTE && !str_ends_with(event.raw, PASTE_END);
}

size_t input_paste_rest(String bytes, String *text, bool *ended)
{
    size_t end;
    *ended = str_find(bytes, PASTE_END, &end);
    if (*ended) {
        *text = str_slice(bytes, 0, end);
        return end + PASTE_END.len;
    }
    size_t keep = min_size(PASTE_END.len - 1, bytes.len);
    while (keep > 0 && !str_starts_with(PASTE_END, str_slice(bytes, bytes.len - keep, bytes.len))) {
        keep--;
    }
    *text = str_slice(bytes, 0, bytes.len - keep);
    return text->len;
}

bool input_decode(String bytes, bool complete, Event *event, size_t *used)
{
    unsigned char first = (unsigned char)bytes.data[0];
    if (first == ESC) {
        return decode_escape(bytes, complete, event, used);
    }
    *used = 1;
    String raw = str_slice(bytes, 0, 1);
    switch (first) {
    case '\r':
    case '\n':
        *event = plain(KEY_ENTER, raw);
        return true;
    case '\t':
        *event = plain(KEY_TAB, raw);
        return true;
    case DEL:
    case 0x08:
        *event = plain(KEY_BACKSPACE, raw);
        return true;
    default:
        break;
    }
    if (first < 0x20) {
        *event = plain(KEY_CHAR, raw);
        event->codepoint = first == 0 ? ' ' : (uint32_t)('a' + first - 1);
        event->ctrl = true;
        return true;
    }
    size_t length = first < 0x80 ? 1 : first >= 0xf0 ? 4 : first >= 0xe0 ? 3 : 2;
    if (bytes.len < length && !complete) {
        return false;
    }
    size_t position = 0;
    uint32_t codepoint;
    utf8_decode(bytes, &position, &codepoint);
    *used = position;
    *event = plain(KEY_CHAR, str_slice(bytes, 0, position));
    event->codepoint = codepoint;
    return true;
}
