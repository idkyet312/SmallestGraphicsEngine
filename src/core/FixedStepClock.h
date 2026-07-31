#ifndef FIXED_STEP_CLOCK_H
#define FIXED_STEP_CLOCK_H

#include <algorithm>
#include <cstdint>

class FixedStepClock {
public:
    explicit FixedStepClock(float stepSeconds = 1.0f / 60.0f,
                            uint32_t maxSteps = 4)
        : stepSeconds_(stepSeconds), maxSteps_(maxSteps) {}

    void Reset() { accumulator_ = 0.0f; }

    void Accumulate(float frameDelta) {
        accumulator_ += std::clamp(frameDelta, 0.0f,
            stepSeconds_ * static_cast<float>(maxSteps_));
    }

    bool Consume(float& step) {
        if (accumulator_ + 1e-7f < stepSeconds_) return false;
        accumulator_ -= stepSeconds_;
        step = stepSeconds_;
        return true;
    }

    float Alpha() const {
        return stepSeconds_ > 0.0f ? accumulator_ / stepSeconds_ : 0.0f;
    }

private:
    float stepSeconds_;
    uint32_t maxSteps_;
    float accumulator_ = 0.0f;
};

#endif
