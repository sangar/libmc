#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// CsvReader reads RFC 4180 records from text one at a time. Fields are
// separated by commas and records by CRLF or LF. A field in double quotes may
// hold commas, line breaks and "" for one quote. A leading UTF-8 byte order
// mark is skipped. Records may have different field counts.
typedef struct CsvReader {
    String text;
    size_t position;
    // line is the 1-based line where the next record starts, for messages.
    size_t line;
} CsvReader;

// CsvRow is one record's fields. Each field is a view into the text, except
// a quoted field holding "", which is copied into the arena with the quotes
// undoubled. Reuse one CsvRow across records so its array is allocated once.
typedef struct CsvRow {
    String *fields;
    size_t count;
    size_t capacity;
} CsvRow;

CsvReader csv_reader_create(String text);

// csv_read_row reads the next record into row; found is false at the end of
// the text. A quote that is never closed, or text between a closing quote
// and the next separator, is ERR_PARSE with the line in err.
[[nodiscard]] Error csv_read_row(CsvReader *reader, Arena *arena, CsvRow *row, bool *found, Err *err);
