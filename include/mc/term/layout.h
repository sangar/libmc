#pragma once

#include "mc/term/style.h"

// Blocks are text of one or more lines separated by '\n', which may hold
// escape sequences. Their width is that of their widest line.

// Align places a line in a wider block, or a block beside a taller one:
// start is left or top, end is right or bottom.
typedef enum Align { ALIGN_START, ALIGN_CENTER, ALIGN_END } Align;

typedef enum BorderKind { BORDER_NONE, BORDER_NORMAL, BORDER_ROUNDED, BORDER_THICK, BORDER_DOUBLE, BORDER_HIDDEN } BorderKind;

typedef struct Spacing {
    size_t top;
    size_t right;
    size_t bottom;
    size_t left;
} Spacing;

// Box is how box_render lays out text, as Lip Gloss does: text in style,
// aligned in width columns, inside padding, inside a border, inside margin.
// Box box = { 0 } is the text as it is.
typedef struct Box {
    Style style;
    size_t width; // 0 is the width of the widest line
    Align align;
    Spacing padding;
    BorderKind border;
    Style border_style;
    Spacing margin;
} Box;

// box_render lays out text in box, in arena. Lines wider than box.width are
// cut. The style covers the padding too, so a background fills it.
String box_render(Arena *arena, ColorDepth depth, Box box, String text);
// block_width is the width of the widest line of block.
size_t block_width(String block);
// join_horizontal places blocks side by side, each placed against the
// tallest by align.
String join_horizontal(Arena *arena, StringList blocks, Align align);
// join_vertical places blocks one above the other, each line placed in the
// widest by align.
String join_vertical(Arena *arena, StringList blocks, Align align);
