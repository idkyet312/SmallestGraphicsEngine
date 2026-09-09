#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void DamageHelicopter(float damage, const XMFLOAT3& hit) {
    if (damage <= 0.0f || g_helicopterDead || !g_helicopterModel) return;
    const VehicleSystem::DamageResult result =
        g_game.vehicles.DamagePrimaryHelicopter(damage);
    if (!result.applied) return;
    scene.SpawnSmokeBurst(hit, 0.22f, 0.10f);
    if (!result.destroyed) return;
    scene.SpawnExplosionFX(g_helicopterPosition, 7.0f, 1.0f);
    scene.SpawnSmokeBurst(g_helicopterPosition, 1.25f, 1.5f);
}

// The second airframe is present either as the stress-test patrol gunship or as
// an inbound reinforcement dropship. Both are shootable; a wave can be turned
// back by killing the craft before it unloads.
static bool SecondaryHelicopterPresent() {
    // A downed craft stays present until it has hit the ground, and its wreck
    // stays after that. UpdateDropship sets the state to Idle the instant the
    // airframe dies, so without the crash terms here the falling helicopter
    // would stop being drawn on the very frame it was destroyed.
    if (g_secondaryHelicopterDead)
        return g_helicopterModel != nullptr && scene.showHelicopter;
    return g_stressTestMode || g_game.vehicles.DropshipActive();
}

static void DamageSecondaryHelicopter(float damage, const XMFLOAT3& hit) {
    if (damage <= 0.0f || g_secondaryHelicopterDead || !g_helicopterModel ||
        !SecondaryHelicopterPresent()) return;
    const VehicleSystem::DamageResult result =
        g_game.vehicles.DamageSecondaryHelicopter(damage);
    if (!result.applied) return;
    scene.SpawnSmokeBurst(hit, 0.22f, 0.10f);
    if (!result.destroyed) return;
    scene.SpawnExplosionFX(g_secondaryHelicopterPosition, 7.0f, 1.0f);
    scene.SpawnSmokeBurst(g_secondaryHelicopterPosition, 1.25f, 1.5f);
}

static void DamageBoat(float damage, const XMFLOAT3& hit) {
    if (damage <= 0.0f || g_boatDead || !g_boatModel) return;
    const VehicleSystem::DamageResult result = g_game.vehicles.DamageBoat(damage);
    if (!result.applied) return;
    scene.SpawnSmokeBurst(hit, 0.22f, 0.10f);
    if (!result.destroyed) return;
    scene.SpawnExplosionFX(g_boatPosition, 6.0f, 1.0f);
    scene.SpawnSmokeBurst(g_boatPosition, 1.6f, 2.2f);
}

static bool HitSphereAtSegment(const XMFLOAT3& centerPosition,
                               float hitRadius,
                               const XMFLOAT3& start, const XMFLOAT3& end,
                               float radius, XMFLOAT3& hit) {
    const XMVECTOR a = XMLoadFloat3(&start);
    const XMVECTOR b = XMLoadFloat3(&end);
    const XMVECTOR center = XMLoadFloat3(&centerPosition);
    const XMVECTOR ab = b - a;
    const float lengthSq = XMVectorGetX(XMVector3LengthSq(ab));
    float t = lengthSq > 1e-6f
        ? XMVectorGetX(XMVector3Dot(center - a, ab)) / lengthSq : 0.0f;
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const XMVECTOR closest = a + ab * t;
    const float combinedRadius = hitRadius + radius;
    if (XMVectorGetX(XMVector3LengthSq(center - closest)) >
        combinedRadius * combinedRadius)
        return false;
    XMStoreFloat3(&hit, closest);
    return true;
}

static bool HitHelicopterAtSegment(const XMFLOAT3& position, bool dead,
                                   const XMFLOAT3& start, const XMFLOAT3& end,
                                   float radius, XMFLOAT3& hit) {
    if (g_emptyLevelMode || !scene.showHelicopter ||
        !g_helicopterModel || dead) return false;
    return HitSphereAtSegment(position, 5.0f, start, end, radius, hit);
}

static bool HitHelicopterSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                                 float radius, XMFLOAT3& hit) {
    return HitHelicopterAtSegment(g_helicopterPosition, g_helicopterDead,
                                  start, end, radius, hit);
}

static bool HitSecondaryHelicopterSegment(const XMFLOAT3& start,
                                          const XMFLOAT3& end, float radius,
                                          XMFLOAT3& hit) {
    return SecondaryHelicopterPresent() && HitHelicopterAtSegment(
        g_secondaryHelicopterPosition, g_secondaryHelicopterDead,
        start, end, radius, hit);
}

// The AA gun as a hit target. Sphere centred on the mount rather than the base
// plate, so shots at the gun itself connect and rounds into the dirt at its feet
// do not. Radius covers the pedestal and the traversing mount together.
// Reports which emplacement the segment struck, so damage lands on the turret
// that was actually shot rather than on whichever one happens to be first.
static bool HitAATurretSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                               float radius, XMFLOAT3& hit,
                               size_t& turretIndex) {
    const VehicleSystem& vehicles = g_game.vehicles;
    float bestDistanceSq = FLT_MAX;
    bool found = false;
    for (size_t i = 0; i < vehicles.aaTurrets.size(); ++i) {
        const VehicleSystem::AATurret& turret = vehicles.aaTurrets[i];
        if (!turret.Active()) continue;
        const XMFLOAT3 center{
            turret.position.x,
            turret.position.y + VehicleSystem::AATurretMountHeight * 0.6f,
            turret.position.z };
        XMFLOAT3 candidate;
        if (!HitSphereAtSegment(center, 2.0f, start, end, radius, candidate))
            continue;
        // Nearest to the segment start wins: a shot passing two emplacements
        // hits the one in front, not the one behind it.
        const float dx = candidate.x - start.x;
        const float dy = candidate.y - start.y;
        const float dz = candidate.z - start.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq >= bestDistanceSq) continue;
        bestDistanceSq = distanceSq;
        hit = candidate;
        turretIndex = i;
        found = true;
    }
    return found;
}

// Wrecks the emplacement: cooks off the ammo boxes and leaves it burning.
static void DamageAATurret(size_t turretIndex, float damage,
                           const XMFLOAT3& hit) {
    const VehicleSystem::DamageResult result =
        g_game.vehicles.DamageAATurret(turretIndex, damage);
    if (!result.applied) return;
    scene.SpawnSmokeBurst(hit, 0.24f, 0.12f);
    if (!result.destroyed) return;
    const XMFLOAT3 base = result.position;
    const XMFLOAT3 center{ base.x,
        base.y + VehicleSystem::AATurretMountHeight, base.z };
    scene.SpawnExplosionFX(center, 6.5f, 1.0f);
    scene.SpawnSmokeBurst(center, 2.2f, 2.4f);
    AddExplosionTerrainCrater(base);
    if (g_destruction.IsInitialized()) {
        g_destruction.ApplyExplosion(center, 6.0f, 45.0f, 10.0f);
        g_destruction.ApplyRagdollExplosion(center, 6.0f, 90.0f);
    }
    g_pendingExplosionAudio.push_back({ 0.0f, 0.95f, 0.80f, false });
    if (g_game.session.TimerRunning()) {
        g_game.mission.RecordDestruction();
        g_game.money.Award(MoneyEvent::PropDestroyed);
    }
    SGE_LOG("LogGameplay", EngineLog::Level::Display, "AA turret destroyed");
}

