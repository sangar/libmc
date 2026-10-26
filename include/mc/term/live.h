#pragma once

#include "mc/core/arena.h"
#include "mc/text/str.h"

// Live redraws a frame of lines in place, below where the cursor was, the
// way spinners and progress bars update without scrolling. Each frame
// replaces the one before: lines that did not change are skipped, lines wider
// than the terminal are cut so they never wrap, and each frame is one
// synchronized update, so the terminal shows it whole. Each line is drawn on
// its own, so a style does not carry over from one line to the next. The
// cursor is hidden until live_finish. A frame taller than the screen cannot be
// redrawn above the screen's top.
typedef struct Live Live;

// live_create keeps its state in arena. A Live that is not interactive, for
// output that is not a terminal, draws nothing until live_finish, which
// appends the last frame once.
Live *live_create(Arena *arena, bool interactive);
// live_render appends to out what turns the previous frame into frame, lines
// separated by '\n', on a terminal columns wide. It splits the lines in
// out's arena.
void live_render(Live *live, StringBuilder *out, String frame, size_t columns);
// live_finish appends what leaves the last frame on the screen with the
// cursor shown on the line below it. The next frame starts a new region there.
void live_finish(Live *live, StringBuilder *out);
// live_clear appends what erases the last frame and shows the cursor where
// the frame began, for output that should leave nothing behind, such as a
// spinner or a menu. A Live that is not interactive appends nothing.
void live_clear(Live *live, StringBuilder *out);
