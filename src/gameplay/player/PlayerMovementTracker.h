#ifndef PLAYER_MOVEMENT_TRACKER_H
#define PLAYER_MOVEMENT_TRACKER_H

#include <DirectXMath.h>
#include <cmath>

class PlayerMovementTracker {
public:
    float Update(const DirectX::XMFLOAT3& position, float deltaTime,
                 bool enabled = true) {
        if (!enabled || deltaTime <= 0.0f) {
            Reset(position);
            return 0.0f;
        }
        if (!valid_) {
            Reset(position);
            return 0.0f;
        }

        const float dx = position.x - previous_.x;
        const float dz = position.z - previous_.z;
        previous_ = position;
        const float speed = std::sqrt(dx * dx + dz * dz) / deltaTime;
        // Level spawns and teleports are not locomotion.
        horizontalSpeed_ = speed <= 30.0f ? speed : 0.0f;
        return horizontalSpeed_;
    }

    void Reset(const DirectX::XMFLOAT3& position = {}) {
        previous_ = position;
        horizontalSpeed_ = 0.0f;
        valid_ = true;
    }

    // Moving platforms translate the player without player locomotion. Shift the
    // stored sample by that exact world delta so next frame measures only input.
    void ApplyPlatformDisplacement(const DirectX::XMFLOAT3& displacement) {
        if (!valid_) return;
        previous_.x += displacement.x;
        previous_.z += displacement.z;
    }

    float HorizontalSpeed() const { return horizontalSpeed_; }

private:
    DirectX::XMFLOAT3 previous_{};
    float horizontalSpeed_ = 0.0f;
    bool valid_ = false;
};

#endif