static bool HitOccupiedInsertionBlackHawkSegment(
        const XMFLOAT3& start, const XMFLOAT3& end,
        float radius, XMFLOAT3& hit) {
    const VehicleSystem& vehicles = g_game.vehicles;
    if (!g_blackHawkModel || !vehicles.blackHawkVisible ||
        !vehicles.blackHawkCarryingPlayer)
        return false;
    XMFLOAT3 center = vehicles.blackHawkPosition;
    center.y += 2.2f;
    return HitSphereAtSegment(center, 5.0f, start, end, radius, hit);
}

static bool HitBoatHullAtSegment(const XMFLOAT3& position, float yaw,
                                 const XMFLOAT3& start, const XMFLOAT3& end,
                                 float radius, XMFLOAT3& hit) {
    // Low, flat hull -- a single generous sphere at deck height would engulf
    // passengers and steal shots aimed over the gunwale.
    const XMVECTOR a = XMLoadFloat3(&start);
    const XMVECTOR b = XMLoadFloat3(&end);
    const XMVECTOR center = XMLoadFloat3(&position);
    const XMVECTOR ab = b - a;
    const float lengthSq = XMVectorGetX(XMVector3LengthSq(ab));
    float t = lengthSq > 1e-6f
        ? XMVectorGetX(XMVector3Dot(center - a, ab)) / lengthSq : 0.0f;
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const XMVECTOR closest = a + ab * t;
    const XMVECTOR offset = closest - center;
    const float sinYaw = std::sin(yaw), cosYaw = std::cos(yaw);
    XMFLOAT3 offsetF; XMStoreFloat3(&offsetF, offset);
    const float localX = offsetF.x * cosYaw - offsetF.z * sinYaw;
    const float localZ = offsetF.x * sinYaw + offsetF.z * cosYaw;
    const float localY = offsetF.y;
    constexpr float halfBeam = 1.5f, halfLength = 4.5f, hullHeight = 1.1f;
    if (std::fabs(localX) > halfBeam + radius ||
        std::fabs(localZ) > halfLength + radius ||
        localY < -hullHeight - radius || localY > radius)
        return false;
    XMStoreFloat3(&hit, closest);
    return true;
}

static bool HitBoatSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                           float radius, XMFLOAT3& hit) {
    if (!g_boatModel || g_boatDead) return false;
    return HitBoatHullAtSegment(
        g_boatPosition, g_boatYaw, start, end, radius, hit);
}

static bool HitOccupiedInsertionBoatSegment(
        const XMFLOAT3& start, const XMFLOAT3& end,
        float radius, XMFLOAT3& hit) {
    const VehicleSystem& vehicles = g_game.vehicles;
    if (!g_insertionBoatModel || !vehicles.insertionBoatVisible ||
        !vehicles.insertionBoatCarryingPlayer)
        return false;
    XMFLOAT3 position = vehicles.insertionBoatPosition;
    position.y -= vehicles.insertionBoatSinkOffset;
    return HitBoatHullAtSegment(
        position, vehicles.insertionBoatYaw, start, end, radius, hit);
}

static void PlayBanditDeathEvents();

static bool KillBanditsTouchingRotor(const std::shared_ptr<SceneNode>& rotor,
                                     FXMVECTOR localAxis, float rotorRadius,
                                     CXMMATRIX helicopterWorld, bool dead) {
    if (!rotor || dead || !g_banditLoaded) return false;
    const XMMATRIX rotorWorld = XMLoadFloat4x4(&rotor->globalTransform) *
                                helicopterWorld;
    const XMVECTOR center = XMVector3TransformCoord(XMVectorZero(), rotorWorld);
    const XMVECTOR axis = XMVector3Normalize(
        XMVector3TransformNormal(localAxis, rotorWorld));
    bool killed = false;
    for (auto& bandit : g_bandits) {
        if (!bandit || bandit->Dead()) continue;
        const XMVECTOR body = XMVectorSet(
            bandit->position.x,
            bandit->position.y + bandit->footOffset + 1.0f,
            bandit->position.z, 1.0f);
        const XMVECTOR delta = body - center;
        const float axial = XMVectorGetX(XMVector3Dot(delta, axis));
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(delta));
        const float radialSq = (std::max)(0.0f, distanceSq - axial * axial);
        constexpr float bodyRadius = 0.72f;
        if (std::abs(axial) > bodyRadius + 0.18f ||
            radialSq > (rotorRadius + bodyRadius) * (rotorRadius + bodyRadius))
            continue;

        XMVECTOR radial = delta - axis * axial;
        if (XMVectorGetX(XMVector3LengthSq(radial)) < 1e-5f)
            radial = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        radial = XMVector3Normalize(radial);
        XMVECTOR impulse = XMVector3Normalize(
            XMVector3Cross(axis, radial) + XMVectorSet(0.0f, 0.22f, 0.0f, 0.0f));
        XMFLOAT3 direction, impact;
        XMStoreFloat3(&direction, impulse);
        XMStoreFloat3(&impact, body);
        if (!bandit->KillFromRotor(direction, impact)) continue;
        if (bandit.get() == g_heldBandit) g_heldBandit = nullptr;
        scene.SpawnBloodBurst(impact, direction);
        g_hitAudio.Play(0.72f * 0.3f,
            0.88f + ((float)std::rand() / RAND_MAX) * 0.16f);
        killed = true;
    }
    return killed;
}

static void UpdateHelicopterRotorKills() {
    if (!scene.showHelicopter) return;
    bool killed = KillBanditsTouchingRotor(
        g_helicopterMainRotorNode, XMVectorSet(0, 1, 0, 0), 4.75f,
        HelicopterWorldMatrix(), g_helicopterDead);
    killed |= KillBanditsTouchingRotor(
        g_helicopterTailRotorNode, XMVectorSet(1, 0, 0, 0), 1.10f,
        HelicopterWorldMatrix(), g_helicopterDead);
    if (SecondaryHelicopterPresent()) {
        // This craft's own rotor nodes -- they sit at a different angle than the
        // patrol gunship's, so the swept arc must be read from the clone.
        const std::shared_ptr<SceneNode>& secondaryMain =
            g_secondaryHelicopterMainRotorNode ? g_secondaryHelicopterMainRotorNode
                                               : g_helicopterMainRotorNode;
        const std::shared_ptr<SceneNode>& secondaryTail =
            g_secondaryHelicopterTailRotorNode ? g_secondaryHelicopterTailRotorNode
                                               : g_helicopterTailRotorNode;
        killed |= KillBanditsTouchingRotor(
            secondaryMain, XMVectorSet(0, 1, 0, 0), 4.75f,
            SecondaryHelicopterWorldMatrix(), g_secondaryHelicopterDead);
        killed |= KillBanditsTouchingRotor(
            secondaryTail, XMVectorSet(1, 0, 0, 0), 1.10f,
            SecondaryHelicopterWorldMatrix(), g_secondaryHelicopterDead);
    }
    if (killed) PlayBanditDeathEvents();
}

