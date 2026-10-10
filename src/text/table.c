#include "mc/text/table.h"

#include "mc/text/utf8.h"

enum { PADDING = 2 };

Table table_create(Arena *arena)
{
    return (Table){ .arena = arena };
}

void table_row(Table *table, size_t count, const String *cells)
{
    StringList row = { 0 };
    for (size_t i = 0; i < count; i++) {
        strlist_push(table->arena, &row, cells[i]);
    }
    table->rows = arena_grow(table->arena, table->rows, &table->capacity, table->count, sizeof *table->rows);
    table->rows[table->count++] = row;
}

static bool padded(const Table *table, size_t row, size_t column)
{
    return row < table->count && table->rows[row].count > column + 1;
}

// column_widths gives each padded cell the width of its block: the run of
// adjacent rows padded in that column.
static size_t *column_widths(const Table *table, size_t columns)
{
    size_t *widths = arena_push(table->arena, arena_size_mul(arena_size_mul(table->count, columns), sizeof *widths));
    for (size_t column = 0; column < columns; column++) {
        size_t block_start = 0;
        size_t block_width = 0;
        for (size_t row = 0; row <= table->count; row++) {
            if (padded(table, row, column)) {
                block_width = max_size(block_width, utf8_count(table->rows[row].items[column]));
                continue;
            }
            for (size_t b = block_start; b < row; b++) {
                widths[b * columns + column] = block_width + PADDING;
            }
            block_start = row + 1;
            block_width = 0;
        }
    }
    return widths;
}

String table_format(const Table *table)
{
    size_t columns = 0;
    for (size_t row = 0; row < table->count; row++) {
        columns = max_size(columns, table->rows[row].count);
    }
    size_t *widths = column_widths(table, columns);
    StringBuilder builder = str_builder_create(table->arena, 256);
    for (size_t row = 0; row < table->count; row++) {
        StringList cells = table->rows[row];
        for (size_t column = 0; column < cells.count; column++) {
            String cell = cells.items[column];
            str_builder_append(&builder, cell);
            if (!padded(table, row, column)) {
                continue;
            }
            for (size_t pad = utf8_count(cell); pad < widths[row * columns + column]; pad++) {
                str_builder_append_char(&builder, ' ');
            }
        }
        str_builder_append_char(&builder, '\n');
    }
    return str_builder_finish(&builder);
}
