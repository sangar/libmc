#pragma once

#include "mc/text/str.h"

// Text written for a terminal: UTF-8 with escape sequences in it, CSI such as
// colours and OSC such as hyperlinks, which take no columns.

// ansi_width is the number of columns text takes on a terminal.
size_t ansi_width(String text);
// ansi_truncate is the longest prefix of text that takes at most columns
// columns. It keeps the escape sequences before the cut and the combining
// marks of the last character it keeps.
String ansi_truncate(String text, size_t columns);
// ansi_strip is text without its escape sequences, in arena.
String ansi_strip(Arena *arena, String text);
// ansi_escape_end is the offset just past the escape sequence at start, where
// text holds ESC: a CSI ends at its final byte, an OSC at BEL or ESC \, any
// other after the byte that follows ESC.
size_t ansi_escape_end(String text, size_t start);
