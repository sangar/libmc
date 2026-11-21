#include "test.h"

#include "mc/term/input.h"

static Event decode_event(Test *test, const char *bytes, size_t *used)
{
    Event event = { 0 };
    test_check(test, input_decode(S(bytes), true, &event, used), "decodes");
    return event;
}

static void input_decodes_keys(Test *test)
{
    size_t used;
    Event event = decode_event(test, "jk", &used);
    test_check(test, event.key == KEY_CHAR && event.codepoint == 'j' && used == 1, "a plain key, one at a time");
    event = decode_event(test, "\x01", &used);
    test_check(test, event.key == KEY_CHAR && event.ctrl && event.codepoint == 'a', "ctrl-a");
    event = decode_event(test, "\r", &used);
    test_check(test, event.key == KEY_ENTER, "enter");
    event = decode_event(test, "é", &used);
    test_check(test, event.key == KEY_CHAR && event.codepoint == 0xE9 && used == 2, "a UTF-8 character");
    event = decode_event(test, "\x1b[A", &used);
    test_check(test, event.key == KEY_UP && used == 3, "an arrow");
    event = decode_event(test, "\x1b[1;5B", &used);
    test_check(test, event.key == KEY_DOWN && event.ctrl, "an arrow with ctrl");
    event = decode_event(test, "\x1b[5~", &used);
    test_check(test, event.key == KEY_PAGE_UP, "page up");
    event = decode_event(test, "\x1bx", &used);
    test_check(test, event.key == KEY_CHAR && event.alt && event.codepoint == 'x' && used == 2, "alt-x");
    test_check_str(test, event.raw, S("\x1bx"), "raw keeps the bytes to pass on");
}

static void input_waits_for_the_rest_of_a_sequence(Test *test)
{
    Event event;
    size_t used;
    test_check(test, !input_decode(S("\x1b"), false, &event, &used), "a lone escape may start a sequence");
    test_check(test, !input_decode(S("\x1b[1;5"), false, &event, &used), "half a sequence");
    test_check(test, input_decode(S("\x1b"), true, &event, &used) && event.key == KEY_ESCAPE,
               "nothing more came: the escape key");
}

static void input_decodes_mouse_and_paste(Test *test)
{
    size_t used;
    Event event = decode_event(test, "\x1b[<0;12;5M", &used);
    test_check(test, event.key == KEY_MOUSE && event.mouse == MOUSE_PRESS && event.x == 11 && event.y == 4,
               "a click, at 0-based coordinates");
    event = decode_event(test, "\x1b[<65;3;3M", &used);
    test_check(test, event.mouse == MOUSE_WHEEL_DOWN, "the wheel");
    event = decode_event(test, "\x1b[200~ls -la\x1b[201~rest", &used);
    test_check(test, event.key == KEY_PASTE, "a bracketed paste");
    test_check_str(test, event.raw, S("\x1b[200~ls -la\x1b[201~"), "the paste is one event");
}

static void input_takes_a_paste_in_parts(Test *test)
{
    Event event;
    size_t used;
    test_check(test, input_decode(S("\x1b[200~first part"), true, &event, &used), "decodes");
    test_check(test, input_paste_continues(event), "a paste without its end marker continues");
    test_check_str(test, input_paste_text(event.raw), S("first part"), "its text");
    String text;
    bool ended;
    used = input_paste_rest(S("more q x\x1b[20"), &text, &ended);
    test_check(test, !ended && used == 8, "a tail that may start the end marker waits");
    test_check_str(test, text, S("more q x"), "the text so far");
    used = input_paste_rest(S("\x1b[201~q"), &text, &ended);
    test_check(test, ended && used == 6 && text.len == 0, "the end marker ends the paste");
}

static void input_decodes_terminal_strings_and_focus(Test *test)
{
    size_t used;
    Event event = decode_event(test, "\x1b]11;rgb:0000/0000/0000\aq", &used);
    test_check(test, event.key == KEY_UNKNOWN && used == 24 && !input_string_continues(event),
               "an OSC reply ended by BEL is one event, BEL included");
    event = decode_event(test, "\x1bP1$r0m\x1b\\q", &used);
    test_check(test, event.key == KEY_UNKNOWN && used == 9 && !input_string_continues(event), "a DCS reply ended by ESC \\");
    test_check(test, !input_decode(S("\x1b]11;rgb:00"), false, &event, &used), "half a reply waits for the rest");
    event = decode_event(test, "\x1b]", &used);
    test_check(test, event.key == KEY_CHAR && event.alt && event.codepoint == ']', "ESC ] alone is alt-]");
    event = decode_event(test, "\x1b]11;rgb:00", &used);
    test_check(test, event.key == KEY_UNKNOWN && used == 11 && input_string_continues(event),
               "a reply cut short by a forced decode continues");
    event = decode_event(test, "\x1b]11;rgb:0/0/0\x1b", &used);
    test_check(test, used == 14 && input_string_continues(event), "an escape that could start the end stays for the next read");
    event = decode_event(test, "\x1bPa", &used);
    test_check(test, event.key == KEY_CHAR && event.alt && event.codepoint == 'P' && used == 2,
               "alt-P followed by a key does not start a string");
    test_check(test, !input_decode(S("\x1b]"), false, &event, &used), "ESC ] may start a reply");
    bool ended;
    used = input_string_rest(S("00/0000\x1b"), &ended);
    test_check(test, !ended && used == 7, "an escape that could start the end waits");
    used = input_string_rest(S("\x1b\\x"), &ended);
    test_check(test, ended && used == 2, "ESC \\ ends the reply, and what follows is input again");
    used = input_string_rest(S("/0000\ax"), &ended);
    test_check(test, ended && used == 6, "so does BEL, which is not taken for ctrl-g");
    event = decode_event(test, "\x1b[I", &used);
    test_check(test, event.key == KEY_FOCUS_IN && used == 3, "focus in");
    event = decode_event(test, "\x1b[O", &used);
    test_check(test, event.key == KEY_FOCUS_OUT, "focus out");
}

const TestCase TERM_INPUT_TESTS[] = {
    { "input_decodes_terminal_strings_and_focus", input_decodes_terminal_strings_and_focus },
    { "input_decodes_keys", input_decodes_keys },
    { "input_waits_for_the_rest_of_a_sequence", input_waits_for_the_rest_of_a_sequence },
    { "input_decodes_mouse_and_paste", input_decodes_mouse_and_paste },
    { "input_takes_a_paste_in_parts", input_takes_a_paste_in_parts },
    { nullptr, nullptr },
};
