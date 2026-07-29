#ifndef COMBAT_SYSTEM_H
#define COMBAT_SYSTEM_H

#include <DirectXMath.h>
#include <cstddef>
#include <cstdint>
#include <vector>

class RuntimeWorld;

struct CombatSystem {
    struct PrefabDamageResult {
        uint64_t entityId = 0;
        DirectX::XMFLOAT3 effectPosition{};
        bool applied = false;
        bool destroyed = false;
    };

    size_t heldBarrelIndex = SIZE_MAX;
    bool suppressFireUntilMouseRelease = false;
    float fleshHitPitchMin = 0.9f;
    float fleshHitPitchMax = 1.1f;

    void ResetLevel() {
        heldBarrelIndex = SIZE_MAX;
        suppressFireUntilMouseRelease = true;
    }

    PrefabDamageResult DamagePrefab(
        RuntimeWorld& world, uint64_t entityId, float damage,
        const DirectX::XMFLOAT3& effectPosition);
    std::vector<PrefabDamageResult> DamagePrefabsInRadius(
        RuntimeWorld& world, const DirectX::XMFLOAT3& center,
        float radius, float damage);
};

#endif
