#ifndef VEHICLE_SYSTEM_H
#define VEHICLE_SYSTEM_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

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
    enum class BlackHawkPhase {
        Inbound, Descending, Unloading, Departing, Crashing, Down, Gone };

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

    // The insertion bird is losing its engine. Each flight rolls how long it
    // survives, and the drain rate is derived from that -- rolling the time
    // directly keeps the failure point uniform across the window, which rolling
    // the rate would not (a flat rate range bunches the outcomes at the fast
    // end, since time is 1/rate).
    static constexpr float BlackHawkMaxHealth = 100.0f;
    static constexpr float BlackHawkMinFailSeconds = 7.0f;
    static constexpr float BlackHawkMaxFailSeconds = 35.0f;
    // Below this fraction the airframe starts trailing smoke.
    static constexpr float BlackHawkSmokeThreshold = 0.55f;
    // Downward acceleration and tumble rates once the engine quits.
    static constexpr float BlackHawkCrashGravity = 9.81f;
    static constexpr float BlackHawkCrashPitchRate = 0.42f;
    static constexpr float BlackHawkCrashRollRate = 0.78f;
    static constexpr float BlackHawkCrashYawRate = 0.55f;

    float blackHawkHealth = BlackHawkMaxHealth;
    float blackHawkDrainRate = 0.0f;
    DirectX::XMFLOAT3 blackHawkCrashVelocity{};
    // Terrain height under the falling wreck, refreshed by the caller each
    // frame while crashing -- the drop-off elevation is no use out here.
    float blackHawkCrashGroundY = 0.0f;
    // Raised for the single frame the wreck hits the ground, so the caller can
    // fire the explosion and hurt whoever is still strapped in.
    bool blackHawkJustCrashed = false;

    DirectX::XMFLOAT3 blackHawkModelCenter{};
    float blackHawkModelMinY = 0.0f;
    float blackHawkModelScale = 1.0f;
    DirectX::XMFLOAT3 blackHawkPosition{ 0.0f, BlackHawkStartHeight, 0.0f };
    // Drop-off point: the player spawn, projected onto the terrain.
    DirectX::XMFLOAT3 blackHawkDropOff{ 0.0f, 0.0f, 0.0f };
    // The heading the run was originally set up on. Kept separate from the live
    // blackHawkYaw, which tumbles during a crash and drifts on approach, so a
    // level reset can restore the real starting pose instead of whatever
    // attitude the wreck happened to end up in.
    float blackHawkApproachHeading = 0.0f;
    bool blackHawkRouteValid = false;
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

    // The ride-along attach point in the helicopter's local frame: side
    // (positive is starboard, since right is (cos, -sin)), forward from the
    // model centre, and height above the skids. World metres against the
    // normalized airframe (see ConfigureBlackHawkBounds).
    //
    // Taken from the "PlayerRide" empty in the GLB when the model carries one,
    // so the spot is placed in Blender instead of dialled in by hand. These
    // fallbacks only apply to a model without that node.
    float blackHawkRideSide = 1.15f;
    float blackHawkRideForward = 0.4f;
    float blackHawkRideHeight = 2.1f;

    // Where the player's centre sits while riding, given the current pose. The
    // offset is rotated by the full roll/pitch/yaw the airframe is drawn with,
    // not yaw alone: the bird runs in nose-down and banks on departure, and
    // ignoring those left the player low and aft of the authored point.
    DirectX::XMFLOAT3 BlackHawkRidePosition() const {
        // Model local axes: +X starboard, +Y up, +Z forward -- the same basis
        // BlackHawkWorldMatrix rotates, so the two stay in step.
        const DirectX::XMVECTOR offset = DirectX::XMVectorSet(
            blackHawkRideSide, blackHawkRideHeight, blackHawkRideForward, 0.0f);
        const DirectX::XMMATRIX orientation =
            DirectX::XMMatrixRotationRollPitchYaw(blackHawkPitch, blackHawkYaw,
                                                  blackHawkRoll);
        DirectX::XMFLOAT3 rotated{};
        DirectX::XMStoreFloat3(&rotated,
                               DirectX::XMVector3TransformNormal(offset, orientation));
        return { blackHawkPosition.x + rotated.x,
                 blackHawkPosition.y + rotated.y,
                 blackHawkPosition.z + rotated.z };
    }

    // Set for the single frame the player bails out early, so the caller can
    // release them beside the aircraft wherever it happens to be rather than
    // teleporting them to the drop-off like a normal touchdown would.
    bool blackHawkBailedOut = false;

    // Lets the player jump out mid-flight. Does nothing when they are not
    // aboard, so it is safe to call straight off a keypress. The helicopter
    // carries on flying its route; only the passenger leaves.
    bool BailOutOfBlackHawk() {
        if (!blackHawkCarryingPlayer) return false;
        blackHawkCarryingPlayer = false;
        blackHawkBailedOut = true;
        return true;
    }

    // Places the drop-off at the player spawn and parks the helicopter at the
    // start of its approach run, inbound on the given heading (radians, the
    // direction it flies toward).
    void BeginBlackHawkInsertion(const DirectX::XMFLOAT3& dropOff,
                                 float groundY, float approachHeading) {
        blackHawkDropOff = dropOff;
        blackHawkGroundY = groundY;
        blackHawkApproachHeading = approachHeading;
        blackHawkRouteValid = true;
        blackHawkYaw = approachHeading;
        blackHawkPitch = 0.0f;
        blackHawkRoll = 0.0f;
        blackHawkPhase = BlackHawkPhase::Inbound;
        blackHawkLanded = false;
        blackHawkDroppedPlayer = false;
        blackHawkBailedOut = false;
        blackHawkJustCrashed = false;
        blackHawkVisible = true;
        blackHawkCarryingPlayer = true;
        // Fresh airframe, fresh roll: the drain rate decides how far into the
        // run it gets before the engine gives out.
        blackHawkHealth = BlackHawkMaxHealth;
        blackHawkCrashVelocity = { 0.0f, 0.0f, 0.0f };
        const float roll = (float)std::rand() / (float)RAND_MAX;
        const float failSeconds = BlackHawkMinFailSeconds +
            roll * (BlackHawkMaxFailSeconds - BlackHawkMinFailSeconds);
        blackHawkDrainRate = BlackHawkMaxHealth / failSeconds;
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
        const bool flying = blackHawkPhase != BlackHawkPhase::Crashing &&
                            blackHawkPhase != BlackHawkPhase::Down &&
                            blackHawkPhase != BlackHawkPhase::Gone;
        const bool idling = blackHawkPhase == BlackHawkPhase::Unloading;
        // The rotor winds down once the engine quits instead of holding revs.
        const float rotorRate =
            blackHawkPhase == BlackHawkPhase::Down ? 0.0f :
            blackHawkPhase == BlackHawkPhase::Crashing ? 18.0f :
            (idling ? 22.0f : 34.0f);
        blackHawkRotorSpin += dt * rotorRate;
        blackHawkDroppedPlayer = false;
        blackHawkJustCrashed = false;
        // blackHawkBailedOut is deliberately NOT cleared here: it is raised
        // from the input handler, which runs earlier in the frame than this
        // update, so clearing it now would drop the event before the release
        // code ever sees it. The consumer clears it instead.

        // Engine failure: bleed health while under power. Hitting zero drops it
        // out of whatever it was doing and into the spiral.
        if (flying && blackHawkDrainRate > 0.0f) {
            blackHawkHealth =
                (std::max)(0.0f, blackHawkHealth - blackHawkDrainRate * dt);
            if (blackHawkHealth <= 0.0f) BeginBlackHawkCrash();
        }

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
        case BlackHawkPhase::Crashing: {
            // Ballistic fall with a tumble. The player stays strapped in, so
            // BlackHawkRidePosition keeps dragging them along for the ride.
            blackHawkCrashVelocity.y -= BlackHawkCrashGravity * dt;
            blackHawkPosition.x += blackHawkCrashVelocity.x * dt;
            blackHawkPosition.y += blackHawkCrashVelocity.y * dt;
            blackHawkPosition.z += blackHawkCrashVelocity.z * dt;
            blackHawkPitch += BlackHawkCrashPitchRate * dt;
            blackHawkRoll += BlackHawkCrashRollRate * dt;
            blackHawkYaw += BlackHawkCrashYawRate * dt;
            // Impact height comes from the caller, since the crash site is
            // nowhere near the drop-off whose elevation blackHawkGroundY holds.
            if (blackHawkPosition.y <= blackHawkCrashGroundY) {
                blackHawkPosition.y = blackHawkCrashGroundY;
                blackHawkPhase = BlackHawkPhase::Down;
                blackHawkCrashVelocity = { 0.0f, 0.0f, 0.0f };
                blackHawkLanded = true;
                blackHawkJustCrashed = true;
                // Anyone still aboard is thrown clear on impact; the caller
                // reads this to place and hurt them.
                blackHawkDroppedPlayer = blackHawkCarryingPlayer;
                blackHawkCarryingPlayer = false;
            }
            break;
        }
        case BlackHawkPhase::Down:
            // Wreck stays put and visible.
            break;
        case BlackHawkPhase::Gone:
            break;
        }
    }

    // Kills the engine and starts the spiral, carrying whatever momentum the
    // current phase had into the fall.
    void BeginBlackHawkCrash() {
        if (blackHawkPhase == BlackHawkPhase::Crashing ||
            blackHawkPhase == BlackHawkPhase::Down) return;
        blackHawkHealth = 0.0f;
        blackHawkPhase = BlackHawkPhase::Crashing;
        blackHawkLanded = false;
        // Keep flying forward along the current heading while dropping, so it
        // arcs into the ground instead of falling straight down.
        blackHawkCrashVelocity = {
            std::sin(blackHawkYaw) * 12.0f, -2.0f,
            std::cos(blackHawkYaw) * 12.0f };
    }

