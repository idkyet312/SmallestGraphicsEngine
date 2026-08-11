#ifndef VEHICLE_SYSTEM_H
#define VEHICLE_SYSTEM_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// The rappel rope is a box3d simulation, referenced here by forward-declared
// pointer on purpose. This header is included by GameArchitectureTests, which
// links neither box3d nor the renderer; a by-value member would drag the physics
// library into that target and break its link step. A non-owning raw pointer
// (rather than unique_ptr) also keeps this struct implicitly destructible
// without RopeSwing being a complete type -- the rope itself is owned by the
// app layer, which is the only place that knows what box3d is.
//
// The pointer stays null in the test build and every rope call site is guarded,
// so the phase logic the test drives runs unchanged with no rope present.
class RopeSwing;

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

    // Battle-damage smoke on the enemy gunships, read the same way as the
    // insertion BlackHawk's: below the threshold the airframe trails smoke, and
    // severity ramps 0..1 from the first wisp to zero health. Sharing the curve
    // keeps "that thing is nearly dead" looking identical on either aircraft.
    static constexpr float HelicopterSmokeThreshold = 0.55f;

    float HelicopterHealthFraction() const {
        return HelicopterMaxHealth > 0.0f
            ? (std::max)(0.0f, (std::min)(1.0f,
                  helicopterHealth / HelicopterMaxHealth))
            : 0.0f;
    }
    float SecondaryHelicopterHealthFraction() const {
        return HelicopterMaxHealth > 0.0f
            ? (std::max)(0.0f, (std::min)(1.0f,
                  secondaryHelicopterHealth / HelicopterMaxHealth))
            : 0.0f;
    }
    // 0 at the first wisp, 1 at zero health. Same shape as
    // BlackHawkDamageSeverity so both aircraft read alike.
    static float HelicopterSeverityFromFraction(float fraction) {
        if (HelicopterSmokeThreshold <= 0.0f) return 0.0f;
        if (fraction >= HelicopterSmokeThreshold) return 0.0f;
        return (HelicopterSmokeThreshold - fraction) / HelicopterSmokeThreshold;
    }
    float HelicopterDamageSeverity() const {
        return HelicopterSeverityFromFraction(HelicopterHealthFraction());
    }
    float SecondaryHelicopterDamageSeverity() const {
        return HelicopterSeverityFromFraction(
            SecondaryHelicopterHealthFraction());
    }

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

    // Insertion boat: the seaborne counterpart to the BlackHawk, for levels that
    // land the player from the water. It runs in across the surface, slows onto
    // the drop-off point, holds while the player steps off, then turns and heads
    // back out. Kept entirely separate from the patrol boat above, which circles
    // the island on its own schedule.
    enum class InsertionBoatPhase {
        Inbound, Slowing, Unloading, Departing, Foundering, Sunk, Gone };

    // Where the run begins relative to the landing point, and how fast it covers
    // that water.
    static constexpr float InsertionBoatApproachDistance = 180.0f;
    static constexpr float InsertionBoatApproachSpeed = 24.0f;
    // Radius inside which the approach hands over to the final slow-in.
    static constexpr float InsertionBoatArrivalRadius = 12.0f;
    // How close to the landing point it noses in before the player steps off.
    static constexpr float InsertionBoatDropRadius = 1.2f;
    static constexpr float InsertionBoatUnloadTime = 3.0f;
    static constexpr float InsertionBoatDepartSpeed = 20.0f;
    // Distance back out from the landing point at which it stops being drawn.
    static constexpr float InsertionBoatDepartDistance = 200.0f;

    // Enemy fire is the only thing that reduces insertion hull integrity.
    static constexpr float InsertionBoatMaxHealth = 300.0f;
    // Below this fraction the hull starts trailing smoke.
    static constexpr float InsertionBoatSmokeThreshold = 0.55f;
    // How fast a holed hull settles, and how far down before it is gone.
    static constexpr float InsertionBoatSinkRate = 0.85f;
    static constexpr float InsertionBoatSinkDepth = 3.2f;
    static constexpr float InsertionBoatFounderRoll = 0.62f;
    static constexpr float InsertionBoatFounderRollRate = 0.30f;

    float insertionBoatHealth = InsertionBoatMaxHealth;
    // Raised for the single frame the hull goes under, so the caller can fire
    // the effects and dunk whoever is still aboard.
    bool insertionBoatJustSank = false;

    DirectX::XMFLOAT3 insertionBoatModelCenter{};
    float insertionBoatModelMinY = 0.0f;
    float insertionBoatModelScale = 1.0f;
    DirectX::XMFLOAT3 insertionBoatPosition{ 0.0f, 0.0f, 0.0f };
    // Landing point: the player spawn, projected onto the shore.
    DirectX::XMFLOAT3 insertionBoatLanding{ 0.0f, 0.0f, 0.0f };
    // The heading the run was set up on. Kept apart from the live yaw, which
    // swings on approach and on the way out, so a reset restores the real
    // starting pose rather than whatever heading it ended on.
    float insertionBoatApproachHeading = 0.0f;
    bool insertionBoatRouteValid = false;
    // Height of the water the boat rides on.
    float insertionBoatWaterY = 0.0f;
    float insertionBoatYaw = 0.0f;
    float insertionBoatRoll = 0.0f;
    float insertionBoatSinkOffset = 0.0f;
    float insertionBoatBobTime = 0.0f;
    float insertionBoatUnloadTimer = 0.0f;
    InsertionBoatPhase insertionBoatPhase = InsertionBoatPhase::Inbound;
    bool insertionBoatLanded = false;
    // Set for the single frame the bow touches the landing point, so the caller
    // can release the player ashore.
    bool insertionBoatDroppedPlayer = false;
    bool insertionBoatVisible = true;
    // True while the player is riding the deck, before the drop-off.
    bool insertionBoatCarryingPlayer = false;
    // Set for the single frame the player jumps off early.
    bool insertionBoatBailedOut = false;

    // Ride-along spot in the boat's local frame: side (positive to starboard,
    // since right is (cos, -sin)), forward from the model centre, and height
    // above the waterline. World metres against the normalized hull.
    float insertionBoatRideSide = 0.0f;
    float insertionBoatRideForward = -1.2f;
    float insertionBoatRideHeight = 0.9f;

    // Where the passenger's centre sits for the current pose. Rotated by roll
    // and yaw both, so they lean with the hull as it founders instead of
    // hovering upright over a capsizing deck.
    DirectX::XMFLOAT3 InsertionBoatRidePosition() const {
        const DirectX::XMVECTOR offset = DirectX::XMVectorSet(
            insertionBoatRideSide, insertionBoatRideHeight,
            insertionBoatRideForward, 0.0f);
        const DirectX::XMMATRIX orientation =
            DirectX::XMMatrixRotationRollPitchYaw(0.0f, insertionBoatYaw,
                                                  insertionBoatRoll);
        DirectX::XMFLOAT3 rotated{};
        DirectX::XMStoreFloat3(&rotated,
                               DirectX::XMVector3TransformNormal(offset, orientation));
        return { insertionBoatPosition.x + rotated.x,
                 insertionBoatPosition.y - insertionBoatSinkOffset + rotated.y,
                 insertionBoatPosition.z + rotated.z };
    }

    // Lets the player jump off mid-run. Safe to call straight off a keypress:
    // it does nothing when they are not aboard. The boat carries on its route.
    bool BailOutOfInsertionBoat() {
        if (!insertionBoatCarryingPlayer) return false;
        insertionBoatCarryingPlayer = false;
        insertionBoatBailedOut = true;
        return true;
    }

    // Places the landing at the player spawn and parks the boat at the start of
    // its run, inbound on the given heading (radians, the direction it travels).
    // Started only for a level that inserts by boat, so the run always carries
    // the player. A level using some other craft calls DisableInsertionBoat
    // instead -- only one insertion vehicle may be live, since two both writing
    // the camera each frame would fight over it.
    void BeginInsertionBoatRun(const DirectX::XMFLOAT3& landing,
                               float waterY, float approachHeading) {
        insertionBoatLanding = landing;
        insertionBoatWaterY = waterY;
        insertionBoatApproachHeading = approachHeading;
        insertionBoatRouteValid = true;
        insertionBoatYaw = approachHeading;
        insertionBoatRoll = 0.0f;
        insertionBoatPhase = InsertionBoatPhase::Inbound;
        insertionBoatLanded = false;
        insertionBoatDroppedPlayer = false;
        insertionBoatBailedOut = false;
        insertionBoatJustSank = false;
        insertionBoatVisible = true;
        insertionBoatCarryingPlayer = true;
        insertionBoatSinkOffset = 0.0f;
        insertionBoatBobTime = 0.0f;
        insertionBoatHealth = InsertionBoatMaxHealth;
        insertionBoatUnloadTimer = 0.0f;
        // Back the boat up along its heading so it runs in toward the landing.
        insertionBoatPosition = {
            landing.x - std::sin(approachHeading) * InsertionBoatApproachDistance,
            waterY,
            landing.z - std::cos(approachHeading) * InsertionBoatApproachDistance };
    }

    // Advances the run: closes on the landing, slows onto it, holds while the
    // player gets off, then turns and heads back out.
    void UpdateInsertionBoat(float deltaTime) {
        const float dt = (std::max)(0.0f, deltaTime);
        insertionBoatBobTime += dt;
        insertionBoatDroppedPlayer = false;
        insertionBoatJustSank = false;
        // insertionBoatBailedOut is deliberately NOT cleared here: it is raised
        // from the input handler, which runs earlier in the frame, so clearing
        // it now would drop the event before the release code sees it. The
        // consumer clears it instead.

        // Gentle swell everywhere except on the bottom.
        const float bob = insertionBoatPhase == InsertionBoatPhase::Sunk
            ? 0.0f : std::sin(insertionBoatBobTime * 0.9f) * 0.10f;

        switch (insertionBoatPhase) {
        case InsertionBoatPhase::Inbound: {
            const float dx = insertionBoatLanding.x - insertionBoatPosition.x;
            const float dz = insertionBoatLanding.z - insertionBoatPosition.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance > 0.001f) insertionBoatYaw = std::atan2(dx, dz);
            const float step = (std::min)(distance,
                                          InsertionBoatApproachSpeed * dt);
            if (distance > 0.001f) {
                insertionBoatPosition.x += dx / distance * step;
                insertionBoatPosition.z += dz / distance * step;
            }
            insertionBoatPosition.y = insertionBoatWaterY + bob;
            // Heel into the run, easing off as the landing comes up.
            insertionBoatRoll = std::sin(insertionBoatBobTime * 0.5f) * 0.05f;
            if (distance <= InsertionBoatArrivalRadius)
                insertionBoatPhase = InsertionBoatPhase::Slowing;
            break;
        }
        case InsertionBoatPhase::Slowing: {
            const float dx = insertionBoatLanding.x - insertionBoatPosition.x;
            const float dz = insertionBoatLanding.z - insertionBoatPosition.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance <= InsertionBoatDropRadius) {
                PutAshore();
                break;
            }
            if (distance > 0.001f) insertionBoatYaw = std::atan2(dx, dz);
            // Taper the last stretch so the bow noses in rather than ramming.
            const float closing =
                (std::min)(1.0f, distance / InsertionBoatArrivalRadius);
            const float speed =
                InsertionBoatApproachSpeed * (0.10f + 0.90f * closing);
            const float step = (std::min)(distance, speed * dt);
            if (distance > 0.001f) {
                insertionBoatPosition.x += dx / distance * step;
                insertionBoatPosition.z += dz / distance * step;
            }
            insertionBoatPosition.y = insertionBoatWaterY + bob;
            insertionBoatRoll *= (std::max)(0.0f, 1.0f - 2.0f * dt);
            break;
        }
        case InsertionBoatPhase::Unloading: {
            insertionBoatPosition.y = insertionBoatWaterY + bob;
            insertionBoatUnloadTimer += dt;
            if (insertionBoatUnloadTimer >= InsertionBoatUnloadTime) {
                insertionBoatPhase = InsertionBoatPhase::Departing;
                insertionBoatLanded = false;
                // Turn about and run back out the way it came in.
                insertionBoatYaw = insertionBoatApproachHeading +
                                   3.14159265358979323846f;
            }
            break;
        }
        case InsertionBoatPhase::Departing: {
            const float forward = InsertionBoatDepartSpeed * dt;
            insertionBoatPosition.x += std::sin(insertionBoatYaw) * forward;
            insertionBoatPosition.z += std::cos(insertionBoatYaw) * forward;
            insertionBoatPosition.y = insertionBoatWaterY + bob;
            const float dx = insertionBoatPosition.x - insertionBoatLanding.x;
            const float dz = insertionBoatPosition.z - insertionBoatLanding.z;
            if (std::sqrt(dx * dx + dz * dz) >= InsertionBoatDepartDistance) {
                insertionBoatPhase = InsertionBoatPhase::Gone;
                insertionBoatVisible = false;
            }
            break;
        }
        case InsertionBoatPhase::Foundering: {
            // Settles in place with a list, rather than falling like a downed
            // helicopter: the water holds it up until the hull fills.
            insertionBoatSinkOffset += InsertionBoatSinkRate * dt;
            insertionBoatRoll = (std::min)(InsertionBoatFounderRoll,
                insertionBoatRoll + InsertionBoatFounderRollRate * dt);
            insertionBoatPosition.y = insertionBoatWaterY + bob * 0.4f;
            if (insertionBoatSinkOffset >= InsertionBoatSinkDepth) {
                insertionBoatSinkOffset = InsertionBoatSinkDepth;
                insertionBoatPhase = InsertionBoatPhase::Sunk;
                insertionBoatLanded = true;
                insertionBoatJustSank = true;
                // Anyone still aboard goes in the water; the caller reads this
                // to place and hurt them.
                insertionBoatDroppedPlayer = insertionBoatCarryingPlayer;
                insertionBoatCarryingPlayer = false;
            }
            break;
        }
        case InsertionBoatPhase::Sunk:
            // Hull stays under, listing where it went down.
            break;
        case InsertionBoatPhase::Gone:
            break;
        }
    }

    // Stands the boat down for a level that inserts by some other means. It is
    // not merely hidden: the route is dropped, so a reset does not resurrect a
    // run this level never wanted or leave one parked at the origin.
    void DisableInsertionBoat() {
        insertionBoatRouteValid = false;
        insertionBoatVisible = false;
        insertionBoatCarryingPlayer = false;
        insertionBoatDroppedPlayer = false;
        insertionBoatBailedOut = false;
        insertionBoatJustSank = false;
        insertionBoatHealth = InsertionBoatMaxHealth;
        insertionBoatSinkOffset = 0.0f;
        insertionBoatRoll = 0.0f;
        insertionBoatPhase = InsertionBoatPhase::Gone;
    }

    // Hulls the boat and starts it going down, from whatever phase it was in.
    void BeginInsertionBoatFounder() {
        if (insertionBoatPhase == InsertionBoatPhase::Foundering ||
            insertionBoatPhase == InsertionBoatPhase::Sunk) return;
        insertionBoatHealth = 0.0f;
        insertionBoatPhase = InsertionBoatPhase::Foundering;
        insertionBoatLanded = false;
    }

    // Remaining hull integrity as 0..1.
    float InsertionBoatHealthFraction() const {
        return InsertionBoatMaxHealth > 0.0f
            ? (std::max)(0.0f, (std::min)(1.0f,
                  insertionBoatHealth / InsertionBoatMaxHealth))
            : 0.0f;
    }

    // How far past the smoking threshold the hull is, 0 at the first wisp and 1
    // at zero health, so the smoke reads as a health bar.
    float InsertionBoatDamageSeverity() const {
        if (InsertionBoatSmokeThreshold <= 0.0f) return 0.0f;
        const float fraction = InsertionBoatHealthFraction();
        if (fraction >= InsertionBoatSmokeThreshold) return 0.0f;
        return (InsertionBoatSmokeThreshold - fraction) /
               InsertionBoatSmokeThreshold;
    }

    bool InsertionBoatSmoking() const {
        return insertionBoatPhase != InsertionBoatPhase::Gone &&
               insertionBoatHealth <
                   InsertionBoatMaxHealth * InsertionBoatSmokeThreshold;
    }
    bool InsertionBoatIsSunk() const {
        return insertionBoatPhase == InsertionBoatPhase::Sunk;
    }
    bool InsertionBoatIsFoundering() const {
        return insertionBoatPhase == InsertionBoatPhase::Foundering;
    }