// Battle-damage smoke on the enemy gunships, matching the insertion BlackHawk's
// trail in UpdateBlackHawkCrashEffects: intermittent wisps at the threshold
// building to a near-continuous plume near zero health, so an airframe about to
// go down reads the same whichever aircraft it is.
//
// A crashed wreck stops trailing (its engine is gone) but a dead-and-still-
// falling one keeps smoking all the way in, which is what sells the kill.
static void UpdateEnemyHelicopterDamageSmoke(float deltaTime) {
    if (!g_helicopterModel || !scene.showHelicopter) return;
    const VehicleSystem& vehicles = g_game.vehicles;

    const auto trail = [&](float severity, bool crashed, float& timer,
                           const XMFLOAT3& position, float yaw) {
        if (severity <= 0.0f || crashed) { timer = 0.0f; return; }
        timer -= deltaTime;
        if (timer > 0.0f) return;
        timer = 0.34f - 0.29f * severity;
        const float radius = 0.35f + 0.95f * severity;
        const float intensity = 0.35f + 1.15f * severity;
        // Off the engine deck, just aft of the model origin.
        const float backX = -std::sin(yaw) * 0.6f;
        const float backZ = -std::cos(yaw) * 0.6f;
        scene.SpawnSmokeBurst({ position.x + backX, position.y + 1.1f,
                                position.z + backZ }, radius, intensity);
    };

    static float primaryTimer = 0.0f;
    trail(vehicles.HelicopterDamageSeverity(), g_helicopterCrashed,
          primaryTimer, g_helicopterPosition, g_helicopterYaw);

    // Any time the second airframe is in play -- stress-test patrol, an inbound
    // reinforcement wave, or a crash still falling -- a damaged one trails smoke.
    if (SecondaryHelicopterPresent()) {
        static float secondaryTimer = 0.0f;
        trail(vehicles.SecondaryHelicopterDamageSeverity(),
              g_secondaryHelicopterCrashed, secondaryTimer,
              g_secondaryHelicopterPosition, g_secondaryHelicopterYaw);
    }
}

static void UpdateHelicopter(float dt) {
    if (!g_helicopterModel || !scene.showHelicopter) return;
    const bool rotorPowered = !g_helicopterDead ||
        (SecondaryHelicopterPresent() && !g_secondaryHelicopterDead);
    g_helicopterRotorSpeedScale = VehicleSystem::StepHelicopterRotorSpeed(
        g_helicopterRotorSpeedScale, rotorPowered, dt);
    g_helicopterMainRotorAngle = std::fmod(
        g_helicopterMainRotorAngle +
            dt * 24.0f * g_helicopterRotorSpeedScale, XM_2PI);
    g_helicopterTailRotorAngle = std::fmod(
        g_helicopterTailRotorAngle +
            dt * 38.0f * g_helicopterRotorSpeedScale, XM_2PI);
    if (g_helicopterMainRotorNode)
        XMStoreFloat4(&g_helicopterMainRotorNode->rotation,
            XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0),
                                     g_helicopterMainRotorAngle));
    if (g_helicopterTailRotorNode)
        XMStoreFloat4(&g_helicopterTailRotorNode->rotation,
            XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0),
                                     g_helicopterTailRotorAngle));
    if (g_helicopterDead) {
        if (g_helicopterCrashed) {
            // Flush the hierarchy before leaving. The rotor rotations above are
            // written unconditionally, so returning straight out left those
            // node locals newer than the global transforms built from them.
            // The wreck's rotors are stopped, which is why this never produced
            // a visible artefact, but the two were drifting apart every frame
            // and anything that later read the globals would have got a pose
            // the locals no longer described.
            XMFLOAT4X4 restIdentity;
            XMStoreFloat4x4(&restIdentity, XMMatrixIdentity());
            g_helicopterModel->UpdateGlobalTransform(restIdentity);
            return;
        }
        g_helicopterCrashVelocity.y -= 9.81f * dt;
        g_helicopterPosition.x += g_helicopterCrashVelocity.x * dt;
        g_helicopterPosition.y += g_helicopterCrashVelocity.y * dt;
        g_helicopterPosition.z += g_helicopterCrashVelocity.z * dt;
        g_helicopterPitch += 0.42f * dt;
        g_helicopterRoll += 0.78f * dt;
        g_helicopterYaw += 0.18f * dt;

        float groundY = 0.0f;
        if (scene.useMeshTerrain && g_terrain.supported) {
            auto params = CurrentTerrainParams();
            params.heightScale = scene.terrainHeightScale;
            groundY = TerrainRendererDX12::HeightAt(
                params, g_helicopterPosition.x, g_helicopterPosition.z);
        }
        if (g_helicopterPosition.y <= groundY + 1.65f) {
            g_helicopterPosition.y = groundY + 1.65f;
            g_helicopterCrashed = true;
            g_helicopterCrashVelocity = { 0.0f, 0.0f, 0.0f };
            scene.SpawnExplosionFX(
                { g_helicopterPosition.x, g_helicopterPosition.y + 1.6f,
                  g_helicopterPosition.z }, 9.0f, 1.1f);
            scene.SpawnSmokeBurst(g_helicopterPosition, 2.8f, 3.0f);
            if (g_destruction.IsInitialized())
                g_destruction.ApplyExplosion(g_helicopterPosition, 4.5f, 55.0f, 12.0f);
        }
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        g_helicopterModel->UpdateGlobalTransform(identity);
        return;
    }
    g_helicopterHoverTime += dt;

    // Slow bounded patrol around spawn, with small independent hover drift.
    // Horizontal distance never exceeds the configured patrol radius.
    const float patrolPhase = g_helicopterHoverTime * 0.105f;
    g_helicopterPosition.x =
        g_helicopterSpawn.x +
        std::sin(patrolPhase) * (kHelicopterPatrolRadius - 0.72f) +
        std::sin(g_helicopterHoverTime * 0.31f) * 0.72f;
    g_helicopterPosition.y = g_helicopterSpawn.y +
        std::sin(g_helicopterHoverTime * 1.27f) * 0.26f +
        std::sin(g_helicopterHoverTime * 0.43f) * 0.12f;
    g_helicopterPosition.z =
        g_helicopterSpawn.z +
        std::cos(patrolPhase) * (kHelicopterPatrolRadius * 0.68f) +
        std::cos(g_helicopterHoverTime * 0.27f) * 0.55f;

    const float targetX = scene.camera.Position.x - g_helicopterPosition.x;
    const float targetZ = scene.camera.Position.z - g_helicopterPosition.z;
    const float desiredYaw = std::atan2(targetX, targetZ);
    const float yawDelta = std::atan2(
        std::sin(desiredYaw - g_helicopterYaw),
        std::cos(desiredYaw - g_helicopterYaw));
    const float yawLerp = 1.0f - std::exp(-1.65f * (std::max)(0.0f, dt));
    g_helicopterYaw += yawDelta * yawLerp;
    const float desiredRoll = (std::max)(-0.10f, (std::min)(0.10f,
        -yawDelta * 0.075f + std::sin(g_helicopterHoverTime * 0.71f) * 0.025f));
    const float desiredPitch = std::sin(g_helicopterHoverTime * 0.47f) * 0.022f;
    const float attitudeLerp = 1.0f - std::exp(-2.4f * (std::max)(0.0f, dt));
    g_helicopterRoll += (desiredRoll - g_helicopterRoll) * attitudeLerp;
    g_helicopterPitch += (desiredPitch - g_helicopterPitch) * attitudeLerp;

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    g_helicopterModel->UpdateGlobalTransform(identity);

    // The airframe keeps flying on the deployment screen -- it is part of the
    // map read -- but it must not engage. The player body is parked at the
    // insertion point while the zone is being chosen and cannot fight back,
    // so acquiring it there is a free kill before the run has started.
    if (DeploymentPlanningActive()) {
        g_helicopterFireCooldown = 0.0f;
        return;
    }

    g_helicopterFireCycleTime = std::fmod(
        g_helicopterFireCycleTime + dt, 7.0f);
    if (g_helicopterFireCycleTime >= 2.0f) {
        g_helicopterFireCooldown = 0.0f;
        return;
    }
    g_helicopterFireCooldown -= dt;
    if (scene.player.health <= 0.0f || g_helicopterFireCooldown > 0.0f) return;
    const XMFLOAT3 forward{
        std::sin(g_helicopterYaw), 0.0f, std::cos(g_helicopterYaw) };
    const XMFLOAT3 muzzle{
        g_helicopterPosition.x + forward.x * 3.75f,
        g_helicopterPosition.y - 0.65f,
        g_helicopterPosition.z + forward.z * 3.75f };
    XMVECTOR direction = XMLoadFloat3(&scene.camera.Position) - XMLoadFloat3(&muzzle);
    const float distanceSq = XMVectorGetX(XMVector3LengthSq(direction));
    if (distanceSq < 4.0f ||
        distanceSq > kHelicopterEngagementRange * kHelicopterEngagementRange) {
        g_helicopterFireCooldown = 0.10f;
        return;
    }
    // Lead the player rather than firing at where they stand. The door gun
    // engages out to kHelicopterEngagementRange, far enough that flight time is
    // what let a running player walk out from under a burst untouched.
    const XMFLOAT3 helicopterAim = PrimaryHelicopterWeaponAimPoint();
    direction = XMLoadFloat3(&helicopterAim) - XMLoadFloat3(&muzzle);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-5f) return;
    const float randomX = ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f;
    const float randomY = ((float)std::rand() / RAND_MAX - 0.5f) * 0.012f;
    const float randomZ = ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f;
    direction = XMVector3Normalize(direction) + XMVectorSet(randomX, randomY, randomZ, 0.0f);
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));
    scene.SpawnHostileProjectile(muzzle, shotDirection);
    scene.SpawnWeaponSmoke(muzzle, shotDirection, 0.8f);
    g_gunAudio.PlayAt(muzzle.x, muzzle.y, muzzle.z, 0.68f,
                      0.82f + ((float)std::rand() / RAND_MAX) * 0.08f, 120.0f);
    g_helicopterFireCooldown = 0.10f;
}