public:
    // Remaining health as 0..1.
    float BlackHawkHealthFraction() const {
        return BlackHawkMaxHealth > 0.0f
            ? (std::max)(0.0f, (std::min)(1.0f,
                  blackHawkHealth / BlackHawkMaxHealth))
            : 0.0f;
    }

    // How far past the smoking threshold the airframe is, 0 at the first wisp
    // and 1 at zero health. Drives how thick and how dark the trail gets, so
    // the smoke reads as a health bar.
    float BlackHawkDamageSeverity() const {
        if (BlackHawkSmokeThreshold <= 0.0f) return 0.0f;
        const float fraction = BlackHawkHealthFraction();
        if (fraction >= BlackHawkSmokeThreshold) return 0.0f;
        return (BlackHawkSmokeThreshold - fraction) / BlackHawkSmokeThreshold;
    }

    // True while the airframe is trailing smoke from battle damage.
    bool BlackHawkSmoking() const {
        return blackHawkPhase != BlackHawkPhase::Gone &&
               blackHawkHealth <
                   BlackHawkMaxHealth * BlackHawkSmokeThreshold;
    }
    bool BlackHawkIsDown() const {
        return blackHawkPhase == BlackHawkPhase::Down;
    }
    bool BlackHawkIsCrashing() const {
        return blackHawkPhase == BlackHawkPhase::Crashing;
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
        // insertion again on reload. Reuses the heading the route was set up
        // on, not the live yaw: a crash tumbles that arbitrarily, which would
        // otherwise restart the run flying in from a random direction.
        blackHawkRotorSpin = 0.0f;
        blackHawkPitch = 0.0f;
        blackHawkRoll = 0.0f;
        blackHawkCrashVelocity = {};
        blackHawkJustCrashed = false;
        if (blackHawkRouteValid) {
            BeginBlackHawkInsertion(blackHawkDropOff, blackHawkGroundY,
                                    blackHawkApproachHeading);
        }
    }
};

#endif