private:
    void PutAshore() {
        insertionBoatPosition.x = insertionBoatLanding.x;
        insertionBoatPosition.z = insertionBoatLanding.z;
        insertionBoatPosition.y = insertionBoatWaterY;
        insertionBoatRoll = 0.0f;
        insertionBoatLanded = true;
        insertionBoatDroppedPlayer = insertionBoatCarryingPlayer;
        insertionBoatCarryingPlayer = false;
        insertionBoatUnloadTimer = 0.0f;
        insertionBoatPhase = InsertionBoatPhase::Unloading;
    }

public:

    // BlackHawk: flies the player in at level start. The normal run lands; the
    // fast run holds above the spawn while the player rappels down. Enemy fire
    // can send either route through the same crash sequence.
    enum class BlackHawkPhase {
        Inbound, Descending, Rappelling, Unloading, Departing, Crashing, Down,
        Gone };

    static constexpr float BlackHawkStartHeight = 90.0f;
    static constexpr float BlackHawkDescentSpeed = 6.5f;
    // Where the approach begins relative to the drop-off point, and how fast it
    // covers that ground.
    static constexpr float BlackHawkApproachDistance = 220.0f;
    static constexpr float BlackHawkApproachHeight = 55.0f;
    static constexpr float BlackHawkApproachSpeed = 42.0f;
    static constexpr float BlackHawkFastSpeedMultiplier = 2.0f;
    static constexpr float BlackHawkRappelHoverHeight = 14.0f;
    static constexpr float BlackHawkRappelTime = 2.25f;
    // Radius inside which the approach is considered finished and the descent
    // takes over.
    static constexpr float BlackHawkHoverRadius = 1.5f;
    // Seconds spent on the ground with the doors open before lifting off.
    static constexpr float BlackHawkUnloadTime = 3.0f;
    static constexpr float BlackHawkDepartSpeed = 18.0f;
    // Height above the drop-off point at which the bird stops being drawn.
    static constexpr float BlackHawkDepartHeight = 140.0f;

    // Enemy fire is the only thing that reduces insertion airframe integrity.
    static constexpr float BlackHawkMaxHealth = 300.0f;
    // Below this fraction the airframe starts trailing smoke.
    static constexpr float BlackHawkSmokeThreshold = 0.55f;
    // Downward acceleration and tumble rates once the engine quits.
    static constexpr float BlackHawkCrashGravity = 9.81f;
    static constexpr float BlackHawkCrashPitchRate = 0.42f;
    static constexpr float BlackHawkCrashRollRate = 0.78f;
    static constexpr float BlackHawkCrashYawRate = 0.55f;

    float blackHawkHealth = BlackHawkMaxHealth;
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
    float blackHawkRappelProgress = 0.0f;
    BlackHawkPhase blackHawkPhase = BlackHawkPhase::Inbound;
    bool blackHawkLanded = false;
    // Set for the single frame the skids touch down, so the caller can release
    // the player at the drop-off point.
    bool blackHawkDroppedPlayer = false;
    bool blackHawkVisible = true;
    // True while the player is riding in the cabin, before the drop-off.
    bool blackHawkCarryingPlayer = false;
    bool blackHawkFastRappel = false;

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

    // Where the rappel rope is tied on, in the same local frame as the ride
    // point above. Taken from a "RopeAnchor" empty in the GLB when the model has
    // one; otherwise it falls back to the ride point, so a model without the
    // node still hangs a rope from somewhere sensible rather than from the
    // origin. blackHawkRopeAnchorValid records which of the two is in use.
    float blackHawkRopeSide = 1.15f;
    float blackHawkRopeForward = 0.4f;
    float blackHawkRopeHeight = 2.1f;
    bool blackHawkRopeAnchorValid = false;

    // The live rope, owned by the app layer (see the forward declaration above).
    // Null whenever no rope is out, which is every phase but Rappelling and the
    // tail of a cut rope still falling.
    RopeSwing* blackHawkRope = nullptr;
    // Raised for the single frame the rope should be hung, so the owner can
    // build the box3d world without this header knowing how. Paired with
    // blackHawkRopeReleaseRequested, which asks for it to be torn down.
    bool blackHawkRopeSpawnRequested = false;
    bool blackHawkRopeReleaseRequested = false;
    // True once the rope has been cut from under a descending player: the
    // descent stops being rope-driven and the player is handed to gravity.
    bool blackHawkRopeCut = false;
    // How far down the rope the player had got when it was cut, 0..1. Drives
    // fall damage -- a cut near the ground should barely sting.
    float blackHawkRopeCutProgress = 0.0f;

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

    // Where the rappel rope is tied on, given the current pose. Same basis and
    // the same full roll/pitch/yaw rotation as BlackHawkRidePosition, so the
    // rope stays on the airframe through the flare and the departure bank.
    DirectX::XMFLOAT3 BlackHawkRopeAnchorPosition() const {
        const DirectX::XMVECTOR offset = DirectX::XMVectorSet(
            blackHawkRopeSide, blackHawkRopeHeight, blackHawkRopeForward, 0.0f);
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

    bool BlackHawkIsRappelling() const {
        return blackHawkPhase == BlackHawkPhase::Rappelling &&
               blackHawkCarryingPlayer;
    }

    // Records that the rope was shot through while the player was on it. Latches
    // the progress at the moment of the cut, which is what the fall height and so
    // the damage are worked out from. Ignored unless a descent is actually in
    // progress, so a stray hit on a departing aircraft cannot fake a cut.
    bool NotifyBlackHawkRopeCut() {
        if (!BlackHawkIsRappelling() || blackHawkRopeCut) return false;
        blackHawkRopeCut = true;
        blackHawkRopeCutProgress = blackHawkRappelProgress;
        return true;
    }

    // True while a rope is out and still worth simulating: either the player is
    // riding it down, or it has been cut and the severed section is still
    // falling. The owner uses this to decide whether to step the simulation.
    bool BlackHawkRopeActive() const {
        return blackHawkRope != nullptr &&
               (blackHawkPhase == BlackHawkPhase::Rappelling || blackHawkRopeCut);
    }

    // Places the drop-off at the player spawn and parks the helicopter at the
    // start of its approach run, inbound on the given heading (radians, the
    // direction it flies toward).
    // Started only for a level that inserts by helicopter, so the run always
    // carries the player. A level using some other craft calls
    // DisableBlackHawkInsertion instead.
    void BeginBlackHawkInsertion(const DirectX::XMFLOAT3& dropOff,
                                 float groundY, float approachHeading,
                                 bool fastRappel = false) {
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
        blackHawkFastRappel = fastRappel;
        blackHawkHealth = BlackHawkMaxHealth;
        blackHawkCrashVelocity = { 0.0f, 0.0f, 0.0f };
        blackHawkUnloadTimer = 0.0f;
        blackHawkRappelProgress = 0.0f;
        // A restart must not inherit the last run's rope, cut or otherwise.
        blackHawkRopeSpawnRequested = false;
        blackHawkRopeReleaseRequested = true;
        blackHawkRopeCut = false;
        blackHawkRopeCutProgress = 0.0f;
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
            const float speedScale = blackHawkFastRappel
                ? BlackHawkFastSpeedMultiplier : 1.0f;
            const float speed = BlackHawkApproachSpeed * speedScale *
                (0.14f + 0.86f * closing);
            const float step = (std::min)(distance, speed * dt);
            if (distance > 0.001f) {
                blackHawkPosition.x += dx / distance * step;
                blackHawkPosition.z += dz / distance * step;
            }
            // Nose down while running in, levelling out into the flare.
            blackHawkPitch = -0.16f * closing;
            const float hoverY =
                blackHawkGroundY + BlackHawkRappelHoverHeight;
            const float cruiseY = blackHawkGroundY + BlackHawkApproachHeight;
            blackHawkPosition.y = hoverY + (cruiseY - hoverY) * closing;
            if (distance <= BlackHawkHoverRadius) {
                blackHawkPosition.x = blackHawkDropOff.x;
                blackHawkPosition.y = hoverY;
                blackHawkPosition.z = blackHawkDropOff.z;
                blackHawkPitch = 0.0f;
                blackHawkPhase = blackHawkFastRappel
                    ? BlackHawkPhase::Rappelling
                    : BlackHawkPhase::Descending;
                // Only the fast route rappels, so only it gets a rope. The
                // normal route lands on its skids and never hangs one.
                if (blackHawkPhase == BlackHawkPhase::Rappelling)
                    blackHawkRopeSpawnRequested = true;
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
        case BlackHawkPhase::Rappelling: {
            // A cut rope stops driving the descent: the player is falling under
            // gravity now, so advancing progress would keep pulling them down a
            // rope that is no longer there. Release them and let the bird go.
            if (blackHawkRopeCut) {
                blackHawkDroppedPlayer = blackHawkCarryingPlayer;
                blackHawkCarryingPlayer = false;
                blackHawkPhase = BlackHawkPhase::Departing;
                break;
            }
            blackHawkRappelProgress = (std::min)(
                1.0f, blackHawkRappelProgress + dt / BlackHawkRappelTime);
            if (blackHawkRappelProgress >= 1.0f) {
                blackHawkDroppedPlayer = blackHawkCarryingPlayer;
                blackHawkCarryingPlayer = false;
                blackHawkPhase = BlackHawkPhase::Departing;
                // Rope has done its job; the freed chain is not needed.
                blackHawkRopeReleaseRequested = true;
            }
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
            const float speedScale = blackHawkFastRappel
                ? BlackHawkFastSpeedMultiplier : 1.0f;
            blackHawkPosition.y += BlackHawkDepartSpeed * speedScale *
                (0.35f + 0.65f * ramp) * dt;
            const float forward = BlackHawkDepartSpeed * speedScale * ramp * dt;
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

    // Stands the helicopter down for a level that inserts by some other means.
    // Mirrors DisableInsertionBoat: dropping the route keeps reset from
    // resurrecting a run this level never wanted.
    void DisableBlackHawkInsertion() {
        blackHawkRouteValid = false;
        blackHawkVisible = false;
        blackHawkCarryingPlayer = false;
        blackHawkDroppedPlayer = false;
        blackHawkBailedOut = false;
        blackHawkJustCrashed = false;
        blackHawkLanded = false;
        blackHawkFastRappel = false;
        blackHawkRappelProgress = 0.0f;
        blackHawkHealth = BlackHawkMaxHealth;
        blackHawkCrashVelocity = { 0.0f, 0.0f, 0.0f };
        blackHawkPhase = BlackHawkPhase::Gone;
        // Never leak a box3d world across a level reset.
        blackHawkRopeSpawnRequested = false;
        blackHawkRopeReleaseRequested = true;
        blackHawkRopeCut = false;
        blackHawkRopeCutProgress = 0.0f;
    }

    // Kills the engine and starts the spiral, carrying whatever momentum the
    // current phase had into the fall.
    void BeginBlackHawkCrash() {
        if (blackHawkPhase == BlackHawkPhase::Crashing ||
            blackHawkPhase == BlackHawkPhase::Down) return;
        // A passenger already on the rope is released into normal falling
        // movement instead of snapping back inside the crashing airframe.
        if (blackHawkPhase == BlackHawkPhase::Rappelling &&
            blackHawkCarryingPlayer) {
            blackHawkCarryingPlayer = false;
            blackHawkBailedOut = true;
        }
        // The rope goes down with the aircraft either way: a wreck spiralling
        // away from a rope still tied to it looks worse than no rope at all.
        blackHawkRopeReleaseRequested = true;
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

    DamageResult DamageInsertionBoatFromEnemyFire(float damage) {
        DamageResult result{ insertionBoatPosition };
        if (damage <= 0.0f ||
            insertionBoatPhase == InsertionBoatPhase::Foundering ||
            insertionBoatPhase == InsertionBoatPhase::Sunk ||
            insertionBoatPhase == InsertionBoatPhase::Gone)
            return result;
        result.applied = true;
        insertionBoatHealth =
            (std::max)(0.0f, insertionBoatHealth - damage);
        if (insertionBoatHealth > 0.0f) return result;
        BeginInsertionBoatFounder();
        result.destroyed = true;
        return result;
    }

    DamageResult DamageInsertionBlackHawkFromEnemyFire(float damage) {
        DamageResult result{ blackHawkPosition };
        if (damage <= 0.0f ||
            blackHawkPhase == BlackHawkPhase::Crashing ||
            blackHawkPhase == BlackHawkPhase::Down ||
            blackHawkPhase == BlackHawkPhase::Gone)
            return result;
        result.applied = true;
        blackHawkHealth = (std::max)(0.0f, blackHawkHealth - damage);
        if (blackHawkHealth > 0.0f) return result;
        BeginBlackHawkCrash();
        result.destroyed = true;
        return result;
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

        // Same for the insertion boat: re-run it from the heading the route was
        // set up on, not the live yaw, which the turn-out and a founder list
        // both leave pointing somewhere arbitrary.
        insertionBoatRoll = 0.0f;
        insertionBoatSinkOffset = 0.0f;
        insertionBoatBobTime = 0.0f;
        insertionBoatJustSank = false;
        if (insertionBoatRouteValid) {
            BeginInsertionBoatRun(insertionBoatLanding, insertionBoatWaterY,
                                  insertionBoatApproachHeading);
        }

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
                                    blackHawkApproachHeading,
                                    blackHawkFastRappel);
        }
    }
};

#endif