// Both defined with the other spawn helpers, below.
static float RandomUnit();
static bool SpawnDropshipBandit(const XMFLOAT3& position);
// Defined with the aircraft objective, below. Needed up here by the burning-
// material tick, which must not set fire to a player-only objective.
static bool IsObjectivePlaneEntity(uint64_t entityId);
static bool PrefabSurfaceSupports(const DirectX::XMFLOAT3& position,
                                  float feetY, float maxDrop, float& surfaceY);

// A spawn point authored above a prop is placed by eye, so the actor can start
// well clear of the surface it is meant to stand on -- further than the
// steady-state probe reaches. Search this far down once at spawn to settle it
// onto the deck instead of dropping it to terrain on the first update.
static constexpr float kBanditPrefabSpawnDrop = 40.0f;

// Ledge test for an actor already standing on a prop. Probes deeper than the
// support drop so a real ledge reports the height it would fall to rather than
// simply finding nothing, which lets a stair tread be told from a sheer edge.
static constexpr float kBanditLedgeProbeDrop = 6.0f;

// Largest height difference an actor will step down on a prop. Above this the
// move is cancelled and it holds the ledge. Matches the player's step height,
// so a bandit negotiates the same stairs the player can.
static constexpr float kBanditMaxStepDown = 0.45f;
// Defined with the lightning state, below.
static void ResetLightning();

static void UpdateEnemyVisionForCurrentConditions() {
    TimeOfDaySettings applied = MakeTimeOfDaySettings(g_selectedTimeOfDay);
    applied.enableVolumetricFog = scene.enableVolumetricFog;
    applied.volumetricFogDensity = scene.volumetricFogDensity;
    applied.volumetricFogDistance = scene.volumetricFogDistance;
    g_enemyVisionScale = TimeOfDayVisibilityFactor(applied);
}

// Weather is an authored group rather than a rain toggle: every preset moves
// precipitation, wind, both cloud layers and fog together. The fog copy is
// kept with the selected time so switching the sun away and back does not
// silently replace the chosen weather with that time's previous fog override.
void ApplyLiveWeatherState(WeatherState state) {
    // Arm lightning on the transition into Storm, not on every apply.
    // ApplyTimeOfDay re-applies the current weather whenever the sun moves, so
    // resetting unconditionally would push the next strike back to the start of
    // its cooldown each time the player changed the time of day.
    const bool enteringStorm = state == WeatherState::Storm &&
                               scene.weatherState != WeatherState::Storm;
    scene.ApplyWeatherPreset(state);
    if (enteringStorm) ResetLightning();
    // Night rain/storm keeps night's near-black fog instead of the preset's
    // daylight blue-grey, which would otherwise light the ground layer with a
    // sky that is not there. Runs after the preset, before the per-time copy
    // below, so the corrected values are what get stored and shown in the UI.
    if (state != WeatherState::Custom &&
        g_selectedTimeOfDay == TimeOfDay::Night) {
        WeatherSettings nightWeather = MakeWeatherSettings(state);
        ApplyNightWeatherFog(nightWeather, state);
        scene.volumetricFogTint = nightWeather.fogTint;
        scene.volumetricFogHeightFalloff = nightWeather.fogHeightFalloff;
        scene.volumetricFogAnisotropy = nightWeather.fogAnisotropy;
    }
    if (state != WeatherState::Custom) {
        VolumetricFogSettings& fog =
            VolumetricFogFor(g_selectedTimeOfDay);
        fog = {scene.enableVolumetricFog,
               scene.volumetricFogDensity,
               scene.volumetricFogAnisotropy,
               scene.volumetricFogHeightFalloff,
               scene.volumetricFogBaseHeight,
               scene.volumetricFogDistance,
               scene.volumetricFogTint};
    }
    UpdateEnemyVisionForCurrentConditions();
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        std::string("Weather set to ") + WeatherStateName(state) +
        " (enemy sight x" + std::to_string(g_enemyVisionScale) + ")");
}

// Pushes a time-of-day preset onto the live scene. Every field moves together:
// the sky, volumetric fog and DDGI all read scene.lightPos as the sun
// direction, so changing the key light without the atmosphere alongside it
// gives a midnight sun over a blue afternoon sky.
static void ApplyTimeOfDay(TimeOfDay time) {
    TimeOfDaySettings settings = MakeTimeOfDaySettings(time);
    scene.lightPos = settings.lightPos;
    scene.lightColor = settings.lightColor;
    scene.directionalLightIntensity = settings.directionalLightIntensity;
    scene.ambientStrength = settings.ambientStrength;
    scene.ambientLightingIntensity = settings.ambientLightingIntensity;
    scene.clearColor = settings.clearColor;
    scene.atmosphereRayleighStrength = settings.atmosphereRayleighStrength;
    scene.atmosphereMieStrength = settings.atmosphereMieStrength;
    scene.atmosphereAerialDensity = settings.atmosphereAerialDensity;
    // Fog comes from the per-time override rather than the preset, so values
    // tuned on the deployment screen survive DEPLOY and any later re-apply.
    ApplyVolumetricFogSettings(VolumetricFogFor(time));
    // Weather remains authoritative over its fog component when the sun moves.
    // Custom retains the per-time fog that was just restored above.
    ApplyLiveWeatherState(scene.weatherState);
    // The demo light animation walks lightPos around on its own, which would
    // drag a chosen sun back out of place within a few seconds.
    scene.animateDemoLights = false;

    // Night swaps the environment map too. Only requested here: replacing a
    // texture still sampled by in-flight frames has to run between frames rather
    // than from the UI callback that picked the preset.
    RequestTimeOfDaySkyEnvironment(time);
    // Indirect light is cached across frames, so a sun that moves this far in
    // one frame leaves the old bounce baked in until it converges out.
    g_game.commands.Request(GameCommand::ResetDDGIHistory);
    g_game.commands.Request(GameCommand::RebuildDDGI);

    // Sight range follows the light and the fog. Computed from the settings
    // actually applied -- fog comes from the player's per-time override, not
    // the preset -- so tuning the fog on the deployment screen moves enemy
    // perception with it rather than leaving the two disagreeing.
    UpdateEnemyVisionForCurrentConditions();

    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        std::string("Time of day set to ") + TimeOfDayName(time) +
        " (enemy sight x" + std::to_string(g_enemyVisionScale) + ")");
}

