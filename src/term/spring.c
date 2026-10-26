#include "mc/term/spring.h"

#include <math.h>

static const double EPSILON = 0.0001;

static Spring over_damped(double dt, double frequency, double damping)
{
    double za = -frequency * damping;
    double zb = frequency * sqrt(damping * damping - 1.0);
    double z1 = za - zb;
    double z2 = za + zb;
    double e1 = exp(z1 * dt) / (2.0 * zb);
    double e2 = exp(z2 * dt) / (2.0 * zb);
    double z1e1 = z1 * e1;
    double z2e2 = z2 * e2;
    return (Spring){
        .pos_pos = e1 * z2 - z2e2 + exp(z2 * dt),
        .pos_vel = -e1 + e2,
        .vel_pos = (z1e1 - z2e2 + exp(z2 * dt)) * z2,
        .vel_vel = -z1e1 + z2e2,
    };
}

static Spring under_damped(double dt, double frequency, double damping)
{
    double omega_zeta = frequency * damping;
    double alpha = frequency * sqrt(1.0 - damping * damping);
    double decay = exp(-omega_zeta * dt);
    double decay_sin = decay * sin(alpha * dt);
    double decay_cos = decay * cos(alpha * dt);
    double decay_omega_zeta_sin = decay * omega_zeta * sin(alpha * dt) / alpha;
    return (Spring){
        .pos_pos = decay_cos + decay_omega_zeta_sin,
        .pos_vel = decay_sin / alpha,
        .vel_pos = -decay_sin * alpha - omega_zeta * decay_omega_zeta_sin,
        .vel_vel = decay_cos - decay_omega_zeta_sin,
    };
}

static Spring critically_damped(double dt, double frequency)
{
    double decay = exp(-frequency * dt);
    double time_decay = dt * decay;
    double time_decay_frequency = time_decay * frequency;
    return (Spring){
        .pos_pos = time_decay_frequency + decay,
        .pos_vel = time_decay,
        .vel_pos = -frequency * time_decay_frequency,
        .vel_vel = -time_decay_frequency + decay,
    };
}

Spring spring_make(double dt, double frequency, double damping)
{
    frequency = fmax(0.0, frequency);
    damping = fmax(0.0, damping);
    if (frequency < EPSILON) {
        return (Spring){ .pos_pos = 1.0, .vel_vel = 1.0 };
    }
    if (damping > 1.0 + EPSILON) {
        return over_damped(dt, frequency, damping);
    }
    if (damping < 1.0 - EPSILON) {
        return under_damped(dt, frequency, damping);
    }
    return critically_damped(dt, frequency);
}

Motion spring_step(Spring spring, Motion motion, double target)
{
    double offset = motion.position - target;
    return (Motion){
        .position = offset * spring.pos_pos + motion.velocity * spring.pos_vel + target,
        .velocity = offset * spring.vel_pos + motion.velocity * spring.vel_vel,
    };
}
