#ifndef ENEMY_SYSTEM_H
#define ENEMY_SYSTEM_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

class SkinnedEnemy;

struct EnemySystem {
    std::vector<std::unique_ptr<SkinnedEnemy>> actors;
    SkinnedEnemy* held = nullptr;
    uint32_t spawnSerial = 0;
    float voiceCooldown = 0.0f;
    float painCooldown = 0.0f;

    void ResetLevelCounters() {
        held = nullptr;
        spawnSerial = 0;
        voiceCooldown = 0.0f;
        painCooldown = 0.0f;
    }

    void TickCooldowns(float deltaTime) {
        voiceCooldown = (std::max)(0.0f, voiceCooldown - deltaTime);
        painCooldown = (std::max)(0.0f, painCooldown - deltaTime);
    }
};

#endif