// Terrain height under a world position, or 0 on levels without mesh terrain.
static float GroundHeightAt(float x, float z) {
    if (!scene.useMeshTerrain || !g_terrain.supported) return 0.0f;
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    return TerrainRendererDX12::HeightAt(params, x, z);
}

// Squad size per wave, escalating: the first call-in is a probe, later ones
// commit. Capped so a long run cannot bury the player in bodies.
static int DropshipWaveTroopCount(uint32_t waveIndex) {
    constexpr int kBase = 3;
    constexpr int kMax = 6;
    return (std::min)(kMax, kBase + static_cast<int>(waveIndex));
}

// Calls in a reinforcement wave on the gunship. The drop point is offset from
// the player so the squad lands nearby but not on top of them, and the entry
// point is pushed far out along that same bearing so the craft flies in over
// open ground rather than materialising at the hover.
static void CallInReinforcementWave() {
    VehicleSystem& vehicles = g_game.vehicles;
    if (!g_helicopterModel || !scene.showHelicopter) return;
    if (!vehicles.DropshipAvailable()) return;

    // Bearing chosen per wave so successive drops do not stack on one side.
    const float bearing = RandomUnit() * XM_2PI;
    constexpr float kDropDistance = 34.0f;
    constexpr float kEntryDistance = 210.0f;

    const XMFLOAT3 player = scene.camera.Position;
    float dropX = player.x + std::sin(bearing) * kDropDistance;
    float dropZ = player.z + std::cos(bearing) * kDropDistance;
    float laneX = std::sin(bearing);
    float laneZ = std::cos(bearing);

    // Once the exfil is on the water, reinforcements are dropped between the
    // player and the boat instead of on a random bearing: the garrison's job at
    // that point is to stand between the player and the way off the island, and
    // a squad landing behind them is not doing it.
    //
    // They cannot be put AT the boat -- it floats 42 m offshore and troops
    // rappel onto terrain (see SpawnDropshipBandit), so a drop over open water
    // would rope a squad into the sea. Instead, walk inward from the boat along
    // its own bearing and take the first point that is genuinely dry land. That
    // lands the blocking force on the beach the player has to cross.
    if (vehicles.EscapeBoatReady()) {
        const XMFLOAT3& boat = vehicles.escapeBoatPosition;
        const float toBoatX = boat.x - player.x;
        const float toBoatZ = boat.z - player.z;
        const float toBoatLength =
            std::sqrt(toBoatX * toBoatX + toBoatZ * toBoatZ);
        if (toBoatLength > 0.001f) {
            laneX = toBoatX / toBoatLength;
            laneZ = toBoatZ / toBoatLength;

            // Step in from the boat until the ground clears the waterline. The
            // island falloff works in per-axis normalised space, so a fixed
            // world offset would miss the beach on a stretched island -- this
            // samples the real height instead of assuming a shore radius.
            constexpr float kMinBeachHeight = 0.6f;
            constexpr float kSearchStep = 4.0f;
            constexpr int kMaxSearchSteps = 14;
            // Never land them on top of the player, however far in dry land
            // turns out to start.
            constexpr float kMinPlayerClearance = 18.0f;

            bool foundShore = false;
            for (int step = 0; step <= kMaxSearchSteps; ++step) {
                const float distanceFromBoat =
                    static_cast<float>(step) * kSearchStep;
                if (distanceFromBoat >= toBoatLength - kMinPlayerClearance)
                    break;
                const float candidateX = boat.x - laneX * distanceFromBoat;
                const float candidateZ = boat.z - laneZ * distanceFromBoat;
                if (GroundHeightAt(candidateX, candidateZ) < kMinBeachHeight)
                    continue;
                dropX = candidateX;
                dropZ = candidateZ;
                foundShore = true;
                break;
            }
            // No dry ground between the two (the player is already at the
            // water's edge, or swimming). Fall back to the standard offset
            // along the boat lane so the wave still arrives from that side.
            if (!foundShore) {
                dropX = player.x + laneX * kDropDistance;
                dropZ = player.z + laneZ * kDropDistance;
            }
        }
    }

    const XMFLOAT3 drop{ dropX, GroundHeightAt(dropX, dropZ), dropZ };

    // Fly in from beyond the drop, along the same lane, so the craft crosses the
    // water the player is heading for rather than appearing inland behind them.
    const float entryX = dropX + laneX * kEntryDistance;
    const float entryZ = dropZ + laneZ * kEntryDistance;
    const XMFLOAT3 entry{ entryX,
                          GroundHeightAt(entryX, entryZ) +
                              VehicleSystem::DropshipHoverHeight + 14.0f,
                          entryZ };

    const int troops = DropshipWaveTroopCount(vehicles.dropshipWavesCalled);
    vehicles.BeginDropshipRun(entry, drop, troops);
    // No exfil is placed here. The boat used to ride in with the first wave,
    // which put the way out on the water while the aircraft -- the thing the
    // run is actually about -- was still on the ground. It now appears only
    // once that aircraft is resolved (see OnObjectivePlaneResolved), so a wave
    // called by a falling comm tower escalates the fight without also handing
    // the player the exit.
    //
    // Waves called after the aircraft goes down still see an active boat above
    // and drop their squad on the shore in front of it.
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Reinforcement wave " + std::to_string(vehicles.dropshipWavesCalled) +
        " inbound with " + std::to_string(troops) + " troops");
}

// Resolving the aircraft -- shot down, or gone -- brings the ride home and the
// garrison's answer to it, both onto the insertion ring the player deployed
// from. This is the ONLY thing that puts an exfil on the water on a level that
// authored an aircraft: until the run's objective has an outcome, there is no
// way off the island.
//
// The ring, not a free bearing: those points are already known to be reachable
// coast (the planner samples terrain height to build them), and landing the
// response where the player came ashore is the readable version of "they know
// where you came in".
//
// Rolled once per call and shared by both, so the wave lands on the same stretch
// of coast the boat is waiting off -- the garrison stands between the player and
// the way out rather than somewhere unrelated.
//
// Called for an escaped aircraft as well as a downed one. A failed intercept is
// a worse run, not an unfinishable one, and the boat is the only way to end a
// mission -- without this an escape would strand the player on the island with
// nothing left to shoot and no exit. Idempotent through
// PlaceEscapeBoatOnBearing, so a second aircraft resolving later leaves the
// first one's boat where it is.
static void OnObjectivePlaneResolved() {
    VehicleSystem& vehicles = g_game.vehicles;
    // Nothing to do once the exfil is out there: the boat would not move
    // anyway, and re-running this would call in a wave per aircraft.
    if (vehicles.EscapeBoatReady()) return;
    // The live ring when a normal run has one. An editor playtest skips
    // deployment planning entirely (CancelDeploymentPlanning clears the zones),
    // so rebuild the same ring geometry here rather than degrading to a free
    // bearing -- a playtest is exactly where this gets checked, and it should
    // behave like the real thing.
    std::vector<XMFLOAT3> rebuilt;
    const std::vector<XMFLOAT3>* zones = &DeploymentZonePositions();
    if (zones->empty()) {
        auto params = CurrentTerrainParams();
        params.heightScale = scene.terrainHeightScale;
        const float deploymentRadius = CurrentDeploymentRadius();
        rebuilt = DeploymentPlanner::BuildPerimeterZones(
            deploymentRadius, deploymentRadius, 20,
            [&](float x, float z) {
                return (std::max)(0.0f,
                    TerrainRendererDX12::HeightAt(params, x, z));
            });
        zones = &rebuilt;
    }
    // Nothing to place against at all: still owe the run an exfil, so fall back
    // to the free bearing the rest of the game uses.
    if (zones->empty()) {
        vehicles.PlaceEscapeBoatOnBearing(RandomUnit() * XM_2PI, 0.0f,
                                          CurrentEscapeBoatDistance());
        CallInReinforcementWave();
        return;
    }

    const size_t pick = static_cast<size_t>(
        RandomUnit() * static_cast<float>(zones->size())) % zones->size();
    const XMFLOAT3& zone = (*zones)[pick];
    // The ring point's own compass bearing, measured the way the boat placer
    // expects (+Z = 0, turning through +X). The boat sits on that bearing but
    // further out than the ring point itself, so the exfil is a leg beyond the
    // coast the player inserted onto rather than a return to it -- and it stays
    // in deep water however wide the deployment ring was authored.
    const float bearing = std::atan2(zone.x, zone.z);
    vehicles.PlaceEscapeBoatOnBearing(bearing, 0.0f,
                                      CurrentEscapeBoatDistance());

    // Called after the boat is placed, so the wave's own shore-search sees an
    // active exfil and drops the squad on the beach in front of it -- the
    // blocking position -- instead of on a fresh random bearing.
    CallInReinforcementWave();

    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Aircraft resolved -- exfil and reinforcements on insertion zone " +
        std::to_string(pick));
}

