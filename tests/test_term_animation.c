#include "mc/term/progress.h"
#include "mc/term/spinner.h"
#include "mc/term/spring.h"
#include "test.h"

static void test_spinner(Test *test)
{
    test_check_str(test, spinner_frame(SPINNER_DOTS, 0), S("⠋"), "first frame");
    test_check_str(test, spinner_frame(SPINNER_DOTS, 85), S("⠙"), "second frame after one interval");
    test_check_str(test, spinner_frame(SPINNER_DOTS, 800), S("⠋"), "wraps around");
}

// max_position steps a spring from 0 toward 1 at 60 frames a second and
// returns the furthest it got, with where it ended in *last.
static double max_position(double damping, int steps, double *last)
{
    Spring spring = spring_make(1.0 / 60.0, 18.0, damping);
    Motion motion = { 0 };
    double max = 0.0;
    for (int i = 0; i < steps; i++) {
        motion = spring_step(spring, motion, 1.0);
        max = motion.position > max ? motion.position : max;
    }
    *last = motion.position;
    return max;
}

static void test_spring(Test *test)
{
    double critical;
    test_check(test, max_position(1.0, 60, &critical) <= 1.0, "critically damped does not overshoot");
    test_check(test, critical > 0.99, "critically damped settles within a second");
    double last;
    test_check(test, max_position(0.3, 60, &last) > 1.1, "under-damped overshoots");
    max_position(1.0, 10, &critical);
    max_position(2.0, 10, &last);
    test_check(test, last < critical, "over-damped lags behind");
}

static String bar(Test *test, double fraction)
{
    StringBuilder out = str_builder_create(test->arena, 64);
    progress_append(&out, COLOR_DEPTH_NONE, 10, fraction, (Gradient){ 0 });
    return str_builder_finish(&out);
}

static void test_progress(Test *test)
{
    test_check_str(test, bar(test, 0.55), S("█████▌░░░░"), "half a column in eighths");
    test_check_str(test, bar(test, 0.0), S("░░░░░░░░░░"), "empty");
    test_check_str(test, bar(test, 1.5), S("██████████"), "clamped to full");
}

const TestCase TERM_ANIMATION_TESTS[] = {
    { "spinner", test_spinner },
    { "spring", test_spring },
    { "progress", test_progress },
    { nullptr, nullptr },
};
