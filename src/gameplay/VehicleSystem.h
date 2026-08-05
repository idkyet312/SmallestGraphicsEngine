#ifndef VEHICLE_SYSTEM_H
#define VEHICLE_SYSTEM_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

struct VehicleSystem {
    static constexpr float HelicopterMaxHealth = 2000.0f;
    static constexpr float BoatMaxHealth = 1200.0f;

    DirectX::XMFLOAT3 humveeModelCenter{};
    float humveeModelMinY = 0.0f;
    float humveeModelScale = 1.0f;
    DirectX::XMFLOAT3 helicopterModelCenter{};
    float helicopterModelScale = 1.0f;

    float helicopterLevelScale = 1.0f;
    float helicopterMainRotorAngle = 0.0f;
    float helicopterTailRotorAngle = 0.0f;
    float helicopterRotorSpeedScale = 1.0f;
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

    // Boat: circles the island on the water surface. Sinks in place (rather
    // than falling like a downed helicopter) once destroyed.
    DirectX::XMFLOAT3 boatPosition{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 boatCenter{ 0.0f, 0.0f, 0.0f };
    float boatYaw = 0.0f;
    float boatRoll = 0.0f;
    float boatPatrolTime = 0.0f;
    float boatHealth = BoatMaxHealth;
    bool boatDead = false;
    bool boatSunk = false;
    float boatSinkDepth = 0.0f;

    // BlackHawk: starts hovering high above the map centre on level start and
    // descends under its own power until it touches down, then idles on the
    // ground. Purely scripted -- it carries no health or damage state.
    static constexpr float BlackHawkStartHeight = 90.0f;
    static constexpr float BlackHawkDescentSpeed = 6.5f;
    DirectX::XMFLOAT3 blackHawkModelCenter{};
    float blackHawkModelMinY = 0.0f;
    float blackHawkModelScale = 1.0f;
    DirectX::XMFLOAT3 blackHawkPosition{ 0.0f, BlackHawkStartHeight, 0.0f };
    float blackHawkGroundY = 0.0f;
    float blackHawkYaw = 0.0f;
    float blackHawkRotorSpin = 0.0f;
    bool blackHawkLanded = false;

    // Descends toward groundY, easing out over the last few metres so the
    // touchdown reads as a controlled landing rather than a drop.
    void UpdateBlackHawk(float deltaTime) {
        const float dt = (std::max)(0.0f, deltaTime);
        blackHawkRotorSpin += dt * (blackHawkLanded ? 6.0f : 34.0f);
        if (blackHawkLanded) return;
        const float remaining = blackHawkPosition.y - blackHawkGroundY;
        if (remaining <= 0.01f) {
            blackHawkPosition.y = blackHawkGroundY;
            blackHawkLanded = true;
            return;
        }
        // Full speed while high, tapering to a slow flare inside the last 12 m.
        const float flare = (std::min)(1.0f, remaining / 12.0f);
        const float speed = BlackHawkDescentSpeed * (0.12f + 0.88f * flare);
        blackHawkPosition.y =
            (std::max)(blackHawkGroundY, blackHawkPosition.y - speed * dt);
        if (blackHawkPosition.y <= blackHawkGroundY + 0.01f) {
            blackHawkPosition.y = blackHawkGroundY;
            blackHawkLanded = true;
        }
    }

    struct DamageResult {
        DirectX::XMFLOAT3 position{};
        bool applied = false;
        bool destroyed = false;
    };

    static float StepHelicopterRotorSpeed(float current, bool powered,
                                          float deltaTime) {
        const float target = powered ? 1.0f : 0.0f;
        const float rate = powered ? 2.0f : 0.2f;
        const float step = rate * (std::max)(0.0f, deltaTime);
        if (current < target) return (std::min)(target, current + step);
        return (std::max)(target, current - step);
    }

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

    DamageResult DamageBoat(float damage) {
        DamageResult result{ boatPosition };
        if (damage <= 0.0f || boatDead) return result;
        result.applied = true;
        boatHealth = (std::max)(0.0f, boatHealth - damage);
        if (boatHealth > 0.0f) return result;
        boatDead = true;
        result.destroyed = true;
        return result;
    }

    void ResetLevel() {
        helicopterLevelScale = 1.0f;
        helicopterMainRotorAngle = 0.0f;
        helicopterTailRotorAngle = 0.0f;
        helicopterRotorSpeedScale = 1.0f;
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

        boatPosition = boatCenter;
        boatYaw = 0.0f;
        boatRoll = 0.0f;
        boatPatrolTime = 0.0f;
        boatHealth = BoatMaxHealth;
        boatDead = false;
        boatSunk = false;
        boatSinkDepth = 0.0f;

        // Send the BlackHawk back up so it flies the landing again on reload.
        blackHawkPosition = { 0.0f, BlackHawkStartHeight, 0.0f };
        blackHawkYaw = 0.0f;
        blackHawkRotorSpin = 0.0f;
        blackHawkLanded = false;
    }
};

#endif