// Flies the reinforcement dropship and spawns whatever it releases this frame.
// Runs outside the stress-test gate that guards the patrol behaviour below,
// because a called-in wave is real gameplay, not a stress scenario.
static void UpdateReinforcementDropship(float dt) {
    VehicleSystem& vehicles = g_game.vehicles;
    // Ropes are rebuilt from scratch every frame, including the frame the last
    // troop lands -- otherwise the final rope would hang in the air.
    g_dropshipRopeItems.clear();
    if (!vehicles.DropshipActive()) return;
    if (!g_helicopterModel || !scene.showHelicopter) {
        vehicles.ResetDropship();
        return;
    }

    const float groundY = GroundHeightAt(vehicles.dropshipDropPoint.x,
                                         vehicles.dropshipDropPoint.z);
    const int released = vehicles.UpdateDropship(dt, groundY);

    // A rope from the airframe down to every troop still on the way. Built
    // before the new releases below so a troop spawned this frame gets its rope
    // on the next one, once it has actually started descending.
    //
    // Each rope hangs vertically from the troop's own release offset rather
    // than from one shared point on the airframe: the squad fans out around the
    // craft, and converging every rope on a single hardpoint would splay them
    // through each other. Only the release height comes from the aircraft.
    const float ropeTopY = vehicles.DropshipTroopReleasePoint().y;
    for (const auto& bandit : g_bandits) {
        if (!bandit || !bandit->Rappelling()) continue;
        const XMFLOAT3 hands{ bandit->position.x,
                              bandit->position.y + bandit->footOffset + 1.35f,
                              bandit->position.z };
        AppendDropshipRope({ hands.x, ropeTopY, hands.z }, hands);
    }

    if (released <= 0) return;

    for (int i = 0; i < released; ++i) {
        XMFLOAT3 spawn = vehicles.DropshipTroopReleasePoint();
        // Fan the squad out around the rope so they do not descend inside one
        // another. The offset is horizontal only: they leave the craft at its
        // altitude and rope down from there.
        const float angle = RandomUnit() * XM_2PI;
        const float spread = 0.8f + RandomUnit() * 1.4f;
        spawn.x += std::sin(angle) * spread;
        spawn.z += std::cos(angle) * spread;
        SpawnDropshipBandit(spawn);
    }
}

