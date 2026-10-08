#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Enemy Humvees: a level Humvee whose turret gunner is alive drives itself the
// way an enemy tank does (EnemyTanks.h) -- closes on the nearest player, holds
// at a standoff for the gunner to work, backs off whatever it gets stuck on and
// never drives out to sea. The gunner already rides the chassis pose
// (HumveeTurretMountWorld), so moving the vehicle carries him with it.
//
// Kill the gunner and the Humvee stops where it is. The player taking the
// wheel turns the AI off for as long as they drive it.
//
// Drive conventions, from the shared wheel-joint setup:
//   - The rendered nose is chassis -X (HumveeWorldMatrix rotates the model
//     -PI/2 onto the physics pose; HumveeHeadlightPose says the same).
//   - Positive throttle rolls the chassis toward -X -- measured on the tank,
//     same joints -- so for the Humvee positive throttle is nose first.
//   - Only the +X axle steers, which is the rear one driving nose first, so
//     positive steering yaws the nose the opposite way it would on a
//     front-steered car. kEnemyHumveeSteerSign carries that.
// Both measured with SGE_HUMVEE_TRACE=1 + SGE_HUMVEE_DRIVE_TEST=1: throttle
// 0.75 ran nose first at 4-6.7 m/s, and a 0.33 rad heading error closed to
// 0.007 under steer = -1.45 * error.

static constexpr float kEnemyHumveeDetectRange = 130.0f;
// Where it stops closing: inside the gunner's effective range, outside the
// distance at which a player can simply walk up and throw something in.
static constexpr float kEnemyHumveeStandoff = 26.0f;
static constexpr float kEnemyHumveeMaxThrottle = 0.75f;   // ~14 m/s
static constexpr float kEnemyHumveeSteerSign = -1.0f;
// Nose to chassis centre, metres (GroundVehicleSpec::chassisHalfExtents.x).
static constexpr float kEnemyHumveeHalfLength = 2.2f;

static bool HumveeHasFriendlyGunner(size_t index) {
    if (ClientOwnedByHost() && index < g_humveeGameplay.size() &&
        g_humveeGameplay[index].netFriendlyGunner) return true;
    for (const auto& actor : g_bandits)
        if (actor && !actor->Dead() && actor->faction == Faction::Marine &&
            actor->humveeCrew.Gunner() &&
            actor->humveeCrew.vehicle == static_cast<int>(index)) return true;
    return false;
}

static XMFLOAT3 HumveeMarineSeatPosition(const SkinnedEnemy& actor) {
    const size_t index = static_cast<size_t>(actor.humveeCrew.vehicle);
    if (actor.humveeCrew.Gunner()) return HumveeTurretMountWorld(index);
    XMFLOAT4X4 pose;
    if (!HumveeVisualPose(index, pose)) return actor.position;
    const XMVECTOR offset = actor.humveeCrew.seat == 1
        ? XMVectorSet(-0.65f, -0.45f, -0.48f, 1.0f)
        : XMVectorSet(0.75f, -0.45f, 0.48f, 1.0f);
    XMFLOAT3 position;
    XMStoreFloat3(&position, XMVector3TransformCoord(offset, XMLoadFloat4x4(&pose)));
    return position;
}

static void PoseHumveeMarine(SkinnedEnemy& actor, float dt) {
    if (actor.Dead() || !actor.humveeCrew.Mounted()) return;
    const size_t index = static_cast<size_t>(actor.humveeCrew.vehicle);
    if (!HumveeAlive(index)) return;
    const XMFLOAT3 seat = HumveeMarineSeatPosition(actor);
    if (actor.humveeCrew.Gunner()) {
        XMFLOAT3 target = index < g_humveeGameplay.size()
            ? g_humveeGameplay[index].aimPoint : seat;
        if (ClientOwnedByHost()) {
            const float reach = 100.0f * std::cos(actor.aimPitch);
            target = { seat.x + std::sin(actor.aimYaw) * reach,
                       seat.y + actor.footOffset + 1.48f +
                           std::sin(actor.aimPitch) * 100.0f,
                       seat.z + std::cos(actor.aimYaw) * reach };
        }
        actor.UpdateMounted(dt, seat, target);
    } else {
        XMFLOAT4X4 pose;
        XMFLOAT3 forward;
        if (!HumveeVisualPose(index, pose, nullptr, &forward)) return;
        actor.UpdateHumveePassenger(dt, seat, std::atan2(-forward.x, -forward.z));
    }
}

