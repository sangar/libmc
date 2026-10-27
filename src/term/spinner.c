#include "mc/term/spinner.h"

#include "mc/core/base.h"

static const char *const DOTS[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
static const char *const LINE[] = { "-", "\\", "|", "/" };
static const char *const ARC[] = { "◜", "◠", "◝", "◞", "◡", "◟" };
static const char *const BOUNCE[] = { "⠁", "⠂", "⠄", "⠂" };

const Spinner SPINNER_DOTS = { DOTS, countof(DOTS), 80 };
const Spinner SPINNER_LINE = { LINE, countof(LINE), 130 };
const Spinner SPINNER_ARC = { ARC, countof(ARC), 100 };
const Spinner SPINNER_BOUNCE = { BOUNCE, countof(BOUNCE), 120 };

String spinner_frame(Spinner spinner, int64_t elapsed_ms)
{
    int64_t step = elapsed_ms < 0 ? 0 : elapsed_ms / spinner.interval_ms;
    return S(spinner.frames[(size_t)step % spinner.count]);
}
