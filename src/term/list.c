#include "mc/term/list.h"

#include <stdint.h>

static void move_to(List *list, size_t cursor)
{
    if (list->items.count == 0) {
        return;
    }
    list->cursor = min_size(cursor, list->items.count - 1);
    if (list->cursor < list->offset) {
        list->offset = list->cursor;
    }
    if (list->height > 0 && list->cursor >= list->offset + list->height) {
        list->offset = list->cursor - list->height + 1;
    }
}

static void move_up(List *list, size_t rows)
{
    move_to(list, list->cursor > rows ? list->cursor - rows : 0);
}

static void move_down(List *list, size_t rows)
{
    move_to(list, list->cursor + rows);
}

static size_t page(const List *list)
{
    return list->height > 1 ? list->height - 1 : 1;
}

static bool is_char(Event event, uint32_t codepoint)
{
    return event.key == KEY_CHAR && !event.ctrl && !event.alt && event.codepoint == codepoint;
}

static bool is_ctrl(Event event, uint32_t codepoint)
{
    return event.key == KEY_CHAR && event.ctrl && event.codepoint == codepoint;
}

ListResult list_handle(List *list, Event event)
{
    if (event.key == KEY_ENTER) {
        return list->items.count > 0 ? LIST_SELECTED : LIST_CONTINUE;
    }
    if (event.key == KEY_ESCAPE || is_ctrl(event, 'c')) {
        return LIST_CANCELLED;
    }
    if (event.key == KEY_UP || is_char(event, 'k') || is_ctrl(event, 'p')) {
        move_up(list, 1);
    } else if (event.key == KEY_DOWN || is_char(event, 'j') || is_ctrl(event, 'n')) {
        move_down(list, 1);
    } else if (event.key == KEY_HOME || is_char(event, 'g')) {
        move_to(list, 0);
    } else if (event.key == KEY_END || is_char(event, 'G')) {
        move_to(list, SIZE_MAX);
    } else if (event.key == KEY_PAGE_UP) {
        move_up(list, page(list));
    } else if (event.key == KEY_PAGE_DOWN) {
        move_down(list, page(list));
    }
    return LIST_CONTINUE;
}

void list_append(StringBuilder *out, ColorDepth depth, const List *list, Rgb cursor_color)
{
    size_t end = list->height == 0 ? list->items.count : min_size(list->items.count, list->offset + list->height);
    for (size_t i = list->offset; i < end; i++) {
        if (i > list->offset) {
            str_builder_append_char(out, '\n');
        }
        if (i == list->cursor) {
            style_append(out, depth, style_fg(cursor_color), S("> "));
            style_append(out, depth, style_fg(cursor_color), list->items.items[i]);
        } else {
            str_builder_append(out, S("  "));
            str_builder_append(out, list->items.items[i]);
        }
    }
}
