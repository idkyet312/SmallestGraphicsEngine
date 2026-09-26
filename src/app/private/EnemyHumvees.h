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
static constexpr float kEnemyHumveeMaxThrottle = 0.75f;   // ~7 m/s
static constexpr float kEnemyHumveeSteerSign = -1.0f;
// Nose to chassis centre, metres (GroundVehicleSpec::chassisHalfExtents.x).
static constexpr float kEnemyHumveeHalfLength = 2.2f;

static bool HumveeHasLiveGunner(size_t vehicleIndex) {
    for (const auto& bandit : g_bandits)
        if (bandit && !bandit->networkControlled && bandit->turretGunner &&
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
        if (state.netTurretSeen && !playerDriving) {
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
