#pragma once

// Spring moves a value toward a target the way a damped spring would, with
// Ryan Juckett's closed-form step as charmbracelet/harmonica uses it. Make
// one per frame rate and step every animated value with it.
typedef struct Spring {
    double pos_pos;
    double pos_vel;
    double vel_pos;
    double vel_vel;
} Spring;

typedef struct Motion {
    double position;
    double velocity;
} Motion;

// spring_make is a spring for steps of dt seconds. frequency is in radians
// per second: higher moves faster. A damping below 1 overshoots and
// oscillates, 1 settles fastest without overshooting, above 1 settles slower.
Spring spring_make(double dt, double frequency, double damping);
// spring_step is motion one step later, pulled toward target.
Motion spring_step(Spring spring, Motion motion, double target);
