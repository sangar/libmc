#pragma once

#include <stdint.h>

#include "mc/text/str.h"

// Spinner is an animation of frames, each shown for interval_ms. The frames
// are from sindresorhus/cli-spinners, MIT licensed.
typedef struct Spinner {
    const char *const *frames;
    size_t count;
    int64_t interval_ms;
} Spinner;

extern const Spinner SPINNER_DOTS;
extern const Spinner SPINNER_LINE;
extern const Spinner SPINNER_ARC;
extern const Spinner SPINNER_BOUNCE;

// spinner_frame is the frame shown elapsed_ms after the spinner started.
String spinner_frame(Spinner spinner, int64_t elapsed_ms);
