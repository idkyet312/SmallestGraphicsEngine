#ifndef TURRET_TRAVERSE_H
#define TURRET_TRAVERSE_H

#include <algorithm>
#include <cmath>

namespace SGE {

// Slews a turret angle toward `desired` like a heavy mount: it spins up at
// `acceleration`, cruises at `maxRate`, and brakes along the stopping curve so
// it settles on the target instead of slamming to a halt. A constant-rate clamp
// (the old behaviour) starts and stops at full speed, which reads as a jerk on
// every aim change and a twitch whenever the aim point wobbles.
//
// `rate` is the turret's angular velocity (rad/s), kept by the caller between
// frames. Angles in radians; the error is wrapped, so the turret always takes
// the short way round. Returns the remaining wrapped error.
inline float StepTurretTraverse(float& angle, float& rate, float desired,
                                float maxRate, float acceleration,
                                float deltaTime) {
    const float dt = (std::max)(0.0f, deltaTime);
    const auto wrap = [](float a) { return std::atan2(std::sin(a), std::cos(a)); };
    float error = wrap(desired - angle);
    if (dt <= 0.0f || maxRate <= 0.0f || acceleration <= 0.0f) return error;

    // Fastest speed from which the mount can still stop exactly on target,
    // braking acceleration*dt per frame: v^2/2a + v*dt/2 = |error|. The
    // continuous v = sqrt(2a|e|) leaves a speed jump on the last frame.
    const float stoppable = acceleration *
        (std::sqrt(0.25f * dt * dt + 2.0f * std::abs(error) / acceleration) -
         0.5f * dt);
    const float targetRate =
        std::copysign((std::min)(maxRate, stoppable), error);
    const float change = acceleration * dt;
    rate += (std::max)(-change, (std::min)(change, targetRate - rate));

    const float step = rate * dt;
    // Would cross the target this frame: land on it and stop, rather than
    // overshooting and hunting back and forth.
    if ((error >= 0.0f && step >= error) || (error <= 0.0f && step <= error)) {
        angle = wrap(desired);
        rate = 0.0f;
        return 0.0f;
    }
    angle = wrap(angle + step);
    return wrap(desired - angle);
}

} // namespace SGE

#endif