static void UpdateSecondaryHelicopter(float dt) {
    if (!g_helicopterModel || !scene.showHelicopter) return;

    // Spin this airframe's own blades. Runs before the DropshipActive bail-out
    // below, because the dropship steers the craft's POSITION but nothing else
    // turns its rotors -- and on its own clone, so a dead patrol gunship no
    // longer stops the reinforcement helicopter's blades along with its own.
    if (SecondaryHelicopterPresent()) {
        g_secondaryHelicopterRotorSpeedScale =
            VehicleSystem::StepHelicopterRotorSpeed(
                g_secondaryHelicopterRotorSpeedScale,
                !g_secondaryHelicopterDead, dt);
        g_secondaryHelicopterMainRotorAngle = std::fmod(
            g_secondaryHelicopterMainRotorAngle +
                dt * 24.0f * g_secondaryHelicopterRotorSpeedScale, XM_2PI);
        g_secondaryHelicopterTailRotorAngle = std::fmod(
            g_secondaryHelicopterTailRotorAngle +
                dt * 38.0f * g_secondaryHelicopterRotorSpeedScale, XM_2PI);
        if (g_secondaryHelicopterMainRotorNode)
            XMStoreFloat4(&g_secondaryHelicopterMainRotorNode->rotation,
                XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0),
                                         g_secondaryHelicopterMainRotorAngle));
        if (g_secondaryHelicopterTailRotorNode)
            XMStoreFloat4(&g_secondaryHelicopterTailRotorNode->rotation,
                XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0),
                                         g_secondaryHelicopterTailRotorAngle));
        if (g_secondaryHelicopterModel) {
            XMFLOAT4X4 identity;
            XMStoreFloat4x4(&identity, XMMatrixIdentity());
            g_secondaryHelicopterModel->UpdateGlobalTransform(identity);
        }
    }

    // The dropship owns the airframe while a wave is in the air, so the patrol
    // path must not fight it for the same position fields.
    if (g_game.vehicles.DropshipActive()) return;
    // A downed airframe falls wherever it was shot down, stress test or not.
    // UpdateDropship drops straight to Idle the moment the craft dies, so
    // outside the stress test this crash is the ONLY thing still simulating it
    // -- gating the whole function on g_stressTestMode (as it once was) made a
    // shot-down reinforcement helicopter freeze mid-air and then vanish.
    if (g_secondaryHelicopterDead) {
        if (g_secondaryHelicopterCrashed) return;
        g_secondaryHelicopterCrashVelocity.y -= 9.81f * dt;
        g_secondaryHelicopterPosition.x += g_secondaryHelicopterCrashVelocity.x * dt;
        g_secondaryHelicopterPosition.y += g_secondaryHelicopterCrashVelocity.y * dt;
        g_secondaryHelicopterPosition.z += g_secondaryHelicopterCrashVelocity.z * dt;
        g_secondaryHelicopterPitch += 0.42f * dt;
        g_secondaryHelicopterRoll += 0.78f * dt;
        g_secondaryHelicopterYaw += 0.18f * dt;

        float groundY = 0.0f;
        if (scene.useMeshTerrain && g_terrain.supported) {
            auto params = CurrentTerrainParams();
            params.heightScale = scene.terrainHeightScale;
            groundY = TerrainRendererDX12::HeightAt(
                params, g_secondaryHelicopterPosition.x,
                g_secondaryHelicopterPosition.z);
        }
        if (g_secondaryHelicopterPosition.y <= groundY + 1.65f) {
            g_secondaryHelicopterPosition.y = groundY + 1.65f;
            g_secondaryHelicopterCrashed = true;
            g_secondaryHelicopterCrashVelocity = { 0.0f, 0.0f, 0.0f };
            scene.SpawnExplosionFX(
                { g_secondaryHelicopterPosition.x,
                  g_secondaryHelicopterPosition.y + 1.6f,
                  g_secondaryHelicopterPosition.z }, 9.0f, 1.1f);
            scene.SpawnSmokeBurst(g_secondaryHelicopterPosition, 2.8f, 3.0f);
            if (g_destruction.IsInitialized())
                g_destruction.ApplyExplosion(
                    g_secondaryHelicopterPosition, 4.5f, 55.0f, 12.0f);
        }
        return;
    }

    // Only the stress test flies a second live patrol. Otherwise this airframe
    // exists solely as the reinforcement dropship, which UpdateDropship steers.
    if (!g_stressTestMode) return;

    g_secondaryHelicopterHoverTime += dt;
    const float patrolPhase = g_secondaryHelicopterHoverTime * 0.105f + XM_PI;
    g_secondaryHelicopterPosition.x = 42.0f +
        std::sin(patrolPhase) * (kHelicopterPatrolRadius - 0.72f) +
        std::sin(g_secondaryHelicopterHoverTime * 0.29f) * 0.72f;
    g_secondaryHelicopterPosition.y = 14.0f +
        std::sin(g_secondaryHelicopterHoverTime * 1.19f) * 0.26f +
        std::sin(g_secondaryHelicopterHoverTime * 0.39f) * 0.12f;
    g_secondaryHelicopterPosition.z =
        std::cos(patrolPhase) * (kHelicopterPatrolRadius * 0.68f) +
        std::cos(g_secondaryHelicopterHoverTime * 0.25f) * 0.55f;

    const float targetX = scene.camera.Position.x - g_secondaryHelicopterPosition.x;
    const float targetZ = scene.camera.Position.z - g_secondaryHelicopterPosition.z;
    const float desiredYaw = std::atan2(targetX, targetZ);
    const float yawDelta = std::atan2(
        std::sin(desiredYaw - g_secondaryHelicopterYaw),
        std::cos(desiredYaw - g_secondaryHelicopterYaw));
    const float yawLerp = 1.0f - std::exp(-1.65f * (std::max)(0.0f, dt));
    g_secondaryHelicopterYaw += yawDelta * yawLerp;
    const float desiredRoll = (std::max)(-0.10f, (std::min)(0.10f,
        -yawDelta * 0.075f +
        std::sin(g_secondaryHelicopterHoverTime * 0.67f) * 0.025f));
    const float desiredPitch =
        std::sin(g_secondaryHelicopterHoverTime * 0.43f) * 0.022f;
    const float attitudeLerp = 1.0f - std::exp(-2.4f * (std::max)(0.0f, dt));
    g_secondaryHelicopterRoll +=
        (desiredRoll - g_secondaryHelicopterRoll) * attitudeLerp;
    g_secondaryHelicopterPitch +=
        (desiredPitch - g_secondaryHelicopterPitch) * attitudeLerp;

    // Held to the same rule as the primary airframe: flies, does not engage.
    if (DeploymentPlanningActive()) {
        g_secondaryHelicopterFireCooldown = 0.0f;
        return;
    }

    g_secondaryHelicopterFireCycleTime = std::fmod(
        g_secondaryHelicopterFireCycleTime + dt, 7.0f);
    if (g_secondaryHelicopterFireCycleTime >= 2.0f) {
        g_secondaryHelicopterFireCooldown = 0.0f;
        return;
    }
    g_secondaryHelicopterFireCooldown -= dt;
    if (scene.player.health <= 0.0f ||
        g_secondaryHelicopterFireCooldown > 0.0f)
        return;
    const XMFLOAT3 forward{
        std::sin(g_secondaryHelicopterYaw), 0.0f,
        std::cos(g_secondaryHelicopterYaw) };
    const XMFLOAT3 muzzle{
        g_secondaryHelicopterPosition.x + forward.x * 3.75f,
        g_secondaryHelicopterPosition.y - 0.65f,
        g_secondaryHelicopterPosition.z + forward.z * 3.75f };
    XMVECTOR direction =
        XMLoadFloat3(&scene.camera.Position) - XMLoadFloat3(&muzzle);
    const float distanceSq = XMVectorGetX(XMVector3LengthSq(direction));
    if (distanceSq < 4.0f ||
        distanceSq > kHelicopterEngagementRange * kHelicopterEngagementRange) {
        g_secondaryHelicopterFireCooldown = 0.10f;
        return;
    }
    // Same lead as the primary door gun above.
    const XMFLOAT3 secondaryAim = SecondaryHelicopterWeaponAimPoint();
    direction = XMLoadFloat3(&secondaryAim) - XMLoadFloat3(&muzzle);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-5f) return;
    direction = XMVector3Normalize(direction) + XMVectorSet(
        ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f,
        ((float)std::rand() / RAND_MAX - 0.5f) * 0.012f,
        ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f, 0.0f);
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));
    scene.SpawnHostileProjectile(muzzle, shotDirection);
    scene.SpawnWeaponSmoke(muzzle, shotDirection, 0.8f);
    g_gunAudio.PlayAt(muzzle.x, muzzle.y, muzzle.z, 0.68f,
                      0.82f + ((float)std::rand() / RAND_MAX) * 0.08f, 120.0f);
    g_secondaryHelicopterFireCooldown = 0.10f;
}

struct BoatPlatformPose {
    XMFLOAT3 position = {};
    float yaw = 0.0f;
    float sinkDepth = 0.0f;
};

static BoatPlatformPose CurrentBoatPlatformPose() {
    return { g_boatPosition, g_boatYaw, g_boatSinkDepth };
}

static float BoatDeckY(const BoatPlatformPose& pose) {
    return pose.position.y - pose.sinkDepth + kBoatDeckOffset;
}

static bool BoatDeckSupports(const XMFLOAT3& position, float feetY,
                             const BoatPlatformPose& pose, float padding,
                             float verticalTolerance = 0.35f) {
    const float dx = position.x - pose.position.x;
    const float dz = position.z - pose.position.z;
    const float sinYaw = std::sin(pose.yaw);
    const float cosYaw = std::cos(pose.yaw);
    const float localX = dx * cosYaw - dz * sinYaw;
    const float localZ = dx * sinYaw + dz * cosYaw;
    return std::abs(localX) <= kBoatDeckHalfBeam + padding &&
           std::abs(localZ) <= kBoatDeckHalfLength + padding &&
           std::abs(feetY - BoatDeckY(pose)) <= verticalTolerance;
}

static void TransformWithBoat(XMFLOAT3& position,
                              const BoatPlatformPose& oldPose,
                              const BoatPlatformPose& newPose) {
    const float dx = position.x - oldPose.position.x;
    const float dz = position.z - oldPose.position.z;
    const float oldSin = std::sin(oldPose.yaw);
    const float oldCos = std::cos(oldPose.yaw);
    const float localX = dx * oldCos - dz * oldSin;
    const float localZ = dx * oldSin + dz * oldCos;
    const float newSin = std::sin(newPose.yaw);
    const float newCos = std::cos(newPose.yaw);
    position.x = newPose.position.x + localX * newCos + localZ * newSin;
    position.z = newPose.position.z - localX * newSin + localZ * newCos;
    position.y += BoatDeckY(newPose) - BoatDeckY(oldPose);
}

static constexpr int kBoatGunnerMount = -1;
static constexpr int kStressHumveeGunnerMount = -2;

