#ifndef VEHICLE_SYSTEM_H
#define VEHICLE_SYSTEM_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <vector>

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

    // ---- Anti-air emplacement ------------------------------------------------
    // A fixed twin-barrel AA gun. It leads and engages anything in the air --
    // the insertion BlackHawk above all, and the player too while they are
    // roped, carried or otherwise clear of the ground.
    //
    // It does not engage a player on foot. The barrels do not depress that far,
    // and it makes the emplacement something you approach on the ground and
    // destroy, rather than something that shoots you all the way in.
    //
    // Tuned to threaten rather than delete: the whole point is that an insertion
    // flown straight over the gun goes badly, and that the player has a reason
    // to deal with the emplacement before calling anything in.
    static constexpr float AATurretMaxHealth = 900.0f;
    static constexpr float AATurretAirRange = 190.0f;    // vs aircraft
    // Reach against the player, who is only ever engaged while airborne. Much
    // shorter than the range used against aircraft: a man on a rope is a small
    // target, and holding this in keeps the emplacement a threat you walk into
    // rather than one that covers the whole map.
    static constexpr float AATurretGroundRange = 85.0f;
    // Dead zone against ground targets. A fixed AA mount cannot depress onto
    // something at the foot of its own pedestal, and the perimeter deployment
    // ring can put a player as close as ~7 m to the gun -- without this, picking
    // that zone means being shot before the run is playable. Aircraft are
    // exempt: nothing flies inside this radius without having already hit it.
    static constexpr float AATurretGroundMinRange = 18.0f;
    // How far clear of the terrain the player must be before the gun will
    // engage them. The barrels do not depress onto men on foot, so anything at
    // walking height is not a target at all.
    //
    // 2.5 m rather than "off the ground": a jump peaks at JumpStrength^2 / 2g
    // = 5.0^2 / 19.6 = 1.28 m, so a threshold below that would let hopping past
    // the emplacement call down a burst. This clears the apex with margin while
    // still catching a rope, a rooftop or a fall.
    static constexpr float AATurretMinTargetAltitude = 2.5f;
    static constexpr float AATurretBurstShots = 4.0f;    // rounds per burst
    static constexpr float AATurretShotInterval = 0.11f; // within a burst
    static constexpr float AATurretBurstPause = 1.45f;   // between bursts
    static constexpr float AATurretShellSpeed = 2.4f;    // vs small arms
    static constexpr float AATurretShellDamage = 2.2f;
    static constexpr float AATurretYawRate = 1.5f;       // rad/s traverse
    static constexpr float AATurretPitchRate = 1.1f;     // rad/s elevation
    static constexpr float AATurretMinPitch = -0.12f;
    static constexpr float AATurretMaxPitch = 1.30f;     // ~75 deg, near vertical
    // Muzzle sits at the end of the barrels, measured from the turret origin.
    static constexpr float AATurretBarrelLength = 2.35f;
    static constexpr float AATurretMountHeight = 1.85f;

    // One emplacement. Held in a vector rather than as loose fields on the
    // system, so a level can carry several: the comm tower's gun plus any the
    // designer drops in from the editor's turret prefab.
    struct AATurret {
        DirectX::XMFLOAT3 position{};
        float yaw = 0.0f;
        float pitch = 0.35f;
        float health = AATurretMaxHealth;
        bool dead = false;
        float shotTimer = 0.0f;
        int shotsLeftInBurst = 0;
        // Barrels spin down after firing; cosmetic, drives the muzzle glow.
        float heat = 0.0f;

        bool Active() const { return !dead; }

        // Where the shells leave the gun, following the current attitude.
        DirectX::XMFLOAT3 Muzzle() const {
            const float horizontal = std::cos(pitch) * AATurretBarrelLength;
            return DirectX::XMFLOAT3{
                position.x + std::sin(yaw) * horizontal,
                position.y + AATurretMountHeight +
                    std::sin(pitch) * AATurretBarrelLength,
                position.z + std::cos(yaw) * horizontal };
        }

        float HealthFraction() const {
            return AATurretMaxHealth > 0.0f
                ? (std::max)(0.0f, (std::min)(1.0f, health / AATurretMaxHealth))
                : 0.0f;
        }
    };

    // Capped: each turret costs a draw call and its own target search, so a
    // level stuffed with them should not quietly sink the frame rate.
    static constexpr size_t kMaxAATurrets = 8;
    std::vector<AATurret> aaTurrets;

    // Adds an emplacement and returns its index, or kMaxAATurrets when full.
    size_t PlaceAATurret(const DirectX::XMFLOAT3& position) {
        if (aaTurrets.size() >= kMaxAATurrets) return kMaxAATurrets;
        AATurret turret;
        turret.position = position;
        aaTurrets.push_back(turret);
        return aaTurrets.size() - 1;
    }

    // True while any emplacement is still standing. Callers that need a
    // specific one index aaTurrets directly.
    bool AnyAATurretActive() const {
        for (const AATurret& turret : aaTurrets)
            if (turret.Active()) return true;
        return false;
    }

    // Slews the gun toward `target` and reports true on the frames a shell
    // leaves the barrel, so the caller can spawn the projectile and effects.
    //
    // `target` is where the gun should shoot, already led by the caller -- it
    // knows the shell speed and the target's velocity, and leading a helicopter
    // is the difference between a threat and a firework display.
    bool UpdateAATurret(AATurret& turret, float deltaTime,
                        const DirectX::XMFLOAT3& target, bool hasTarget) {
        if (!turret.Active()) return false;
        const float dt = (std::max)(0.0f, deltaTime);
        turret.heat = (std::max)(0.0f, turret.heat - dt * 1.6f);
        if (!hasTarget) {
            // Nothing to shoot: stop mid-burst rather than emptying into air.
            turret.shotsLeftInBurst = 0;
            return false;
        }

        const float dx = target.x - turret.position.x;
        const float dz = target.z - turret.position.z;
        const float dy = target.y - (turret.position.y + AATurretMountHeight);
        const float horizontal = std::sqrt(dx * dx + dz * dz);

        // Traverse and elevate at a finite rate. A turret that snaps to its
        // target is both trivial to dodge-check and reads as a hitscan cheat;
        // the slew is what makes flying wide of the gun a real option.
        const float desiredYaw = std::atan2(dx, dz);
        float yawDelta = std::atan2(std::sin(desiredYaw - turret.yaw),
                                    std::cos(desiredYaw - turret.yaw));
        const float maxYawStep = AATurretYawRate * dt;
        yawDelta = (std::max)(-maxYawStep, (std::min)(maxYawStep, yawDelta));
        turret.yaw += yawDelta;

        const float desiredPitch = std::atan2(dy, (std::max)(0.001f, horizontal));
        float pitchDelta = desiredPitch - turret.pitch;
        const float maxPitchStep = AATurretPitchRate * dt;
        pitchDelta = (std::max)(-maxPitchStep, (std::min)(maxPitchStep, pitchDelta));
        turret.pitch = (std::max)(AATurretMinPitch,
            (std::min)(AATurretMaxPitch, turret.pitch + pitchDelta));

        // Only fire once roughly on target, so the burst does not spray while
        // the mount is still swinging around.
        const bool onTarget = std::fabs(yawDelta) < 0.05f &&
                              std::fabs(desiredPitch - turret.pitch) < 0.06f;

        turret.shotTimer -= dt;
        if (turret.shotTimer > 0.0f) return false;
        if (!onTarget) return false;

        if (turret.shotsLeftInBurst <= 0) {
            turret.shotsLeftInBurst = static_cast<int>(AATurretBurstShots);
            turret.shotTimer = AATurretShotInterval;
            --turret.shotsLeftInBurst;
            turret.heat = 1.0f;
            return true;
        }
        --turret.shotsLeftInBurst;
        turret.shotTimer = turret.shotsLeftInBurst > 0
            ? AATurretShotInterval : AATurretBurstPause;
        turret.heat = 1.0f;
        return true;
    }

    // Where to aim to hit a target moving at `velocity`. Solves the intercept
    // iteratively: the flight time depends on the lead point, which depends on
    // the flight time. Two passes is plenty at these ranges and speeds.
    DirectX::XMFLOAT3 AATurretLeadPoint(const AATurret& turret,
                                        const DirectX::XMFLOAT3& target,
                                        const DirectX::XMFLOAT3& velocity,
                                        float shellSpeed) const {
        DirectX::XMFLOAT3 aim = target;
        if (shellSpeed <= 0.001f) return aim;
        const DirectX::XMFLOAT3 muzzle = turret.Muzzle();
        for (int i = 0; i < 2; ++i) {
            const float dx = aim.x - muzzle.x;
            const float dy = aim.y - muzzle.y;
            const float dz = aim.z - muzzle.z;
            const float t = std::sqrt(dx * dx + dy * dy + dz * dz) / shellSpeed;
            aim = { target.x + velocity.x * t,
                    target.y + velocity.y * t,
                    target.z + velocity.z * t };
        }
        return aim;
    }

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

    // ---- Enemy reinforcement dropship ---------------------------------------
    // Reinforcements arrive on the craft the player's own insertion uses: the
    // gunship flies in, holds a hover, and fast-ropes a squad down. It reuses
    // the secondaryHelicopter* fields above rather than adding a second
    // airframe, so only one wave is ever in the air -- a second call-in waits
    // for the current craft to clear. That cap is the whole reason this is a
    // state enum on shared fields instead of a pool.
    enum class DropshipState : uint8_t {
        // Nothing inbound. secondaryHelicopter* is owned by the patrol path.
        Idle = 0,
        // Flying from the map edge toward the drop point.
        Inbound = 1,
        // Holding over the drop point, releasing troops on an interval.
        Unloading = 2,
        // Empty and climbing out toward the edge it came from.
        Departing = 3
    };

    // Cruise speed on the way in and out (m/s). Inbound is the slower of the
    // two: the approach is meant to be seen and shot at, the exit is not.
    static constexpr float DropshipInboundSpeed = 26.0f;
    static constexpr float DropshipDepartSpeed = 34.0f;
    // Hover altitude above the drop point's terrain height. High enough to
    // clear the palms, low enough that the rope reads as reaching the ground.
    static constexpr float DropshipHoverHeight = 18.0f;
    // Horizontal distance at which the approach is considered arrived.
    static constexpr float DropshipArriveRadius = 3.5f;
    // Seconds between troops leaving the craft. Paces a squad down the rope
    // instead of teleporting the whole wave in one frame.
    static constexpr float DropshipUnloadInterval = 0.85f;
    // Held after the last troop before the craft leaves, so the airframe does
    // not snap from unloading to departing on the same frame the squad lands.
    static constexpr float DropshipUnloadTrailSeconds = 1.1f;
    // Distance from the drop point at which a departing craft is done and the
    // slot returns to Idle.
    static constexpr float DropshipDepartDistance = 190.0f;
    // Where troops leave the craft, relative to the hover. They fall the rest
    // of the way -- see the fast-rope note in the update.
    static constexpr float DropshipRopeDropHeight = 1.6f;

    DropshipState dropshipState = DropshipState::Idle;
    DirectX::XMFLOAT3 dropshipDropPoint{};
    DirectX::XMFLOAT3 dropshipEntryPoint{};
    float dropshipUnloadTimer = 0.0f;
    int dropshipTroopsLeft = 0;
    // Counts waves called this run. Drives the escalating squad size so the
    // second call-in is heavier than the first.
    uint32_t dropshipWavesCalled = 0;

    // ---- Escape boat ---------------------------------------------------------
    // Exfil. Sits on the water out past the shoreline, under the lane the
    // reinforcement dropship flies in along, so the way out is the same
    // direction the enemy keeps arriving from. Reaching it finishes the level.
    //
    // Placed once per run, when the objective aircraft is resolved: before that
    // the mission has no ending to offer, and an exfil boat on the water from
    // the opening second would advertise one the player has not earned.
    static constexpr float EscapeBoatBoardRadius = 6.5f;
    // How far out from the island centre the boat waits, when nothing wider is
    // asked for.
    //
    // The beach profile crosses the waterline around 43 m (y = +0.04 at 40 m,
    // y = -0.21 at 42 m), and the seabed finishes sloping to full depth at 88 m.
    // 50 m sits clear of the shelf in genuinely deep water with the whole outer
    // slope still beyond it, so the hull never grounds and the swim out is a
    // real leg of the run rather than a step off the sand.
    //
    // Was 42 m, which floated the boat on the very edge of the shelf.
    static constexpr float EscapeBoatShoreDistance = 50.0f;

    // Clearance the exfil keeps outside the insertion ring.
    //
    // The way out has to read as further out than the way in, and the ring is
    // the only distance the player has actually seen before the boat appears --
    // deploying from it is the first thing a run does. A boat inside or level
    // with it would make the exfil look like a walk back to the start rather
    // than a leg out to sea.
    //
    // 16 m because the ring's 20 points sit ~10 m apart on the default 34 m
    // radius: a smaller gap and the boat reads as just another ring point on
    // the horizon instead of somewhere past them.
    static constexpr float EscapeBoatRingClearance = 16.0f;

    // Where the boat sits for a run whose insertion ring has this radius.
    //
    // The floor keeps a tight ring (authorable down to 5 m) from parking the
    // hull on the beach shelf, and the clearance keeps a wide one (up to 600 m)
    // from swallowing the boat inside the ring it is meant to sit beyond.
    static constexpr float EscapeBoatDistanceForRing(float deploymentRadius) {
        const float beyondRing = deploymentRadius + EscapeBoatRingClearance;
        return beyondRing > EscapeBoatShoreDistance ? beyondRing
                                                    : EscapeBoatShoreDistance;
    }

    bool escapeBoatActive = false;
    DirectX::XMFLOAT3 escapeBoatPosition{};
    float escapeBoatYaw = 0.0f;
    // Drives the idle bob so the hull sits on the swell rather than frozen.
    float escapeBoatBobTime = 0.0f;

    bool EscapeBoatReady() const { return escapeBoatActive; }

    // Places the exfil on an explicit compass bearing, measured the same way the
    // dropship's is (+Z = 0, turning through +X).
    //
    // The bearing is chosen by the caller, which owns the run's RNG -- the boat
    // now sits somewhere different every run rather than always on the lane the
    // first reinforcement wave flew in along. A fixed exfil meant the way out
    // was known before the mission started; a rolled one has to be found.
    //
    // `shoreDistance` is how far out along that bearing the hull sits. Pass
    // EscapeBoatDistanceForRing(...) so the exfil clears the run's own
    // insertion ring rather than a radius fixed at compile time.
    //
    // Idempotent: later waves must not move an exfil the player may already be
    // swimming toward.
    void PlaceEscapeBoatOnBearing(float bearingRadians, float waterY,
                                  float shoreDistance) {
        if (escapeBoatActive) return;
        const float nx = std::sin(bearingRadians);
        const float nz = std::cos(bearingRadians);
        escapeBoatPosition = {
            nx * shoreDistance, waterY, nz * shoreDistance };
        // Bow pointed out to sea, the way it would leave.
        escapeBoatYaw = std::atan2(nx, nz);
        escapeBoatBobTime = 0.0f;
        escapeBoatActive = true;
    }

    void UpdateEscapeBoat(float dt) {
        if (!escapeBoatActive || dt <= 0.0f) return;
        escapeBoatBobTime += dt;
    }

    // Vertical bob, applied at render so the hull rides the swell.
    float EscapeBoatBobOffset() const {
        return std::sin(escapeBoatBobTime * 0.9f) * 0.22f +
               std::sin(escapeBoatBobTime * 1.7f) * 0.08f;
    }

    // True once the player is close enough to board. Horizontal only: the boat
    // sits at water level and the player may be swimming or standing on deck.
    bool PlayerCanBoardEscapeBoat(const DirectX::XMFLOAT3& player) const {
        if (!escapeBoatActive) return false;
        const float dx = player.x - escapeBoatPosition.x;
        const float dz = player.z - escapeBoatPosition.z;
        return dx * dx + dz * dz <=
               EscapeBoatBoardRadius * EscapeBoatBoardRadius;
    }

    void ResetEscapeBoat() {
        escapeBoatActive = false;
        escapeBoatPosition = {};
        escapeBoatYaw = 0.0f;
        escapeBoatBobTime = 0.0f;
    }

    bool DropshipActive() const { return dropshipState != DropshipState::Idle; }

    // A wave can only be called when the slot is free and the airframe is
    // still flyable -- a downed gunship stays down for the rest of the run.
    bool DropshipAvailable() const {
        return dropshipState == DropshipState::Idle && !secondaryHelicopterDead;
    }

    // Arms a wave. `entry` is where the craft comes from (map edge), `drop` is
    // the hover it unloads over. Caller supplies both because only the app
    // layer knows the terrain and the player's position.
    void BeginDropshipRun(const DirectX::XMFLOAT3& entry,
                          const DirectX::XMFLOAT3& drop, int troops) {
        if (!DropshipAvailable() || troops <= 0) return;
        dropshipEntryPoint = entry;
        dropshipDropPoint = drop;
        dropshipTroopsLeft = troops;
        dropshipUnloadTimer = DropshipUnloadInterval;
        dropshipState = DropshipState::Inbound;
        ++dropshipWavesCalled;
        // Take the airframe off the patrol path and put it at the entry point.
        secondaryHelicopterPosition = entry;
        secondaryHelicopterCrashed = false;
    }

    void ResetDropship() {
        dropshipState = DropshipState::Idle;
        dropshipDropPoint = {};
        dropshipEntryPoint = {};
        dropshipUnloadTimer = 0.0f;
        dropshipTroopsLeft = 0;
        dropshipWavesCalled = 0;
    }

    // Steps the dropship flight. Returns the number of troops that should be
    // released this frame -- the caller spawns them, because spawning needs the
    // model and bandit list this struct deliberately does not know about.
    //
    // Position is integrated here rather than lerped along a fixed path so a
    // craft knocked around by damage still flies a sane line.
    int UpdateDropship(float dt, float dropPointGroundY) {
        if (dropshipState == DropshipState::Idle) return 0;
        // A gunship shot down mid-run stops being a dropship immediately; the
        // crash path in the app layer takes the airframe from here.
        if (secondaryHelicopterDead) {
            dropshipState = DropshipState::Idle;
            dropshipTroopsLeft = 0;
            return 0;
        }
        if (dt <= 0.0f) return 0;

        const float hoverY = dropPointGroundY + DropshipHoverHeight;
        int released = 0;

        switch (dropshipState) {
        case DropshipState::Inbound: {
            const bool arrived = StepDropshipToward(
                { dropshipDropPoint.x, hoverY, dropshipDropPoint.z },
                DropshipInboundSpeed, dt);
            if (arrived) dropshipState = DropshipState::Unloading;
            break;
        }
        case DropshipState::Unloading: {
            // Hold station. Small drift keeps the hover from looking frozen.
            StepDropshipToward({ dropshipDropPoint.x, hoverY, dropshipDropPoint.z },
                               DropshipInboundSpeed * 0.35f, dt);
            dropshipUnloadTimer -= dt;
            if (dropshipUnloadTimer <= 0.0f) {
                if (dropshipTroopsLeft > 0) {
                    --dropshipTroopsLeft;
                    ++released;
                    dropshipUnloadTimer = dropshipTroopsLeft > 0
                        ? DropshipUnloadInterval
                        : DropshipUnloadTrailSeconds;
                } else {
                    dropshipState = DropshipState::Departing;
                }
            }
            break;
        }
        case DropshipState::Departing: {
            // Climb out toward the entry point. Not a hard arrival test: the
            // craft is done once it is far enough away to be off the play area.
            StepDropshipToward({ dropshipEntryPoint.x,
                                 hoverY + 12.0f,
                                 dropshipEntryPoint.z },
                               DropshipDepartSpeed, dt);
            const float dx = secondaryHelicopterPosition.x - dropshipDropPoint.x;
            const float dz = secondaryHelicopterPosition.z - dropshipDropPoint.z;
            if (dx * dx + dz * dz >=
                    DropshipDepartDistance * DropshipDepartDistance)
                dropshipState = DropshipState::Idle;
            break;
        }
        case DropshipState::Idle:
        default:
            break;
        }
        return released;
    }

    // Where a released troop leaves the airframe.
    DirectX::XMFLOAT3 DropshipTroopReleasePoint() const {
        return DirectX::XMFLOAT3{
            secondaryHelicopterPosition.x,
            secondaryHelicopterPosition.y - DropshipRopeDropHeight,
            secondaryHelicopterPosition.z };
    }

