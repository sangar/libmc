#include "mc/encoding/csv.h"

#include <string.h>

enum { CSV_ROW_INITIAL_CAPACITY = 16 };

static const String BYTE_ORDER_MARK = { "\xEF\xBB\xBF", 3 };

CsvReader csv_reader_create(String text)
{
    return (CsvReader){ .text = str_trim_prefix(text, BYTE_ORDER_MARK), .line = 1 };
}

static void push_field(CsvRow *row, Arena *arena, String field)
{
    if (row->count == row->capacity) {
        if (row->capacity == 0) {
            row->capacity = CSV_ROW_INITIAL_CAPACITY;
            row->fields = arena_push(arena, sizeof(String) * row->capacity);
        } else {
            row->fields = arena_grow(arena, row->fields, &row->capacity, row->count, sizeof(String));
        }
    }
    row->fields[row->count++] = field;
}

// at_record_end reports whether position is at a line break or the end of the text.
static bool at_record_end(const CsvReader *reader)
{
    size_t p = reader->position;
    return p == reader->text.len || reader->text.data[p] == '\n' ||
           (reader->text.data[p] == '\r' && p + 1 < reader->text.len && reader->text.data[p + 1] == '\n');
}

static void skip_record_end(CsvReader *reader)
{
    if (reader->position < reader->text.len && reader->text.data[reader->position] == '\r') {
        reader->position++;
    }
    if (reader->position < reader->text.len) {
        reader->position++;
        reader->line++;
    }
}

static String read_unquoted(CsvReader *reader)
{
    size_t start = reader->position;
    while (!at_record_end(reader) && reader->text.data[reader->position] != ',') {
        reader->position++;
    }
    return str_slice(reader->text, start, reader->position);
}

// undouble copies a quoted field's inside with each "" turned into ".
static String undouble(Arena *arena, String inside)
{
    char *out = arena_push(arena, inside.len);
    size_t length = 0;
    for (size_t i = 0; i < inside.len; i++) {
        out[length++] = inside.data[i];
        i += inside.data[i] == '"';
    }
    return (String){ out, length };
}

[[nodiscard]] static Error read_quoted(CsvReader *reader, Arena *arena, String *field, Err *err)
{
    size_t opened_on = reader->line;
    size_t start = ++reader->position;
    bool doubled = false;
    for (;;) {
        const char *quote = memchr(reader->text.data + reader->position, '"', reader->text.len - reader->position);
        if (quote == nullptr) {
            return err_set(err, ERR_PARSE, "csv: line %zu: quoted field is never closed", opened_on);
        }
        size_t at = (size_t)(quote - reader->text.data);
        reader->line += str_count_char(str_slice(reader->text, reader->position, at), '\n');
        reader->position = at + 1;
        if (reader->position < reader->text.len && reader->text.data[reader->position] == '"') {
            doubled = true;
            reader->position++;
            continue;
        }
        String inside = str_slice(reader->text, start, at);
        *field = doubled ? undouble(arena, inside) : inside;
        break;
    }
    if (reader->position < reader->text.len && reader->text.data[reader->position] != ',' && !at_record_end(reader)) {
        return err_set(err, ERR_PARSE, "csv: line %zu: text after a closing quote", reader->line);
    }
    return ERR_OK;
}

Error csv_read_row(CsvReader *reader, Arena *arena, CsvRow *row, bool *found, Err *err)
{
    row->count = 0;
    *found = reader->position < reader->text.len;
    if (!*found) {
        return ERR_OK;
    }
    for (;;) {
        String field = { 0 };
        if (reader->position < reader->text.len && reader->text.data[reader->position] == '"') {
            Error e = read_quoted(reader, arena, &field, err);
            if (e) {
                return e;
            }
        } else {
            field = read_unquoted(reader);
        }
        push_field(row, arena, field);
        if (at_record_end(reader)) {
            skip_record_end(reader);
            return ERR_OK;
        }
        reader->position++;
    }
}