static void CarryBoatOccupants(const BoatPlatformPose& oldPose) {
    const BoatPlatformPose newPose = CurrentBoatPlatformPose();
    const float yawDelta = std::atan2(
        std::sin(newPose.yaw - oldPose.yaw),
        std::cos(newPose.yaw - oldPose.yaw));

    const float playerFeet =
        scene.camera.Position.y - scene.camera.PlayerHeight;
    if (scene.camera.FPSMode && scene.camera.IsGrounded &&
        BoatDeckSupports(scene.camera.Position, playerFeet, oldPose, 0.35f,
                         0.28f)) {
        const XMFLOAT3 previousPlayerPosition = scene.camera.Position;
        TransformWithBoat(scene.camera.Position, oldPose, newPose);
        g_game.playerMovement.ApplyPlatformDisplacement({
            scene.camera.Position.x - previousPlayerPosition.x,
            scene.camera.Position.y - previousPlayerPosition.y,
            scene.camera.Position.z - previousPlayerPosition.z });
        scene.camera.FloorY = BoatDeckY(newPose);
    }

    for (const auto& bandit : g_bandits) {
        if (!bandit || bandit->Dead() || bandit.get() == g_heldBandit) continue;
        if (bandit->turretGunner &&
            bandit->mountedVehicleIndex == kBoatGunnerMount) {
            bandit->position = BoatTurretMountWorld();
            continue;
        }
        if (bandit->turretGunner ||
            !BoatDeckSupports(bandit->position, bandit->position.y,
                              oldPose, 0.20f, 0.42f))
            continue;
        TransformWithBoat(bandit->position, oldPose, newPose);
        bandit->yaw += yawDelta;
        bandit->aimYaw += yawDelta;
    }
}

static void UpdateBoat(float dt) {
    if (!g_boatModel) return;
    const BoatPlatformPose oldPose = CurrentBoatPlatformPose();
    if (g_boatDead) {
        if (g_boatSunk) return;
        // Settle into the water rather than falling: sink depth grows and
        // levels off, with a slow list to one side as it goes under.
        g_boatSinkDepth += dt * 0.9f;
        g_boatRoll = (std::min)(0.55f, g_boatRoll + dt * 0.35f);
        if (g_boatSinkDepth >= 3.2f) {
            g_boatSinkDepth = 3.2f;
            g_boatSunk = true;
        }
        CarryBoatOccupants(oldPose);
        return;
    }

    g_boatPatrolTime += dt;
    // Slow circle around the island, water-level height with a light bob.
    // Starts 125 degrees around the circle (not phase 0, which sits right
    // in front of the player's spawn point) so it isn't beside the player
    // the moment the level loads.
    const float patrolPhase = g_boatPatrolTime * 0.065f +
                              XMConvertToRadians(125.0f);
    const float px = g_boatCenter.x + std::sin(patrolPhase) * kBoatPatrolRadius;
    const float pz = g_boatCenter.z + std::cos(patrolPhase) * kBoatPatrolRadius;
    g_boatPosition.x = px;
    g_boatPosition.z = pz;
    g_boatPosition.y = g_boatCenter.y + std::sin(g_boatPatrolTime * 0.9f) * 0.10f;

    // Face along the direction of travel (tangent to the circle).
    const float tangentX = std::cos(patrolPhase);
    const float tangentZ = -std::sin(patrolPhase);
    const float desiredYaw = std::atan2(tangentX, tangentZ);
    const float yawDelta = std::atan2(
        std::sin(desiredYaw - g_boatYaw), std::cos(desiredYaw - g_boatYaw));
    g_boatYaw += yawDelta * (1.0f - std::exp(-1.2f * (std::max)(0.0f, dt)));
    const float desiredRoll = std::sin(g_boatPatrolTime * 0.5f) * 0.035f;
    g_boatRoll += (desiredRoll - g_boatRoll) *
        (1.0f - std::exp(-2.0f * (std::max)(0.0f, dt)));
    CarryBoatOccupants(oldPose);
}

static XMFLOAT3 HumveeTurretMountWorld(size_t vehicleIndex) {
    XMFLOAT4X4 physicsPose;
    if (!g_destruction.GetVehicleTransform(vehicleIndex, physicsPose)) {
        if (vehicleIndex >= g_levelHumveeSpawns.size()) return {};
        const Transform& humvee = g_levelHumveeSpawns[vehicleIndex];
        XMStoreFloat4x4(&physicsPose,
            XMMatrixRotationY(XMConvertToRadians(humvee.rotation[1])) *
            XMMatrixTranslation(humvee.position[0], humvee.position[1],
                                humvee.position[2]));
    }
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3TransformCoord(
        XMLoadFloat3(&g_humveeTurretLocal), XMLoadFloat4x4(&physicsPose)));
    return result;
}

static void ConfigureHumveeBounds() {
    if (!g_humveeModel || !g_humveeModel->mesh) return;
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const MeshPrimitive& primitive : g_humveeModel->mesh->primitives) {
        for (size_t vertex = 0; vertex + 11 < primitive.vertices.size(); vertex += 12) {
            const float x = primitive.vertices[vertex];
            const float y = primitive.vertices[vertex + 1];
            const float z = primitive.vertices[vertex + 2];
            minimum.x = (std::min)(minimum.x, x);
            minimum.y = (std::min)(minimum.y, y);
            minimum.z = (std::min)(minimum.z, z);
            maximum.x = (std::max)(maximum.x, x);
            maximum.y = (std::max)(maximum.y, y);
            maximum.z = (std::max)(maximum.z, z);
        }
    }
    const float horizontalLength = (std::max)(
        maximum.x - minimum.x, maximum.z - minimum.z);
    if (horizontalLength <= 0.001f) return;
    g_humveeModelCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    g_humveeModelMinY = minimum.y;
    g_humveeModelScale = 4.8f / horizontalLength;
    const float vehicleTop = 2.5f + (maximum.y - minimum.y) * g_humveeModelScale;
    // Sink legs and pelvis through hatch; only upper torso remains above turret.
    g_humveeTurretLocal = { 0.0f, vehicleTop - 3.0f - 3.45f, 0.0f };
}

// Enemy grenades. Bandits lob one only from a mid-range band: too close and they
// would blow themselves up (and the player has no time to react), too far and the
// throw cannot reach. Inside the band they rifle-fire as before.
// Max range is set by physics, not taste: at grenadeThrowSpeed (16 m/s) the
// ballistic solver has no solution past ~26 m and degrades to a 45-degree heave
// that falls short, and the 2 s fuse burns out mid-air before the grenade
// arrives. 22 m keeps every throw landing within ~0.1 m of the player with fuse
// left. Raise grenadeThrowSpeed or grenadeFuse before raising this.
static constexpr float kBanditGrenadeMinRange = 12.0f;
static constexpr float kBanditGrenadeMaxRange = 22.0f;
// How much further a bandit picks out an occupied insertion craft than a man on
// foot. A helicopter or boat is large, loud and against open sky or water, so it
// is spotted well beyond infantry range -- but through the same visibility
// scaling, so fog and darkness still cover the approach.
static constexpr float kInsertionVehicleSpotRangeScale = 2.6f;
// Seconds between throws for one bandit, randomised per throw so a group does not
// settle into a synchronised rhythm.
static constexpr float kBanditGrenadeCooldownMin = 9.0f;
static constexpr float kBanditGrenadeCooldownMax = 16.0f;
// Chance to actually throw once in range and off cooldown. Keeps grenades feeling
// like an occasional decision rather than a metronome.
static constexpr float kBanditGrenadeChance = 0.35f;
// Marines throw semi-rarely: still less often than bandits once the lower
// roll is combined with the cooldown. Frequent enough to read as a real
// squad capability, rare enough that it stays a moment rather than a steady
// stream of explosions from the friendly side.
static constexpr float kMarineGrenadeCooldownMin = 9.0f;
static constexpr float kMarineGrenadeCooldownMax = 15.0f;
static constexpr float kMarineGrenadeChance = 0.25f;
// Nobody on the player's side should be inside a friendly blast. Checked
// against the impact point before an ally commits to the throw; the grenade
// blast radius itself is scene.grenadeEnemyRadius, so this carries margin.
static constexpr float kAllyGrenadeSafeRadius = 9.0f;
