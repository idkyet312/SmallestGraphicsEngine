#ifndef VEHICLE_SYSTEM_H
#define VEHICLE_SYSTEM_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

struct VehicleSystem {
    static constexpr float HelicopterMaxHealth = 2000.0f;

    DirectX::XMFLOAT3 humveeModelCenter{};
    float humveeModelMinY = 0.0f;
    float humveeModelScale = 1.0f;
    DirectX::XMFLOAT3 helicopterModelCenter{};
    float helicopterModelScale = 1.0f;

    float helicopterLevelScale = 1.0f;
    float helicopterMainRotorAngle = 0.0f;
    float helicopterTailRotorAngle = 0.0f;
    float helicopterYaw = 0.0f;
    float helicopterPitch = 0.0f;
    float helicopterRoll = 0.0f;
    float helicopterHoverTime = 0.0f;
    float helicopterFireCooldown = 0.0f;
    float helicopterFireCycleTime = 0.0f;
    DirectX::XMFLOAT3 helicopterPosition{ 0.0f, 14.0f, 0.0f };
    DirectX::XMFLOAT3 helicopterSpawn{ 0.0f, 14.0f, 0.0f };
    float helicopterHealth = HelicopterMaxHealth;
    bool helicopterDead = false;
    bool helicopterCrashed = false;
    DirectX::XMFLOAT3 helicopterCrashVelocity{};

    DirectX::XMFLOAT3 secondaryHelicopterPosition{ 42.0f, 14.0f, 0.0f };
    float secondaryHelicopterYaw = 0.0f;
    float secondaryHelicopterPitch = 0.0f;
    float secondaryHelicopterRoll = 0.0f;
    float secondaryHelicopterHoverTime = 1.7f;
    float secondaryHelicopterFireCooldown = 0.0f;
    float secondaryHelicopterFireCycleTime = 3.5f;
    float secondaryHelicopterHealth = HelicopterMaxHealth;
    bool secondaryHelicopterDead = false;
    bool secondaryHelicopterCrashed = false;
    DirectX::XMFLOAT3 secondaryHelicopterCrashVelocity{};
    DirectX::XMFLOAT3 secondaryHumveePosition{ 42.0f, 2.5f, 3.0f };

    DirectX::XMFLOAT3 humveeTurretLocal{ 0.0f, 0.35f, 0.0f };
    bool drivingHumvee = false;
    bool savedGunVisible = true;
    DirectX::XMFLOAT3 previousHumveePosition{};
    bool previousHumveePositionValid = false;
    float humveeHouseImpactCooldown = 0.0f;
    DirectX::XMFLOAT3 humveeAimPoint{};
    float humveeTurretYaw = 0.0f;
    float humveeTurretFireCooldown = 0.0f;
    DirectX::XMFLOAT3 primaryHumveeSpawn{ 0.0f, 3.45f, 0.0f };
    float primaryHumveeYaw = 0.0f;

    struct DamageResult {
        DirectX::XMFLOAT3 position{};
        bool applied = false;
        bool destroyed = false;
    };

    DamageResult DamagePrimaryHelicopter(float damage) {
        DamageResult result{ helicopterPosition };
        if (damage <= 0.0f || helicopterDead) return result;
        result.applied = true;
        helicopterHealth = (std::max)(0.0f, helicopterHealth - damage);
        if (helicopterHealth > 0.0f) return result;
        helicopterDead = true;
        helicopterFireCooldown = 9999.0f;
        helicopterCrashVelocity = {
            std::sin(helicopterYaw) * 2.2f,
            -0.8f,
            std::cos(helicopterYaw) * 2.2f };
        result.destroyed = true;
        return result;
    }

    DamageResult DamageSecondaryHelicopter(float damage) {
        DamageResult result{ secondaryHelicopterPosition };
        if (damage <= 0.0f || secondaryHelicopterDead) return result;
        result.applied = true;
        secondaryHelicopterHealth =
            (std::max)(0.0f, secondaryHelicopterHealth - damage);
        if (secondaryHelicopterHealth > 0.0f) return result;
        secondaryHelicopterDead = true;
        secondaryHelicopterFireCooldown = 9999.0f;
        secondaryHelicopterCrashVelocity = {
            std::sin(secondaryHelicopterYaw) * 2.2f,
            -0.8f,
            std::cos(secondaryHelicopterYaw) * 2.2f };
        result.destroyed = true;
        return result;
    }

    void ResetLevel() {
        helicopterLevelScale = 1.0f;
        helicopterMainRotorAngle = 0.0f;
        helicopterTailRotorAngle = 0.0f;
        helicopterYaw = 0.0f;
        helicopterPitch = 0.0f;
        helicopterRoll = 0.0f;
        helicopterHoverTime = 0.0f;
        helicopterFireCooldown = 0.0f;
        helicopterFireCycleTime = 0.0f;
        helicopterPosition = { 0.0f, 14.0f, 0.0f };
        helicopterSpawn = helicopterPosition;
        helicopterHealth = HelicopterMaxHealth;
        helicopterDead = false;
        helicopterCrashed = false;
        helicopterCrashVelocity = {};

        secondaryHelicopterPosition = { 42.0f, 14.0f, 0.0f };
        secondaryHelicopterYaw = 0.0f;
        secondaryHelicopterPitch = 0.0f;
        secondaryHelicopterRoll = 0.0f;
        secondaryHelicopterHoverTime = 1.7f;
        secondaryHelicopterFireCooldown = 0.0f;
        secondaryHelicopterFireCycleTime = 3.5f;
        secondaryHelicopterHealth = HelicopterMaxHealth;
        secondaryHelicopterDead = false;
        secondaryHelicopterCrashed = false;
        secondaryHelicopterCrashVelocity = {};

        drivingHumvee = false;
        savedGunVisible = true;
        previousHumveePositionValid = false;
        humveeHouseImpactCooldown = 0.0f;
        humveeAimPoint = {};
        humveeTurretYaw = 0.0f;
        humveeTurretFireCooldown = 0.0f;
        primaryHumveeSpawn = { 0.0f, 3.45f, 0.0f };
        primaryHumveeYaw = 0.0f;
    }
};

#endif