static void RefreshHumveeMarinePoses(float dt) {
    for (auto& actor : g_bandits)
        if (actor) PoseHumveeMarine(*actor, dt);
}

// Seat ownership stays on each actor, so actor removal cannot leave stale pointers.
static void UpdateMarineHumveeCrew() {
    if (ClientOwnedByHost() || IsEditorEditing()) return;
    struct Driver { bool present = false; net::PlayerId owner = net::kInvalidPlayerId; };
    static std::vector<Driver> drivers;
    drivers.assign(g_humveeGameplay.size(), Driver{});
    if (g_drivingHumvee && g_activeHumveeIndex < drivers.size() &&
        !scene.player.downed && (scene.player.godMode || scene.player.health > 0.0f))
        drivers[g_activeHumveeIndex] = { true, g_netSession.LocalId() };
    if (g_netSession.CurrentRole() == net::Role::Host) {
        g_netSession.GetRemotePlayers(g_humveeSeatScratch);
        for (const net::RemotePlayer& remote : g_humveeSeatScratch)
            if (remote.vehicle.kind == net::DrivenVehicleKind::Humvee &&
                remote.vehicle.index < drivers.size() && !remote.downed &&
                remote.health > 0.0f)
                drivers[remote.vehicle.index] = { true, remote.id };
    }
    for (auto& actor : g_bandits) {
        if (!actor || !actor->humveeCrew.Mounted()) continue;
        const int vehicle = actor->humveeCrew.vehicle;
        if (!actor->Dead() && !actor->Held() &&
            static_cast<size_t>(vehicle) < drivers.size() &&
            drivers[vehicle].present && HumveeAlive(static_cast<size_t>(vehicle))) continue;
        if (!actor->Dead()) {
            XMFLOAT4X4 pose;
            if (HumveeVisualPose(static_cast<size_t>(vehicle), pose)) {
                const float side = actor->humveeCrew.seat == 2 ? 1.0f : -1.0f;
                XMStoreFloat3(&actor->position, XMVector3TransformCoord(
                    XMVectorSet(actor->humveeCrew.seat == 2 ? 1.0f : -0.8f,
                                0.0f, side * 2.6f, 1.0f), XMLoadFloat4x4(&pose)));
                actor->position.y = GroundHeightAt(actor->position.x, actor->position.z);
            }
        }
        actor->EndHumveeRide();
    }
    for (size_t index = 0; index < drivers.size(); ++index) {
        if (!drivers[index].present || !HumveeAlive(index)) continue;
        XMFLOAT4X4 pose;
        XMFLOAT3 centre;
        if (!HumveeVisualPose(index, pose, &centre)) continue;
        uint8_t occupied = 0;
        for (const auto& actor : g_bandits) {
            if (!actor || actor->Dead()) continue;
            if (actor->turretGunner && actor->mountedVehicleIndex == static_cast<int>(index))
                occupied |= 1u;
            if (actor->humveeCrew.Mounted() && actor->humveeCrew.vehicle == static_cast<int>(index))
                occupied |= static_cast<uint8_t>(1u << actor->humveeCrew.seat);
        }
        // A passenger takes over when the gunner is lost.
        if ((occupied & 1u) == 0) {
            for (auto& actor : g_bandits) {
                if (!actor || actor->Dead() || !actor->humveeCrew.Mounted() ||
                    actor->humveeCrew.vehicle != static_cast<int>(index)) continue;
                occupied &= static_cast<uint8_t>(~(1u << actor->humveeCrew.seat));
                actor->humveeCrew.Clear();
                actor->humveeCrew.TryBoard(static_cast<int>(index), occupied);
                actor->turretGunner = true;
                actor->mountedVehicleIndex = static_cast<int>(index);
                actor->PrepareHumveeRide();
                occupied |= 1u;
                break;
            }
        }
        while (occupied != 7u) {
            SkinnedEnemy* nearest = nullptr;
            float bestDistance = 5.5f * 5.5f;
            for (auto& actor : g_bandits) {
                if (!actor || actor->faction != Faction::Marine || actor->networkControlled ||
                    actor->Dead() || actor->Held() || actor->Rappelling() ||
                    actor->turretGunner || actor->humveeCrew.Mounted()) continue;
                if (actor->leashOwner != net::kInvalidPlayerId &&
                    actor->leashOwner != drivers[index].owner) continue;
                const float dx = actor->position.x - centre.x;
                const float dz = actor->position.z - centre.z;
                if (std::abs(actor->position.y - centre.y) > 3.0f) continue;
                const float distance = dx * dx + dz * dz;
                if (distance > bestDistance) continue;
                nearest = actor.get();
                bestDistance = distance;
            }
            if (!nearest || !nearest->humveeCrew.TryBoard(static_cast<int>(index), occupied)) break;
            nearest->turretGunner = nearest->humveeCrew.Gunner();
            nearest->mountedVehicleIndex = static_cast<int>(index);
            nearest->PrepareHumveeRide();
            occupied |= static_cast<uint8_t>(1u << nearest->humveeCrew.seat);
        }
    }
    for (auto& actor : g_bandits)
        if (actor && !actor->Dead() && actor->humveeCrew.Mounted())
            actor->position = HumveeMarineSeatPosition(*actor);
}

