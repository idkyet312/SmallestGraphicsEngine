#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

struct BoatWakeSample {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 direction;
    float speed;
    float strength;
    bool wave;
};

// Distance spacing keeps a continuous trail at different frame rates without
// emitting an unbounded burst after a loading stall or a network correction.
class BoatWakeEmitter {
public:
    static constexpr float Spacing = 0.85f;

    void Reset() { distance_ = 0.0f; sequence_ = 0; }

    template<class Emit>
    void Step(const DirectX::XMFLOAT3& before, const DirectX::XMFLOAT3& after,
              float dt, bool active, Emit emit) {
        if (!active || dt <= 0.0f) { Reset(); return; }
        const float dx = after.x - before.x, dz = after.z - before.z;
        const float distance = std::hypot(dx, dz);
        const float speed = distance / dt;
        if (speed < 0.65f || distance > 40.0f * dt + 3.0f) {
            Reset();
            return;
        }
        const DirectX::XMFLOAT2 direction{dx / distance, dz / distance};
        const float strength = (std::clamp)(speed / 18.0f, 0.05f, 1.0f);
        float next = Spacing - distance_;
        int emitted = 0;
        while (next <= distance && emitted < 4) {
            const float t = next / distance;
            emit(BoatWakeSample{{before.x + dx * t,
                                 before.y + (after.y - before.y) * t,
                                 before.z + dz * t},
                                direction, speed, strength, sequence_++ % 3 == 0});
            next += Spacing;
            ++emitted;
        }
        distance_ = std::fmod(distance_ + distance, Spacing);
    }

private:
    float distance_ = 0.0f;
    unsigned sequence_ = 0;
};
