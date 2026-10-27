#include "mc/term/logbuf.h"

#include <string.h>

enum { LINE_LIMIT = 4096, SEQUENCE_LIMIT = 64, ESC = 0x1b, BEL = 0x07 };

typedef enum {
    PARSE_TEXT,
    PARSE_ESCAPE,
    PARSE_CSI,
    PARSE_OSC,
    PARSE_OSC_ESCAPE,
    PARSE_SKIP_ONE, // the byte after ESC ( and its kin, which pick a character set
} ParseState;

struct LogBuffer {
    Arena *arena;    // the LogBuffer and its ring of lines
    Arena *bytes[2]; // line text; compaction copies the kept lines to the other
    size_t active;
    size_t max_lines;
    size_t max_bytes;
    String *lines; // line n is lines[n % max_lines]
    uint64_t first;
    uint64_t end;
    char partial[LINE_LIMIT];
    size_t partial_len;
    bool carriage_return; // a '\r' waits to see whether '\n' follows
    ParseState state;
    char sequence[SEQUENCE_LIMIT];
    size_t sequence_len;
};

LogBuffer *logbuf_create(size_t max_lines, size_t max_bytes)
{
    Arena *arena = arena_create(0);
    LogBuffer *log = arena_push(arena, sizeof *log);
    log->arena = arena;
    log->bytes[0] = arena_create(0);
    log->bytes[1] = arena_create(0);
    log->max_lines = max_lines > 0 ? max_lines : 1;
    log->max_bytes = max_bytes;
    log->lines = arena_push(arena, arena_size_mul(log->max_lines, sizeof(String)));
    return log;
}

void logbuf_destroy(LogBuffer *log)
{
    arena_destroy(log->bytes[0]);
    arena_destroy(log->bytes[1]);
    arena_destroy(log->arena);
}

static String *slot(const LogBuffer *log, uint64_t number)
{
    return &log->lines[number % log->max_lines];
}

static String copy_bytes(Arena *arena, String text)
{
    char *data = arena_push_aligned(arena, text.len + 1, 1);
    if (text.len > 0) {
        memcpy(data, text.data, text.len);
    }
    return (String) { data, text.len };
}

// compact keeps the newest lines that fit in half of max_bytes, copied to
// the other arena, and frees the rest at once.
static void compact(LogBuffer *log)
{
    size_t budget = log->max_bytes / 2;
    size_t kept = 0;
    uint64_t first = log->end;
    while (first > log->first && kept + slot(log, first - 1)->len <= budget) {
        first--;
        kept += slot(log, first)->len;
    }
    Arena *target = log->bytes[1 - log->active];
    arena_reset(target);
    for (uint64_t n = first; n < log->end; n++) {
        *slot(log, n) = copy_bytes(target, *slot(log, n));
    }
    arena_reset(log->bytes[log->active]);
    log->active = 1 - log->active;
    log->first = first;
}

static void add_line(LogBuffer *log, String text)
{
    if (log->end - log->first == log->max_lines) {
        log->first++;
    }
    *slot(log, log->end) = copy_bytes(log->bytes[log->active], text);
    log->end++;
    if (arena_bytes_used(log->bytes[log->active]) > log->max_bytes) {
        compact(log);
    }
}

static void finish_line(LogBuffer *log)
{
    add_line(log, (String) { log->partial, log->partial_len });
    log->partial_len = 0;
}

static void append(LogBuffer *log, String bytes)
{
    if (log->partial_len + bytes.len > LINE_LIMIT) {
        finish_line(log);
    }
    if (bytes.len <= LINE_LIMIT) {
        memcpy(log->partial + log->partial_len, bytes.data, bytes.len);
        log->partial_len += bytes.len;
    }
}

static void end_sequence(LogBuffer *log, char final)
{
    log->state = PARSE_TEXT;
    if (final == 'm' && log->sequence_len < SEQUENCE_LIMIT) {
        append(log, (String) { log->sequence, log->sequence_len });
    } else if (final == 'G') {
        // Moving to a column rewrites the line, the way spinners use it.
        log->partial_len = 0;
    }
}

static void take_text_byte(LogBuffer *log, char c)
{
    if (log->carriage_return) {
        log->carriage_return = false;
        if (c == '\n') {
            finish_line(log);
            return;
        }
        log->partial_len = 0;
    }
    unsigned char byte = (unsigned char)c;
    if (c == '\n') {
        finish_line(log);
    } else if (c == '\r') {
        log->carriage_return = true;
    } else if (byte == ESC) {
        log->state = PARSE_ESCAPE;
    } else if (c == '\t' || (byte >= 0x20 && byte != 0x7f)) {
        append(log, (String) { &c, 1 });
    }
}

static void take_byte(LogBuffer *log, char c)
{
    unsigned char byte = (unsigned char)c;
    switch (log->state) {
    case PARSE_TEXT:
        take_text_byte(log, c);
        return;
    case PARSE_ESCAPE:
        if (c == '[') {
            log->state = PARSE_CSI;
            log->sequence[0] = (char)ESC;
            log->sequence[1] = '[';
            log->sequence_len = 2;
        } else if (c == ']') {
            log->state = PARSE_OSC;
        } else if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#') {
            log->state = PARSE_SKIP_ONE;
        } else {
            log->state = PARSE_TEXT;
        }
        return;
    case PARSE_CSI:
        if (log->sequence_len < SEQUENCE_LIMIT) {
            log->sequence[log->sequence_len] = c;
        }
        log->sequence_len++;
        if (byte >= 0x40 && byte <= 0x7e) {
            end_sequence(log, c);
        }
        return;
    case PARSE_OSC:
        log->state = byte == BEL ? PARSE_TEXT : byte == ESC ? PARSE_OSC_ESCAPE : PARSE_OSC;
        return;
    case PARSE_OSC_ESCAPE:
        log->state = c == '\\' ? PARSE_TEXT : PARSE_OSC;
        return;
    case PARSE_SKIP_ONE:
        log->state = PARSE_TEXT;
        return;
    }
}

void logbuf_write(LogBuffer *log, String output)
{
    for (size_t i = 0; i < output.len; i++) {
        take_byte(log, output.data[i]);
    }
}

void logbuf_note(LogBuffer *log, String text)
{
    if (log->partial_len > 0) {
        finish_line(log);
    }
    log->carriage_return = false;
    log->state = PARSE_TEXT;
    add_line(log, text);
}

void logbuf_clear(LogBuffer *log)
{
    arena_reset(log->bytes[0]);
    arena_reset(log->bytes[1]);
    log->first = log->end;
    log->partial_len = 0;
    log->carriage_return = false;
}

uint64_t logbuf_first(const LogBuffer *log)
{
    return log->first;
}

uint64_t logbuf_end(const LogBuffer *log)
{
    return log->end;
}

String logbuf_line(const LogBuffer *log, uint64_t number)
{
    return *slot(log, number);
}

String logbuf_partial(const LogBuffer *log)
{
    return (String) { log->partial, log->partial_len };
}
