#pragma once

#include "mc/term/color.h"
#include "mc/term/style.h"

// progress_append appends a bar width columns wide, filled to fraction,
// clamped to 0..1, in eighths of a column. The filled part takes its colours
// from gradient spread over the whole width; the rest is drawn in grey.
void progress_append(StringBuilder *out, ColorDepth depth, size_t width, double fraction, Gradient gradient);