static bool HumveeHasLiveGunner(size_t vehicleIndex) {
    for (const auto& bandit : g_bandits)
        if (bandit && bandit->faction == Faction::Bandit &&
            !bandit->networkControlled && bandit->turretGunner &&
            !bandit->Dead() && bandit->mountedVehicleIndex >= 0 &&
            static_cast<size_t>(bandit->mountedVehicleIndex) == vehicleIndex)
            return true;
    return false;
}

static void TraceEnemyHumvees(float dt) {
    static const bool enabled =
        GetEnvironmentVariableA("SGE_HUMVEE_TRACE", nullptr, 0) > 0;
    static float timer = 0.0f;
    if (!enabled) return;
    timer += dt;
    if (timer < 1.0f) return;
    timer = 0.0f;
    for (size_t index = 0; index < g_humveeGameplay.size(); ++index) {
        const HumveeGameplayState& state = g_humveeGameplay[index];
        if (!state.aiEverDriven) continue;
        XMFLOAT4X4 pose;
        XMFLOAT3 position, forward, velocity;
        if (!g_destruction.GetVehicleTransform(index, pose, &position,
                                               &forward, &velocity)) continue;
        // Along the nose: positive is driving nose first.
        const float noseSpeed = -(velocity.x * forward.x +
                                  velocity.z * forward.z);
        SGE_LOG("LogGameplay", EngineLog::Level::Display,
            "Humvee " + std::to_string(index) +
            (state.aiDriving ? " AI" : " idle") +
            " pos " + std::to_string(position.x) + ", " +
            std::to_string(position.y) + ", " + std::to_string(position.z) +
            " noseSpeed " + std::to_string(noseSpeed) +
            " target " + std::to_string(state.aiTargetDistance) +
            " headErr " + std::to_string(state.aiHeadingError) +
            " throttle " + std::to_string(state.aiThrottle) +
            " steer " + std::to_string(state.aiSteering) +
            (state.aiReverseTime > 0.0f ? " REVERSING" : ""));
    }
}

