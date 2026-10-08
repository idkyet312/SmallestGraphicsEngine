#include "TurretTraverse.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    constexpr float dt = 1.0f / 60.0f;
    constexpr float maxRate = 1.2f, accel = 3.0f;

    {
        // Spins up instead of jumping to full speed, never exceeds maxRate,
        // and comes to rest exactly on target without overshooting.
        float angle = 0.0f, rate = 0.0f;
        const float target = 2.0f;
        SGE::StepTurretTraverse(angle, rate, target, maxRate, accel, dt);
        Check(rate > 0.0f && rate <= accel * dt + 1e-5f,
              "first frame must accelerate, not start at full traverse");
        float previousRate = rate, peak = 0.0f, maxRateJump = 0.0f;
        bool overshot = false;
        for (int i = 0; i < 600; ++i) {
            SGE::StepTurretTraverse(angle, rate, target, maxRate, accel, dt);
            peak = (std::max)(peak, rate);
            maxRateJump = (std::max)(maxRateJump, std::abs(rate - previousRate));
            previousRate = rate;
            if (angle > target + 1e-4f) overshot = true;
        }
        Check(peak <= maxRate + 1e-5f, "traverse must cap at maxRate");
        Check(peak > 0.95f * maxRate, "a long slew must reach cruise rate");
        // The settle frame may drop the last ~2 steps of speed while closing
        // under a tenth of a degree; every other frame is bounded by one step.
        Check(maxRateJump <= 2.0f * accel * dt + 1e-4f,
              "rate may only change by acceleration*dt per frame (no jerks)");
        Check(!overshot, "the mount must brake onto the target, not overshoot");
        Check(std::abs(angle - target) < 1e-5f && rate == 0.0f,
              "the mount must settle on the target and stop");
    }

    {
        // Takes the short way across the +-pi seam.
        float angle = 3.0f, rate = 0.0f;
        for (int i = 0; i < 60; ++i)
            SGE::StepTurretTraverse(angle, rate, -3.0f, maxRate, accel, dt);
        Check(std::abs(std::atan2(std::sin(angle + 3.0f), std::cos(angle + 3.0f))) < 1e-4f,
              "traverse must wrap the short way round");
    }

    {
        // A target that reverses mid-slew turns the mount around smoothly.
        float angle = 0.0f, rate = 0.0f;
        for (int i = 0; i < 30; ++i)
            SGE::StepTurretTraverse(angle, rate, 1.5f, maxRate, accel, dt);
        const float before = rate;
        SGE::StepTurretTraverse(angle, rate, -1.5f, maxRate, accel, dt);
        Check(before > 0.0f && rate > before - accel * dt - 1e-5f,
              "reversing target must decelerate, not flip the rate instantly");
    }

    {
        // Zero time or zero rate moves nothing.
        float angle = 0.5f, rate = 0.0f;
        SGE::StepTurretTraverse(angle, rate, 1.0f, maxRate, accel, 0.0f);
        Check(angle == 0.5f && rate == 0.0f, "zero dt must not move the turret");
    }

    std::cout << "TurretTraverseTests passed\n";
    return 0;
}
