#pragma once

#include "mc/term/input.h"
#include "mc/term/style.h"

// List is a list to pick one item from, height rows at a time, scrolled to
// keep the cursor in view. List list = { .items = items, .height = 10 } is a
// list with the cursor on the first item.
typedef struct List {
    StringList items;
    size_t height;
    size_t cursor;
    size_t offset;
} List;

typedef enum ListResult { LIST_CONTINUE, LIST_SELECTED, LIST_CANCELLED } ListResult;

// list_handle moves the cursor for the arrow keys, j and k, ctrl-n and
// ctrl-p, home and end, g and G, and page up and down. Enter selects the
// item under the cursor; escape and ctrl-c cancel.
ListResult list_handle(List *list, Event event);
// list_append appends the rows in view, one per line, the cursor's marked
// with "> " and drawn in cursor_color.
void list_append(StringBuilder *out, ColorDepth depth, const List *list, Rgb cursor_color);