// Physics input only, after ProcessInput has braked every Humvee the player is
// not driving. The pose comes back from the solver on the next step.
static void UpdateEnemyHumvees(float dt) {
    if (g_humveeGameplay.empty() || dt <= 0.0f || IsEditorEditing()) return;
    // A client's Humvees are driven by the host; SyncEnemyHumveePoses shows it.
    if (ClientOwnedByHost()) return;

    for (size_t index = 0; index < g_humveeGameplay.size(); ++index) {
        HumveeGameplayState& state = g_humveeGameplay[index];
        state.aiDriving = false;
        const bool playerDriving =
            (g_drivingHumvee && g_activeHumveeIndex == index) ||
            state.remoteDriven;
        if (playerDriving || !HumveeHasLiveGunner(index)) continue;
        XMFLOAT4X4 pose;
        XMFLOAT3 position, forward, velocity;
        if (!g_destruction.GetVehicleTransform(index, pose, &position,
                                               &forward, &velocity)) continue;

        XMFLOAT3 target{};
        bool engaged = NearestEnemyTankTarget(
            position, kEnemyHumveeDetectRange, target);
        // SGE_HUMVEE_DRIVE_TEST=1: drive at a fixed point 60 m off the left of
        // where the nose first pointed, player or not, so the throttle and
        // steering signs can be read off the trace unattended.
        static const bool driveTest =
            GetEnvironmentVariableA("SGE_HUMVEE_DRIVE_TEST", nullptr, 0) > 0;
        static std::vector<XMFLOAT3> driveTestTargets;
        if (driveTest) {
            if (driveTestTargets.size() <= index)
                driveTestTargets.resize(index + 1, XMFLOAT3{ FLT_MAX, 0, 0 });
            if (driveTestTargets[index].x == FLT_MAX) {
                // Nose is chassis -X; its left, seen from above, is the nose
                // turned by -90 degrees about +Y.
                const XMVECTOR nose = XMVector3Normalize(XMVectorSet(
                    -forward.x, 0.0f, -forward.z, 0.0f));
                const XMVECTOR left = XMVector3TransformNormal(
                    nose, XMMatrixRotationY(-XM_PIDIV2));
                XMStoreFloat3(&driveTestTargets[index],
                    XMLoadFloat3(&position) + (nose + left) * 42.0f);
            }
            target = driveTestTargets[index];
            engaged = true;
        }
        if (!engaged) {
            state.aiStuckTime = 0.0f;
            state.aiReverseTime = 0.0f;
            continue;   // stays braked by ProcessInput
        }
        state.aiDriving = true;
        state.aiEverDriven = true;

        const float dx = target.x - position.x;
        const float dz = target.z - position.z;
        const float distance = std::sqrt(dx * dx + dz * dz);
        state.aiTargetDistance = distance;
        const float speed = std::sqrt(velocity.x * velocity.x +
                                      velocity.z * velocity.z);
        // Nose heading in plan view.
        const float flat = std::sqrt(forward.x * forward.x +
                                     forward.z * forward.z);
        const float nx = flat > 1e-4f ? -forward.x / flat : -1.0f;
        const float nz = flat > 1e-4f ? -forward.z / flat : 0.0f;

        const float hereGround = GroundHeightAt(position.x, position.z);
        const auto drivable = [&](float x, float z) {
            const float ground = GroundHeightAt(x, z);
            return ground >= -kEnemyTankFordDepth || ground > hereGround + 0.3f;
        };

        float throttle = 0.0f;
        float steering = 0.0f;
        bool brake = true;
        if (state.aiReverseTime > 0.0f) {
            state.aiReverseTime -= dt;
            if (drivable(position.x - nx * 7.0f, position.z - nz * 7.0f)) {
                throttle = -0.6f;
                steering = state.aiReverseSteer;
                brake = false;
            } else {
                state.aiReverseTime = 0.0f;
            }
        } else if (distance > kEnemyHumveeStandoff) {
            const float wantX = dx / distance, wantZ = dz / distance;
            const float dot = nx * wantX + nz * wantZ;
            const float cross = nz * wantX - nx * wantZ;
            const float headingError = std::atan2(cross, dot);
            state.aiHeadingError = headingError;
            // `turn` is the yaw the nose needs; the sign turns it into wheel
            // angle for a rear-steered chassis.
            float turn = headingError * 1.45f;

            // Buildings and props are not all in the physics world: look past
            // the nose and swing away from whatever is there.
            XMFLOAT3 probeStart{ position.x + nx * (kEnemyHumveeHalfLength + 0.5f),
                                 position.y, position.z +
                                 nz * (kEnemyHumveeHalfLength + 0.5f) };
            XMFLOAT3 probeEnd{ probeStart.x + nx * 7.0f, probeStart.y,
                               probeStart.z + nz * 7.0f };
            XMFLOAT3 probeHit;
            if (HitPrefabColliderSegment(probeStart, probeEnd, 1.0f, probeHit,
                                         nullptr))
                turn = cross >= 0.0f ? 1.0f : -1.0f;
            steering = turn * kEnemyHumveeSteerSign;

            // Slow for a big turn, flat out on a straight run in.
            throttle = std::abs(headingError) > 1.2f ? 0.4f
                : (std::min)(kEnemyHumveeMaxThrottle, 0.35f +
                      (distance - kEnemyHumveeStandoff) / 40.0f);
            brake = false;
            // Never out to sea: back off with the nose swinging toward the
            // target instead, a three-point turn. Reversing moves the chassis
            // the other way, so the same yaw needs the opposite wheel angle.
            if (!drivable(position.x + nx * 8.0f, position.z + nz * 8.0f)) {
                throttle = 0.0f;
                brake = true;
                if (drivable(position.x - nx * 7.0f, position.z - nz * 7.0f)) {
                    state.aiReverseTime = 2.2f;
                    state.aiReverseSteer =
                        (cross >= 0.0f ? 1.0f : -1.0f) * -kEnemyHumveeSteerSign;
                }
            }

            // Stuck: pushing with nothing to show for it. Back out continuing
            // the same yaw, so the next attempt comes in at a new angle.
            if (throttle > 0.3f && speed < 0.5f) state.aiStuckTime += dt;
            else state.aiStuckTime = (std::max)(0.0f, state.aiStuckTime - dt);
            if (state.aiStuckTime > 2.0f) {
                state.aiStuckTime = 0.0f;
                state.aiReverseTime = 1.8f;
                state.aiReverseSteer = steering >= 0.0f ? -1.0f : 1.0f;
            }
        } else {
            state.aiStuckTime = 0.0f;
            state.aiHeadingError = 0.0f;
        }
        steering = (std::max)(-1.0f, (std::min)(1.0f, steering));
        state.aiThrottle = throttle;
        state.aiSteering = steering;
        g_destruction.SetVehicleInput(index, throttle, steering, brake);
    }
    TraceEnemyHumvees(dt);
}