private:
    // Moves the airframe toward `target` at `speed`, turning to face the way it
    // is going. Returns true once within DropshipArriveRadius horizontally.
    bool StepDropshipToward(const DirectX::XMFLOAT3& target, float speed,
                            float dt) {
        const float dx = target.x - secondaryHelicopterPosition.x;
        const float dy = target.y - secondaryHelicopterPosition.y;
        const float dz = target.z - secondaryHelicopterPosition.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);

        // Vertical is always eased toward the target height, so the craft
        // settles onto the hover altitude even while station-keeping.
        const float verticalLerp = 1.0f - std::exp(-1.35f * dt);
        secondaryHelicopterPosition.y += dy * verticalLerp;

        if (horizontal > 0.001f) {
            const float step = (std::min)(speed * dt, horizontal);
            secondaryHelicopterPosition.x += dx / horizontal * step;
            secondaryHelicopterPosition.z += dz / horizontal * step;

            // Face the direction of travel, and bank into the turn.
            const float desiredYaw = std::atan2(dx, dz);
            const float yawDelta = std::atan2(
                std::sin(desiredYaw - secondaryHelicopterYaw),
                std::cos(desiredYaw - secondaryHelicopterYaw));
            const float yawLerp = 1.0f - std::exp(-2.1f * dt);
            secondaryHelicopterYaw += yawDelta * yawLerp;

            const float attitudeLerp = 1.0f - std::exp(-2.4f * dt);
            const float desiredRoll = (std::max)(-0.22f,
                (std::min)(0.22f, -yawDelta * 0.55f));
            // Nose down under way, level in the hover.
            const float desiredPitch = horizontal > DropshipArriveRadius
                ? -0.09f : 0.0f;
            secondaryHelicopterRoll +=
                (desiredRoll - secondaryHelicopterRoll) * attitudeLerp;
            secondaryHelicopterPitch +=
                (desiredPitch - secondaryHelicopterPitch) * attitudeLerp;
        }
        return horizontal <= DropshipArriveRadius;
    }

