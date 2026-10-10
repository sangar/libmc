#pragma once

#include "mc/text/str.h"

// Table lays rows of cells out in aligned columns, as Go's tabwriter does
// with a padding of two. A cell followed by another cell is padded to the
// widest such cell in its column across the adjacent rows that also have
// one; a row's last cell is never padded. Widths count code points.
typedef struct Table {
    Arena *arena;
    StringList *rows;
    size_t count;
    size_t capacity;
} Table;

Table table_create(Arena *arena);
// table_row adds a row of count cells; the cells are views and must outlive the table.
void table_row(Table *table, size_t count, const String *cells);
// table_format returns every row followed by '\n'.
String table_format(const Table *table);