// Client-side, after the physics step: put each host-driven Humvee where the
// host says it is, eased so it does not step at the net tick rate. The body is
// moved rather than just the draw: the player collides with it, the gunner
// mounts off it and the headlights follow it.
//
// Host-side, the same easing for a Humvee a remote player is driving: their
// machine runs it, and the host's body follows their reports so the host's
// player can see it, collide with it, and pass it on in the armor state.
static void SyncEnemyHumveePoses(float dt) {
    if (IsEditorEditing()) return;
    const float ease = 1.0f - std::exp(-14.0f * (std::max)(0.0f, dt));
    if (g_netSession.CurrentRole() == net::Role::Host) {
        for (size_t index = 0; index < g_humveeGameplay.size(); ++index) {
            HumveeGameplayState& state = g_humveeGameplay[index];
            if (!state.remoteDriven || !state.netPosed) continue;
            if (!HumveeHasFriendlyGunner(index))
                state.turretYaw += std::atan2(
                    std::sin(state.netTurretYaw - state.turretYaw),
                    std::cos(state.netTurretYaw - state.turretYaw)) * ease;
            XMStoreFloat3(&state.drawPosition, XMVectorLerp(
                XMLoadFloat3(&state.drawPosition),
                XMLoadFloat3(&state.netPosition), ease));
            XMStoreFloat4(&state.drawRotation, XMQuaternionSlerp(
                XMLoadFloat4(&state.drawRotation),
                XMLoadFloat4(&state.netRotation), ease));
            g_destruction.SetVehiclePose(index, state.drawPosition,
                                         state.drawRotation);
        }
        return;
    }
    if (!ClientOwnedByHost()) return;
    for (size_t index = 0; index < g_humveeGameplay.size(); ++index) {
        HumveeGameplayState& state = g_humveeGameplay[index];
        const bool playerDriving =
            g_drivingHumvee && g_activeHumveeIndex == index;
        if (state.netTurretSeen && (!playerDriving || HumveeHasFriendlyGunner(index))) {
            state.turretYaw += std::atan2(
                std::sin(state.netTurretYaw - state.turretYaw),
                std::cos(state.netTurretYaw - state.turretYaw)) * ease;
        }
        if (!state.netPosed || playerDriving) continue;
        XMStoreFloat3(&state.drawPosition, XMVectorLerp(
            XMLoadFloat3(&state.drawPosition),
            XMLoadFloat3(&state.netPosition), ease));
        XMStoreFloat4(&state.drawRotation, XMQuaternionSlerp(
            XMLoadFloat4(&state.drawRotation),
            XMLoadFloat4(&state.netRotation), ease));
        g_destruction.SetVehiclePose(index, state.drawPosition,
                                     state.drawRotation);
    }
}

