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

    // BlackHawk: flies the player in at level start. It comes in level and fast
    // from a point offset from the player spawn, flares into a hover over the
    // spawn, sets down, holds long enough to drop the player off, then pulls
    // pitch and leaves. Purely scripted -- it carries no health or damage state.
    enum class BlackHawkPhase { Inbound, Descending, Unloading, Departing, Gone };

    static constexpr float BlackHawkStartHeight = 90.0f;
    static constexpr float BlackHawkDescentSpeed = 6.5f;
    // Where the approach begins relative to the drop-off point, and how fast it
    // covers that ground.
    static constexpr float BlackHawkApproachDistance = 220.0f;
    static constexpr float BlackHawkApproachHeight = 55.0f;
    static constexpr float BlackHawkApproachSpeed = 42.0f;
    // Radius inside which the approach is considered finished and the descent
    // takes over.
    static constexpr float BlackHawkHoverRadius = 1.5f;
    // Seconds spent on the ground with the doors open before lifting off.
    static constexpr float BlackHawkUnloadTime = 3.0f;
    static constexpr float BlackHawkDepartSpeed = 18.0f;
    // Height above the drop-off point at which the bird stops being drawn.
    static constexpr float BlackHawkDepartHeight = 140.0f;

    DirectX::XMFLOAT3 blackHawkModelCenter{};
    float blackHawkModelMinY = 0.0f;
    float blackHawkModelScale = 1.0f;
    DirectX::XMFLOAT3 blackHawkPosition{ 0.0f, BlackHawkStartHeight, 0.0f };
    // Drop-off point: the player spawn, projected onto the terrain.
    DirectX::XMFLOAT3 blackHawkDropOff{ 0.0f, 0.0f, 0.0f };
    float blackHawkGroundY = 0.0f;
    float blackHawkYaw = 0.0f;
    float blackHawkPitch = 0.0f;
    float blackHawkRoll = 0.0f;
    float blackHawkRotorSpin = 0.0f;
    float blackHawkUnloadTimer = 0.0f;
    BlackHawkPhase blackHawkPhase = BlackHawkPhase::Inbound;
    bool blackHawkLanded = false;
    // Set for the single frame the skids touch down, so the caller can release
    // the player at the drop-off point.
    bool blackHawkDroppedPlayer = false;
    bool blackHawkVisible = true;
    // True while the player is riding in the cabin, before the drop-off.
    bool blackHawkCarryingPlayer = false;

    // Where the player's eyes sit while riding, in the helicopter's local
    // frame: back from the nose, off to the port door, seated height.
    static constexpr float BlackHawkSeatSide = -1.6f;
    static constexpr float BlackHawkSeatForward = 0.4f;
    static constexpr float BlackHawkSeatHeight = 2.1f;

    // Eye position for the riding player, given the current pose.
    DirectX::XMFLOAT3 BlackHawkSeatPosition() const {
        const float sinYaw = std::sin(blackHawkYaw);
        const float cosYaw = std::cos(blackHawkYaw);
        // Forward is (sin, cos); right is (cos, -sin).
        return {
            blackHawkPosition.x + sinYaw * BlackHawkSeatForward
                                + cosYaw * BlackHawkSeatSide,
            blackHawkPosition.y + BlackHawkSeatHeight,
            blackHawkPosition.z + cosYaw * BlackHawkSeatForward
                                - sinYaw * BlackHawkSeatSide };
    }

    // Places the drop-off at the player spawn and parks the helicopter at the
    // start of its approach run, inbound on the given heading (radians, the
    // direction it flies toward).
    void BeginBlackHawkInsertion(const DirectX::XMFLOAT3& dropOff,
                                 float groundY, float approachHeading) {
        blackHawkDropOff = dropOff;
        blackHawkGroundY = groundY;
        blackHawkYaw = approachHeading;
        blackHawkPitch = 0.0f;
        blackHawkRoll = 0.0f;
        blackHawkPhase = BlackHawkPhase::Inbound;
        blackHawkLanded = false;
        blackHawkDroppedPlayer = false;
        blackHawkVisible = true;
        blackHawkCarryingPlayer = true;
        blackHawkUnloadTimer = 0.0f;
        // Back the bird up along its heading so it flies in toward the spawn.
        blackHawkPosition = {
            dropOff.x - std::sin(approachHeading) * BlackHawkApproachDistance,
            groundY + BlackHawkApproachHeight,
            dropOff.z - std::cos(approachHeading) * BlackHawkApproachDistance };
    }

    // Advances the insertion: closes on the drop-off, descends with a flare,
    // holds while the player gets out, then climbs away.
    void UpdateBlackHawk(float deltaTime) {
        const float dt = (std::max)(0.0f, deltaTime);
        const bool idling = blackHawkPhase == BlackHawkPhase::Unloading;
        blackHawkRotorSpin += dt * (idling ? 22.0f : 34.0f);
        blackHawkDroppedPlayer = false;

        switch (blackHawkPhase) {
        case BlackHawkPhase::Inbound: {
            const float dx = blackHawkDropOff.x - blackHawkPosition.x;
            const float dz = blackHawkDropOff.z - blackHawkPosition.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > 0.001f) blackHawkYaw = std::atan2(dx, dz);
            // Ease off the throttle as the drop-off comes up, and bleed the
            // approach altitude off along with the speed.
            const float closing =
                (std::min)(1.0f, distance / BlackHawkApproachDistance);
            const float speed =
                BlackHawkApproachSpeed * (0.14f + 0.86f * closing);
            const float step = (std::min)(distance, speed * dt);
            if (distance > 0.001f) {
                blackHawkPosition.x += dx / distance * step;
                blackHawkPosition.z += dz / distance * step;
            }
            // Nose down while running in, levelling out into the flare.
            blackHawkPitch = -0.16f * closing;
            const float hoverY = blackHawkGroundY + 14.0f;
            const float cruiseY = blackHawkGroundY + BlackHawkApproachHeight;
            blackHawkPosition.y = hoverY + (cruiseY - hoverY) * closing;
            if (distance <= BlackHawkHoverRadius) {
                blackHawkPosition.x = blackHawkDropOff.x;
                blackHawkPosition.z = blackHawkDropOff.z;
                blackHawkPitch = 0.0f;
                blackHawkPhase = BlackHawkPhase::Descending;
            }
            break;
        }
        case BlackHawkPhase::Descending: {
            const float remaining = blackHawkPosition.y - blackHawkGroundY;
            if (remaining <= 0.01f) {
                TouchDown();
                break;
            }
            // Full speed while high, tapering to a slow flare in the last 12 m.
            const float flare = (std::min)(1.0f, remaining / 12.0f);
            const float speed = BlackHawkDescentSpeed * (0.12f + 0.88f * flare);
            blackHawkPosition.y =
                (std::max)(blackHawkGroundY, blackHawkPosition.y - speed * dt);
            if (blackHawkPosition.y <= blackHawkGroundY + 0.01f) TouchDown();
            break;
        }
        case BlackHawkPhase::Unloading: {
            blackHawkUnloadTimer += dt;
            if (blackHawkUnloadTimer >= BlackHawkUnloadTime) {
                blackHawkPhase = BlackHawkPhase::Departing;
                blackHawkLanded = false;
            }
            break;
        }
        case BlackHawkPhase::Departing: {
            // Climb out along the heading it arrived on, nose up and banking.
            const float climbed = blackHawkPosition.y - blackHawkGroundY;
            const float ramp = (std::min)(1.0f, climbed / 20.0f);
            blackHawkPosition.y += BlackHawkDepartSpeed * (0.35f + 0.65f * ramp) * dt;
            const float forward = BlackHawkDepartSpeed * ramp * dt;
            blackHawkPosition.x += std::sin(blackHawkYaw) * forward;
            blackHawkPosition.z += std::cos(blackHawkYaw) * forward;
            blackHawkPitch = 0.20f * ramp;
            blackHawkRoll = 0.18f * ramp;
            if (climbed >= BlackHawkDepartHeight) {
                blackHawkPhase = BlackHawkPhase::Gone;
                blackHawkVisible = false;
            }
            break;
        }
        case BlackHawkPhase::Gone:
            break;
        }
    }

private:
    void TouchDown() {
        blackHawkPosition.y = blackHawkGroundY;
        blackHawkPitch = 0.0f;
        blackHawkRoll = 0.0f;
        blackHawkLanded = true;
        blackHawkDroppedPlayer = blackHawkCarryingPlayer;
        blackHawkCarryingPlayer = false;
        blackHawkUnloadTimer = 0.0f;
        blackHawkPhase = BlackHawkPhase::Unloading;
    }

public:

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

        // Put the BlackHawk back at the start of its run so it flies the whole
        // insertion again on reload.
        blackHawkRotorSpin = 0.0f;
        BeginBlackHawkInsertion(blackHawkDropOff, blackHawkGroundY, blackHawkYaw);
    }
};

#endif