public:

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
    // The insertion never fully sets down: it flares to a low hover and the
    // player steps off from there. Kept as an offset above blackHawkGroundY
    // rather than baked into that value, because the departure climb and the
    // crash impact both measure against the real terrain height.
    static constexpr float BlackHawkTouchdownHoverHeight = 7.0f;
    static constexpr float BlackHawkRappelTime = 2.25f;
    // Radius inside which the approach is considered finished and the descent
    // takes over.
    static constexpr float BlackHawkHoverRadius = 1.5f;
    // Seconds spent on the ground with the doors open before lifting off.
    static constexpr float BlackHawkUnloadTime = 3.0f;
    static constexpr float BlackHawkDepartSpeed = 18.0f;
    // Departure profile. A helicopter leaving a landing zone does not go
    // straight up: it lifts a little, noses over to build forward speed, and
    // only then climbs away. Vertical is a fraction of the cruise speed for
    // that reason, and the ramp is long enough that the transition is visible
    // rather than instant.
    static constexpr float BlackHawkDepartClimbRate = 0.34f;
    static constexpr float BlackHawkDepartRampHeight = 55.0f;
    // How much of the climb passes before it starts translating forward. Below
    // this it is still coming light on the skids.
    static constexpr float BlackHawkDepartNoseOverStart = 0.16f;
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
    // Differenced each UpdateBlackHawk; see BlackHawkVelocity().
    DirectX::XMFLOAT3 blackHawkVelocity{};
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
        // Snapshot for the velocity difference taken at the end of this update.
        const DirectX::XMFLOAT3 poseAtEntry = blackHawkPosition;
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
            // Stops a metre up rather than on the skids, so the drop-off reads
            // as a hover the player jumps down from.
            const float hoverY =
                blackHawkGroundY + BlackHawkTouchdownHoverHeight;
            const float remaining = blackHawkPosition.y - hoverY;
            if (remaining <= 0.01f) {
                TouchDown();
                break;
            }
            // Full speed while high, tapering to a slow flare in the last 12 m.
            const float flare = (std::min)(1.0f, remaining / 12.0f);
            const float speed = BlackHawkDescentSpeed * (0.12f + 0.88f * flare);
            blackHawkPosition.y =
                (std::max)(hoverY, blackHawkPosition.y - speed * dt);
            if (blackHawkPosition.y <= hoverY + 0.01f) TouchDown();
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
            // Climb out along the heading it arrived on. The airframe lifts
            // first, noses over into forward flight, then settles into a climbing
            // cruise -- rather than rising and accelerating on one shared curve.
            const float climbed = blackHawkPosition.y - blackHawkGroundY;
            const float ramp =
                (std::min)(1.0f, climbed / BlackHawkDepartRampHeight);
            // Ease the ramp so the transition is smooth at both ends instead of
            // snapping to full speed the moment the height is reached.
            const float eased = ramp * ramp * (3.0f - 2.0f * ramp);
            const float speedScale = blackHawkFastRappel
                ? BlackHawkFastSpeedMultiplier : 1.0f;
            // Vertical is a modest fraction of cruise: it gains most of its
            // speed going forward, not straight up.
            blackHawkPosition.y += BlackHawkDepartSpeed * speedScale *
                BlackHawkDepartClimbRate * (0.45f + 0.55f * eased) * dt;
            // Forward only after the initial lift, so it clears the LZ before
            // translating away rather than sliding off sideways at once.
            const float noseOver = (std::max)(0.0f,
                (eased - BlackHawkDepartNoseOverStart)) /
                (1.0f - BlackHawkDepartNoseOverStart);
            const float forward =
                BlackHawkDepartSpeed * speedScale * noseOver * dt;
            blackHawkPosition.x += std::sin(blackHawkYaw) * forward;
            blackHawkPosition.z += std::cos(blackHawkYaw) * forward;
            // Nose down into the acceleration, not up: the negative pitch is
            // what a helicopter does to translate forward. It levels off as the
            // climb settles.
            blackHawkPitch = -0.16f * noseOver * (1.0f - 0.45f * eased);
            blackHawkRoll = 0.10f * noseOver;
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

        // Velocity by difference, for anything that needs to lead the airframe
        // (the AA gun). Guarded on dt: the insertion is stepped with dt == 0 on
        // the frame it arms, and dividing by that would produce an infinity that
        // sends a lead solution off to nowhere.
        if (dt > 1e-5f) {
            blackHawkVelocity = {
                (blackHawkPosition.x - poseAtEntry.x) / dt,
                (blackHawkPosition.y - poseAtEntry.y) / dt,
                (blackHawkPosition.z - poseAtEntry.z) / dt };
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
        blackHawkVelocity = { 0.0f, 0.0f, 0.0f };
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

    // Airborne and still under power, so a gun has something worth shooting at.
    // Excludes Crashing and Down (already dealt with) and Gone (off the map).
    bool BlackHawkIsFlying() const {
        return blackHawkPhase != BlackHawkPhase::Crashing &&
               blackHawkPhase != BlackHawkPhase::Down &&
               blackHawkPhase != BlackHawkPhase::Gone &&
               !blackHawkLanded;
    }

    // Per-frame velocity, differenced from the previous pose rather than stored
    // by the flight code: the BlackHawk is driven by phase-based position
    // assignment, not by an integrated velocity there is any way to read.
    // Needed so the AA gun can lead its shots instead of firing where the
    // helicopter already was.
    const DirectX::XMFLOAT3& BlackHawkVelocity() const {
        return blackHawkVelocity;
    }

private:
    void TouchDown() {
        blackHawkPosition.y =
            blackHawkGroundY + BlackHawkTouchdownHoverHeight;
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

    DamageResult DamageAATurret(size_t index, float damage) {
        DamageResult result{};
        if (index >= aaTurrets.size()) return result;
        AATurret& turret = aaTurrets[index];
        result.position = turret.position;
        if (damage <= 0.0f || turret.dead) return result;
        result.applied = true;
        turret.health = (std::max)(0.0f, turret.health - damage);
        if (turret.health > 0.0f) return result;
        turret.dead = true;
        turret.shotsLeftInBurst = 0;
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
        // Cleared rather than re-placed: the level decides whether it has AA
        // guns and where, so a map without any must not inherit the last map's.
        aaTurrets.clear();

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
        ResetDropship();
        ResetEscapeBoat();

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