// Diesel loop per running Humvee, pitched and swelled by load the way the
// boats are. "Running" is crewed -- this player, a remote driver, an AI or
// Marine gunner -- or visibly moving, which also covers a client's view of
// Humvees the host drives. Only the kHumveeEngineVoices nearest are voiced.
static void UpdateHumveeEngineAudio(float deltaTime) {
    struct EngineMotion {
        XMFLOAT3 previous{};
        float load = 0.0f;
        bool valid = false;
    };
    static std::vector<EngineMotion> motions;
    static size_t slotOwner[kHumveeEngineVoices] = {
        kNoHumvee, kNoHumvee, kNoHumvee, kNoHumvee };
    // ~18.7 m/s: kEnemyHumveeMaxThrottle is documented as ~14 m/s.
    constexpr float kFullSpeed = 14.0f / kEnemyHumveeMaxThrottle;
    constexpr float kMaxDistance = 120.0f;

    const bool audible = IsGameplayScreen() && !g_game.loading.Active() &&
        !IsEditorEditing() &&
        (scene.player.godMode || scene.player.health > 0.0f);
    motions.resize(g_humveeGameplay.size());

    struct Running { size_t index; XMFLOAT3 position; float distanceSq; };
    std::vector<Running> running;
    const float dt = (std::clamp)(deltaTime, 0.0f, 0.25f);
    for (size_t index = 0; audible && index < g_humveeGameplay.size(); ++index) {
        const HumveeGameplayState& state = g_humveeGameplay[index];
        EngineMotion& motion = motions[index];
        XMFLOAT4X4 pose;
        XMFLOAT3 position;
        if (state.dead ||
            !g_destruction.GetVehicleTransform(index, pose, &position)) {
            motion = {};
            continue;
        }
        const bool playerDriving =
            g_drivingHumvee && g_activeHumveeIndex == index;
        float targetLoad = 0.0f;
        if (motion.valid && deltaTime > 0.0001f) {
            const float dx = position.x - motion.previous.x;
            const float dz = position.z - motion.previous.z;
            targetLoad = (std::min)(1.0f,
                std::sqrt(dx * dx + dz * dz) / (deltaTime * kFullSpeed));
        }
        if (playerDriving)
            targetLoad = (std::max)(targetLoad,
                                    std::abs(state.playerThrottle) * 0.5f);
        else if (state.aiDriving)
            targetLoad = (std::max)(targetLoad, std::abs(state.aiThrottle) * 0.5f);
        // Eased so physics settling and snapshot corrections do not become
        // pitch jumps.
        motion.load += (targetLoad - motion.load) * (1.0f - std::exp(-4.0f * dt));
        motion.previous = position;
        motion.valid = true;

        const bool crewed = playerDriving || state.remoteDriven ||
            HumveeHasLiveGunner(index) || HumveeHasFriendlyGunner(index);
        if (!crewed && motion.load < 0.05f) continue;
        const float dx = position.x - scene.camera.Position.x;
        const float dy = position.y - scene.camera.Position.y;
        const float dz = position.z - scene.camera.Position.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq > kMaxDistance * kMaxDistance) continue;
        running.push_back({ index, position, distanceSq });
    }
    std::sort(running.begin(), running.end(),
        [](const Running& a, const Running& b) { return a.distanceSq < b.distanceSq; });
    if (running.size() > kHumveeEngineVoices) running.resize(kHumveeEngineVoices);

    // Keep a Humvee on the slot it already had, so its loop is not restarted
    // when another one crosses into or out of the nearest set.
    bool claimed[kHumveeEngineVoices] = {};
    std::vector<bool> placed(running.size(), false);
    for (size_t slot = 0; slot < kHumveeEngineVoices; ++slot)
        for (size_t i = 0; i < running.size(); ++i)
            if (!placed[i] && slotOwner[slot] == running[i].index) {
                claimed[slot] = placed[i] = true;
                break;
            }
    for (size_t slot = 0; slot < kHumveeEngineVoices; ++slot) {
        if (claimed[slot]) continue;
        slotOwner[slot] = kNoHumvee;
        for (size_t i = 0; i < running.size(); ++i)
            if (!placed[i]) {
                slotOwner[slot] = running[i].index;
                claimed[slot] = placed[i] = true;
                break;
            }
    }
    for (size_t slot = 0; slot < kHumveeEngineVoices; ++slot) {
        GunAudio& sound = g_humveeEngineAudio[slot];
        const auto owner = std::find_if(running.begin(), running.end(),
            [&](const Running& r) { return r.index == slotOwner[slot]; });
        if (owner == running.end()) {
            sound.StopLoop();
            continue;
        }
        const float load = motions[owner->index].load;
        sound.SetLoopAt(true, owner->position.x, owner->position.y,
            owner->position.z, 0.30f + load * 0.35f, 0.80f + load * 0.55f,
            kMaxDistance);
    }
}
