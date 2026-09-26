#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void UpdateHumveeChaseCamera(float dt) {
    if (!g_drivingHumvee || g_activeHumveeIndex == kNoHumvee) return;
    XMFLOAT4X4 pose;
    XMFLOAT3 position, forward;
    if (!g_destruction.GetVehicleTransform(
            g_activeHumveeIndex, pose, &position, &forward)) return;

    const XMVECTOR target = XMLoadFloat3(&position) +
        XMVectorSet(0.0f, 1.65f, 0.0f, 0.0f);
    // Camera Front comes from mouse look. Orbit around vehicle using that view
    // direction instead of forcing camera behind chassis heading every frame.
    const XMVECTOR orbitView = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    const XMVECTOR desired = target - orbitView * 6.5f;
    const float follow = 1.0f - std::exp(-8.0f * (std::max)(0.0f, dt));
    const XMVECTOR cameraPosition = XMVectorLerp(
        XMLoadFloat3(&scene.camera.Position), desired, follow);
    XMStoreFloat3(&scene.camera.Position, cameraPosition);

    XMStoreFloat3(&scene.camera.Front,
        XMVector3Normalize(target - cameraPosition));
    scene.camera.Up = { 0.0f, 1.0f, 0.0f };
}

static XMFLOAT3 HumveeScreenCenterAimPoint() {
    const XMFLOAT3 origin = scene.camera.Position;
    const XMFLOAT3 end = {
        origin.x + scene.camera.Front.x * 140.0f,
        origin.y + scene.camera.Front.y * 140.0f,
        origin.z + scene.camera.Front.z * 140.0f };
    XMFLOAT3 closest = end;
    float closestDistanceSq = FLT_MAX;
    auto accept = [&](const XMFLOAT3& hit) {
        const float dx = hit.x - origin.x;
        const float dy = hit.y - origin.y;
        const float dz = hit.z - origin.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq < closestDistanceSq) {
            closestDistanceSq = distanceSq;
            closest = hit;
        }
    };
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(origin, end, 0.03f, hit)) accept(hit);
    if (HitTerrainSegment(origin, end, 0.03f, hit)) accept(hit);
    return closest;
}

static void UpdateHumveeTurretAimAt(size_t vehicleIndex,
                                    const XMFLOAT3& aimPoint, float dt) {
    if (vehicleIndex >= g_humveeGameplay.size() || !g_humveeModel ||
        !g_humveeTurretNode) return;
    HumveeGameplayState& state = g_humveeGameplay[vehicleIndex];
    state.aimPoint = aimPoint;
    const XMMATRIX modelWorld = HumveeWorldMatrix(vehicleIndex);
    const XMMATRIX inverseModel = XMMatrixInverse(nullptr, modelWorld);
    XMFLOAT3 localTarget;
    XMStoreFloat3(&localTarget, XMVector3TransformCoord(
        XMLoadFloat3(&state.aimPoint), inverseModel));
    const float dx = localTarget.x - g_humveeTurretNode->translation.x;
    const float dz = localTarget.z - g_humveeTurretNode->translation.z;
    if (dx * dx + dz * dz < 0.001f) return;
    const float desiredYaw = std::atan2(dx, dz);
    const float yawDelta = std::atan2(
        std::sin(desiredYaw - state.turretYaw),
        std::cos(desiredYaw - state.turretYaw));
    const float maxTraverse = 1.35f * (std::max)(0.0f, dt);
    state.turretYaw += (std::max)(-maxTraverse,
        (std::min)(maxTraverse, yawDelta));
    PrepareHumveeModelForRender(vehicleIndex);
}

static void UpdateHumveeTurretAim(float dt) {
    if (!g_drivingHumvee || g_activeHumveeIndex == kNoHumvee) return;
    UpdateHumveeTurretAimAt(
        g_activeHumveeIndex, HumveeScreenCenterAimPoint(), dt);
}

static void FireHumveeTurret() {
    if (!g_drivingHumvee || g_activeHumveeIndex >= g_humveeGameplay.size() ||
        !g_humveeTurretNode) return;
    HumveeGameplayState& state = g_humveeGameplay[g_activeHumveeIndex];
    if (state.turretFireCooldown > 0.0f) return;
    state.aimPoint = HumveeScreenCenterAimPoint();
    PrepareHumveeModelForRender(g_activeHumveeIndex);
    const XMMATRIX turretWorld =
        XMLoadFloat4x4(&g_humveeTurretNode->globalTransform) *
        HumveeWorldMatrix(g_activeHumveeIndex);
    XMFLOAT3 muzzle;
    XMStoreFloat3(&muzzle, XMVector3TransformCoord(
        XMVectorSet(0.0f, 72.0f, 338.0f, 1.0f), turretWorld));
    XMVECTOR direction = XMLoadFloat3(&state.aimPoint) - XMLoadFloat3(&muzzle);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 0.01f) return;
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));
    scene.SpawnPlayerProjectile(muzzle, shotDirection, 1.35f);
    scene.SpawnWeaponSmoke(muzzle, shotDirection, 1.15f);
    g_gunAudio.Play(0.72f, 0.90f + ((float)std::rand() / RAND_MAX) * 0.06f);
    state.turretFireCooldown = 0.12f;
}

static void UpdateHumveeImpacts(float dt) {
    for (size_t vehicleIndex = 0;
         vehicleIndex < g_humveeGameplay.size(); ++vehicleIndex) {
        XMFLOAT4X4 pose;
        XMFLOAT3 position, forward, velocity;
        if (!g_destruction.GetVehicleTransform(
                vehicleIndex, pose, &position, &forward, &velocity)) continue;
        HumveeGameplayState& state = g_humveeGameplay[vehicleIndex];

        state.houseImpactCooldown =
            (std::max)(0.0f, state.houseImpactCooldown - dt);
        const float speed = std::sqrt(
            velocity.x * velocity.x + velocity.y * velocity.y +
            velocity.z * velocity.z);
        if (!state.previousPositionValid) {
            state.previousPosition = position;
            state.previousPositionValid = true;
            continue;
        }
        const float travelX = position.x - state.previousPosition.x;
        const float travelY = position.y - state.previousPosition.y;
        const float travelZ = position.z - state.previousPosition.z;
        if (travelX * travelX + travelY * travelY + travelZ * travelZ >
            100.0f) {
            state.previousPosition = position;
            continue;
        }

        if (speed >= 3.5f && g_banditLoaded) {
            XMFLOAT3 worldMin(FLT_MAX, FLT_MAX, FLT_MAX);
            XMFLOAT3 worldMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            const XMMATRIX world = XMLoadFloat4x4(&pose);
            for (float x : { -2.25f, 2.25f })
            for (float y : { -0.7f, 0.7f })
            for (float z : { -1.05f, 1.05f }) {
                XMFLOAT3 corner;
                XMStoreFloat3(&corner, XMVector3TransformCoord(
                    XMVectorSet(x, y, z, 1.0f), world));
                worldMin.x = (std::min)(worldMin.x, corner.x);
                worldMin.y = (std::min)(worldMin.y, corner.y);
                worldMin.z = (std::min)(worldMin.z, corner.z);
                worldMax.x = (std::max)(worldMax.x, corner.x);
                worldMax.y = (std::max)(worldMax.y, corner.y);
                worldMax.z = (std::max)(worldMax.z, corner.z);
            }
            const DestructionDebrisHazard vehicleImpact = {
                worldMin, worldMax, position, velocity, 1200.0f,
                speed >= 7.0f };
            // Running someone down only counts when the player is the one at the
            // wheel. An AI-driven Humvee flattening a bandit is not their kill.
            const bool playerDriving =
                g_drivingHumvee && g_activeHumveeIndex == vehicleIndex;
            for (auto& bandit : g_bandits) {
                if (!bandit || bandit->Dead() || bandit->turretGunner) continue;
                // An enemy-driven Humvee hunting the player does not plough
                // through its own side on the way.
                if (state.aiDriving && bandit->faction == Faction::Bandit)
                    continue;
                XMFLOAT3 impact;
                if (!bandit->ApplyDebrisImpact(vehicleImpact, &impact,
                                               playerDriving)) continue;
                XMFLOAT3 normal;
                XMStoreFloat3(&normal,
                    XMVector3Normalize(-XMLoadFloat3(&velocity)));
                scene.SpawnBloodBurst(impact, normal);
                g_hitAudio.Play(0.34f,
                    0.88f + ((float)std::rand() / RAND_MAX) * 0.18f);
            }
        }

        if (speed >= 4.5f && state.houseImpactCooldown <= 0.0f) {
            XMFLOAT3 impactDirection = { velocity.x, 0.0f, velocity.z };
            XMVECTOR impactVector = XMLoadFloat3(&impactDirection);
            if (XMVectorGetX(XMVector3LengthSq(impactVector)) < 0.01f)
                impactVector = XMLoadFloat3(&forward);
            XMStoreFloat3(&impactDirection, XMVector3Normalize(impactVector));
            const XMFLOAT3 start = {
                state.previousPosition.x + impactDirection.x * 2.1f,
                state.previousPosition.y,
                state.previousPosition.z + impactDirection.z * 2.1f };
            const XMFLOAT3 end = {
                position.x + impactDirection.x * 2.65f,
                position.y,
                position.z + impactDirection.z * 2.65f };
            XMFLOAT3 hit;
            if (g_destruction.HitTestSegment(start, end, 0.75f, hit)) {
                const float impactStrength =
                    (std::min)(320.0f, 80.0f + speed * 24.0f);
                g_destruction.ApplyExplosion(
                    hit, (std::min)(3.2f, 1.4f + speed * 0.16f),
                    impactStrength, impactStrength);
                scene.SpawnSmokeBurst(hit, 0.65f, 0.7f);
                state.houseImpactCooldown = 0.4f;
            }
        }
        state.previousPosition = position;
    }
}

// Pushes an actor out of the props it is standing inside, the same way
// ResolvePlayerPrefabCollisions does for the player.
//
// The navmesh is only advice: an actor steers toward its next waypoint, but
// falls back to walking straight at its target whenever FindPath returns
// nothing -- which is common for a marine following a player who is standing on
// or beside a prop. Nothing enforced the geometry at the movement step, so any
// actor on that fallback walked straight through containers and barracks.
// Resolving position here fixes it regardless of why the path was missing.
static void ResolveBanditPrefabCollisions(SkinnedEnemy& bandit) {
    if (bandit.Dead() || bandit.Held() || bandit.turretGunner) return;

    constexpr float kActorRadius = 0.42f;
    constexpr float kActorHeight = 1.75f;
    const float feet = bandit.position.y + bandit.footOffset;

    // Per-triangle geometry first, so an actor can walk through a doorway
    // instead of being stopped by the box wrapping the whole building.
    //
    // The capsule resolve also reports the walkable surface under the actor,
    // found with the same step allowance it uses to climb. Taking it is what
    // lets an actor step up onto a kerb, a road shoulder or a stair tread
    // instead of walking along at terrain height with its shins through the
    // edge: Update() snaps position.y to the terrain heightfield, which knows
    // nothing about prefab geometry, and PrefabSurfaceSupports earlier in the
    // frame only probes straight down, so it finds a deck the actor is already
    // on top of but never one whose lip is in front of it.
    //
    // Highest wins across instances, and only ever upward -- a raised floor
    // must lift the actor, but nothing here may drop it, or the same result
    // would pull an actor down through the terrain it is standing on.
    float steppedFloorY = 0.0f;
    bool hasSteppedFloor = false;
    for (const CollisionMeshInstance& instance : g_prefabMeshColliders) {
        const XMFLOAT3 base(bandit.position.x, feet, bandit.position.z);
        const CollisionMeshPushout pushout = CollisionMeshInstanceResolveCapsule(
            instance, base, kActorRadius, kActorHeight, kBanditMaxStepDown);
        if (!pushout.touched) continue;
        bandit.position.x += pushout.displacement.x;
        bandit.position.z += pushout.displacement.z;
        if (pushout.hasFloor &&
            (!hasSteppedFloor || pushout.floorY > steppedFloorY)) {
            steppedFloorY = pushout.floorY;
            hasSteppedFloor = true;
        }
    }
    if (hasSteppedFloor) {
        // floorY is where the feet belong; position.y is the actor's origin,
        // which sits footOffset below them.
        const float steppedPositionY = steppedFloorY - bandit.footOffset;
        if (steppedPositionY > bandit.position.y)
            bandit.position.y = steppedPositionY;
    }

    for (const PrefabCollider& collider : g_prefabColliders) {
        // Entities with a triangle mesh were just resolved against it; the
        // bounds box on top would shove the actor back out of the building it
        // just walked into.
        if (g_meshCollisionEntities.count(collider.entityId)) continue;
        // Vertical overlap only: a container the actor is standing on top of,
        // or one it is walking under, must not drag it sideways.
        if (feet >= collider.center.y + collider.halfExtents.y ||
            feet + kActorHeight <= collider.center.y - collider.halfExtents.y)
            continue;

        const XMFLOAT3 local =
            PrefabColliderToLocal(collider, bandit.position);
        float localX = local.x;
        float localZ = local.z;
        const float halfX = collider.halfExtents.x + kActorRadius;
        const float halfZ = collider.halfExtents.z + kActorRadius;
        if (std::abs(localX) >= halfX || std::abs(localZ) >= halfZ) continue;

        // Leave along the shallower axis, which is the nearest way out.
        const float penetrationX = halfX - std::abs(localX);
        const float penetrationZ = halfZ - std::abs(localZ);
        if (penetrationX < penetrationZ)
            localX = std::copysign(halfX, std::abs(localX) > 0.001f ? localX : 1.0f);
        else
            localZ = std::copysign(halfZ, std::abs(localZ) > 0.001f ? localZ : 1.0f);

        const float worldCosine = std::cos(collider.yawRadians);
        const float worldSine = std::sin(collider.yawRadians);
        bandit.position.x = collider.center.x +
                            localX * worldCosine - localZ * worldSine;
        bandit.position.z = collider.center.z +
                            localX * worldSine + localZ * worldCosine;
    }
}

static bool ResolveBanditHumveeCollision(SkinnedEnemy& bandit) {
    if (bandit.Dead() || bandit.Held() || bandit.turretGunner) return false;
    for (size_t vehicleIndex = 0;
         vehicleIndex < g_destruction.VehicleCount(); ++vehicleIndex) {
        XMFLOAT4X4 pose;
        if (!g_destruction.GetVehicleTransform(vehicleIndex, pose)) continue;

        const XMMATRIX vehicleWorld = XMLoadFloat4x4(&pose);
        const XMMATRIX vehicleLocal = XMMatrixInverse(nullptr, vehicleWorld);
        const XMVECTOR bodyCenter = XMVectorSet(
            bandit.position.x,
            bandit.position.y + bandit.footOffset + 1.0f,
            bandit.position.z, 1.0f);
        XMFLOAT3 local;
        XMStoreFloat3(&local,
            XMVector3TransformCoord(bodyCenter, vehicleLocal));

        constexpr float enemyRadius = 0.58f;
        constexpr float halfX = 2.25f + enemyRadius;
        constexpr float halfZ = 1.05f + enemyRadius;
        if (local.y < -0.85f || local.y > 1.15f ||
            std::abs(local.x) >= halfX || std::abs(local.z) >= halfZ)
            continue;

        const float penetrationX = halfX - std::abs(local.x);
        const float penetrationZ = halfZ - std::abs(local.z);
        if (penetrationX < penetrationZ)
            local.x = std::copysign(halfX + 0.02f,
                std::abs(local.x) > 0.001f ? local.x : 1.0f);
        else
            local.z = std::copysign(halfZ + 0.02f,
                std::abs(local.z) > 0.001f ? local.z : 1.0f);

        XMFLOAT3 corrected;
        XMStoreFloat3(&corrected, XMVector3TransformCoord(
            XMLoadFloat3(&local), vehicleWorld));
        bandit.position.x += corrected.x - XMVectorGetX(bodyCenter);
        bandit.position.z += corrected.z - XMVectorGetZ(bodyCenter);
        return true;
    }
    return false;
}

// Keeps actors out of each other. Props and vehicles were the only things that
// ever pushed an actor aside, so nothing stopped two from standing in the same
// spot: a squad ordered to one position converged on it and merged into a
// single body with five heads, and a marine following the player walked through
// anyone already there.
//
// One pass over all actors AFTER the update loop, not a resolve inside it. A
// pair has to be settled once, symmetrically: resolving per-actor mid-loop
// pushes each pair twice, and the second actor would be shoved off a position
// the first had already been corrected against, which walks a crowd sideways
// instead of spreading it. Splitting the overlap between the two also means a
// jam resolves from both ends at once rather than one body bulldozing another.
static void ResolveActorSeparation() {
    // Chest-width, not the 0.42 used against props. A prop pushout has to let
    // an actor through a doorway, so its radius is the shoulder; two bodies
    // only have to stop visibly interpenetrating, and a pair of 0.42s leaves
    // them still clipping at the torso. 0.34 each gives 0.68 between centres,
    // which reads as shoulder to shoulder and still fits a squad through a gap.
    constexpr float kActorRadius = 0.34f;
    constexpr float kMinSeparation = kActorRadius * 2.0f;
    // Vertical reach of a body. Without it an actor on a container roof is
    // shoved by someone standing underneath it, which is the same mistake the
    // prefab box resolve avoids with its own vertical overlap test.
    constexpr float kActorHeight = 1.75f;

    // Held, mounted and rappelling actors have their position owned by
    // something else -- a grab, a turret seat, a rope -- and pushing them would
    // fight that owner every frame. Dead bodies are left alone so a corpse
    // stays where it fell instead of being nudged around by the living, and a
    // network-controlled actor's position belongs to the machine that owns it.
    const auto movable = [](const SkinnedEnemy& actor) {
        return !actor.Dead() && !actor.Held() && !actor.turretGunner &&
               !actor.Rappelling() && !actor.networkControlled;
    };

    // Who was actually pushed, so the prop re-resolve below only pays for the
    // handful of actors in a jam rather than every actor in the level.
    std::vector<bool> separated(g_bandits.size(), false);

    for (size_t i = 0; i < g_bandits.size(); ++i) {
        SkinnedEnemy* a = g_bandits[i].get();
        if (!a || !movable(*a)) continue;
        for (size_t j = i + 1; j < g_bandits.size(); ++j) {
            SkinnedEnemy* b = g_bandits[j].get();
            if (!b || !movable(*b)) continue;

            const float aFeet = a->position.y + a->footOffset;
            const float bFeet = b->position.y + b->footOffset;
            if (aFeet >= bFeet + kActorHeight ||
                bFeet >= aFeet + kActorHeight)
                continue;

            float dx = b->position.x - a->position.x;
            float dz = b->position.z - a->position.z;
            float distanceSquared = dx * dx + dz * dz;
            if (distanceSquared >= kMinSeparation * kMinSeparation) continue;

            float distance = std::sqrt(distanceSquared);
            if (distance < 1e-4f) {
                // Exactly coincident, which spawning a squad on one point
                // does produce. There is no separating axis to read off, so
                // pick a deterministic one from the pair's index rather than
                // a random direction: a random push would jitter a stuck pair
                // differently every frame instead of settling.
                const float angle = static_cast<float>(i + j) * 2.39996f;
                dx = std::cos(angle);
                dz = std::sin(angle);
                distance = 0.0f;
            } else {
                dx /= distance;
                dz /= distance;
            }

            // Half each, so neither actor wins the exchange.
            const float push = (kMinSeparation - distance) * 0.5f;
            a->position.x -= dx * push;
            a->position.z -= dz * push;
            b->position.x += dx * push;
            b->position.z += dz * push;
            separated[i] = true;
            separated[j] = true;
        }
    }

    // This pass runs after the per-actor prop resolve, so a push just now could
    // have put someone inside a wall -- two marines jammed against a container
    // separate along the only axis they have, which is into it. Re-resolving
    // the ones that actually moved keeps the geometry authoritative: the worst
    // case becomes a pair still touching because the prop will not let them
    // apart, which is correct, rather than one of them standing in the crate.
    for (size_t i = 0; i < g_bandits.size(); ++i) {
        if (!separated[i]) continue;
        SkinnedEnemy* actor = g_bandits[i].get();
        if (!actor) continue;
        ResolveBanditPrefabCollisions(*actor);
    }
}

// ---- Captured tanks -------------------------------------------------------
// Any living enemy tank, at any health, can be taken over with E from beside
// its hull. W/S drive, A/D steer, the mouse lays the turret and the left
// button fires the tank's own gun. Only the machine that simulates tanks can
// board one: on a client they are poses from the host with no body to drive.

// Metres from the hull box's surface at which E boards.
static constexpr float kTankBoardReach = 2.5f;
// A player-fired round flies faster than the AI's dodgeable one: nobody has
// to be given a chance to sidestep the player's gun.
static constexpr float kPlayerTankShellSpeed = 99.0f;
static float g_playerTankReload = 0.0f;
// SGE_TANK_BOARD_TEST's stand-in for the keys, read where the keys are, so the
// scripted drive and the real one take the same single path to the solver.
static bool g_tankTestInputActive = false;
static float g_tankTestThrottle = 0.0f;
static float g_tankTestTurn = 0.0f;

static const PrefabCollider* TankHullCollider(const EnemyTankState& tank) {
    for (const PrefabCollider& collider : g_prefabColliders)
        if (collider.entityId == tank.entityId &&
            collider.prefabId == tank.hullPrefabId) return &collider;
    return nullptr;
}

static void ExitPlayerTank() {
    EnemyTankState* tank = PlayerTank();
    g_playerTankEntity = 0;
    scene.playerArmored = false;
    scene.gun.visible = g_savedGunVisible;
    scene.camera.FPSMode = true;
    scene.camera.VerticalVelocity = 0.0f;
    if (!tank) return;
    g_destruction.SetGroundVehicleInput(tank->physicsHandle, 0.0f, 0.0f, true);
    // Out over the side, clear of the tracks, standing on the ground there.
    const float halfWidth = tank->spec.chassisHalfExtents.z / 0.9f;
    XMFLOAT3 exit;
    XMStoreFloat3(&exit, XMVector3TransformCoord(XMVectorSet(
        tank->boxCenterLocal.x, 0.0f,
        tank->boxCenterLocal.z + halfWidth + 1.4f, 1.0f),
        EnemyTankHullWorld(*tank)));
    exit.y = GroundHeightAt(exit.x, exit.z) + scene.camera.PlayerHeight + 0.2f;
    scene.camera.Position = exit;
}

// E: out of the tank being driven, or into the nearest one in reach. False
// when this press is not about a tank, so the Humvee gets its turn.
static bool ToggleTankDriving(bool anyDistance = false) {
    if (g_playerTankEntity != 0) {
        ExitPlayerTank();
        return true;
    }
    if (g_drivingHumvee ||
        scene.player.health <= 0.0f || scene.player.downed) return false;
    EnemyTankState* best = nullptr;
    float bestSurface = anyDistance ? FLT_MAX : kTankBoardReach;
    for (EnemyTankState& tank : g_enemyTanks) {
        if (tank.dead || tank.physicsHandle == 0) continue;
        const PrefabCollider* collider = TankHullCollider(tank);
        if (!collider) continue;
        const XMFLOAT3 local =
            PrefabColliderToLocal(*collider, scene.camera.Position);
        const float outX = (std::max)(0.0f,
            std::abs(local.x) - collider->halfExtents.x);
        const float outY = (std::max)(0.0f,
            std::abs(local.y) - collider->halfExtents.y);
        const float outZ = (std::max)(0.0f,
            std::abs(local.z) - collider->halfExtents.z);
        const float surface =
            std::sqrt(outX * outX + outY * outY + outZ * outZ);
        if (surface >= bestSurface) continue;
        bestSurface = surface;
        best = &tank;
    }
    if (!best) return false;
    best->captured = true;
    best->reverseTime = 0.0f;
    best->stuckTime = 0.0f;
    g_playerTankEntity = best->entityId;
    g_playerTankReload = 0.5f;
    scene.playerArmored = true;
    g_savedGunVisible = scene.gun.visible;
    scene.gun.visible = false;
    scene.camera.FPSMode = false;
    scene.camera.VerticalVelocity = 0.0f;
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Player boarded tank " + best->hullPrefabId + " (" +
        std::to_string(best->entityId) + "), health " +
        std::to_string(best->health) + "/" + std::to_string(best->maxHealth));
    return true;
}

// Throttle and turn in [-1, 1]; turn is positive to the left, as A is.
// The steering sign comes from the AI's heading solve, which is measured: it
// steers with +1 toward a heading on the side cross(forward, want) < 0 names.
// A walks the camera along +cross(front, up), so the hull's left is
// cross(forward, up) = (-fz, 0, fx), whose solve gives -1: left is negative.
static void DrivePlayerTank(float throttle, float turn, bool brake) {
    EnemyTankState* tank = PlayerTank();
    if (!tank || tank->dead) return;
    const float steering = (std::max)(-1.0f, (std::min)(1.0f, -turn));
    g_destruction.SetGroundVehicleInput(tank->physicsHandle,
        throttle * kEnemyTankThrottleSign, steering,
        brake || throttle == 0.0f);
}

static void FirePlayerTankShell() {
    EnemyTankState* tank = PlayerTank();
    if (!tank || tank->dead || g_playerTankReload > 0.0f) return;
    const XMMATRIX turretWorld =
        EnemyTankTurretWorld(*tank, EnemyTankHullWorld(*tank));
    XMFLOAT3 muzzle;
    XMStoreFloat3(&muzzle, XMVector3TransformCoord(
        XMLoadFloat3(&tank->muzzleLocal), turretWorld));
    // The gun only traverses, so the round leaves along the barrel's heading
    // and is pitched to meet the crosshair's range: firing mid-traverse
    // misses where the barrel is still pointing, as it should.
    const XMFLOAT3 aim = HumveeScreenCenterAimPoint();
    XMFLOAT3 barrel;
    XMStoreFloat3(&barrel, XMVector3TransformNormal(
        XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), turretWorld));
    const float barrelFlat =
        std::sqrt(barrel.x * barrel.x + barrel.z * barrel.z);
    if (barrelFlat < 1e-4f) return;
    const float ax = aim.x - muzzle.x, az = aim.z - muzzle.z;
    const float range = (std::max)(1.0f, std::sqrt(ax * ax + az * az));
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(XMVectorSet(
        barrel.x / barrelFlat * range, aim.y - muzzle.y,
        barrel.z / barrelFlat * range, 0.0f)));
    const XMFLOAT3 start = EnemyTankShellStart(*tank, muzzle, shotDirection);
    SpawnEnemyTankShell(muzzle, start, shotDirection, kPlayerTankShellSpeed,
                        tank->fireRange * 1.6f / kPlayerTankShellSpeed,
                        tank->shellDamage, /*hostReplica=*/false,
                        /*playerOwned=*/true);
    scene.camera.AddFireTrauma(0.12f);
    g_playerTankReload = tank->reloadSeconds;
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Player tank fired from " + std::to_string(muzzle.x) + ", " +
        std::to_string(muzzle.y) + ", " + std::to_string(muzzle.z) +
        " at " + std::to_string(aim.x) + ", " + std::to_string(aim.y) +
        ", " + std::to_string(aim.z));
}

// SGE_TANK_BOARD_TEST=1: once a tank is placed, board the nearest one from
// anywhere and run a fixed drive -- straight, then A, then D, firing on
// reload -- logging heading and position, so the steering and throttle signs
// and the gun can be checked without anyone at the keyboard.
static void RunTankBoardTest(float dt) {
    static const bool enabled =
        GetEnvironmentVariableA("SGE_TANK_BOARD_TEST", nullptr, 0) > 0;
    if (!enabled || g_enemyTanks.empty() || g_game.loading.Active()) return;
    static float clock = -1.0f;
    static float logTimer = 0.0f;
    if (clock < 0.0f) {
        if (!ToggleTankDriving(/*anyDistance=*/true)) return;
        clock = 0.0f;
    }
    clock += dt;
    EnemyTankState* tank = PlayerTank();
    if (!tank) return;
    // SGE_TANK_BOARD_TEST_STRAIGHT=1: full throttle dead ahead for 20 s, to
    // drive onto or into whatever is placed in front of the hull.
    static const bool straight =
        GetEnvironmentVariableA("SGE_TANK_BOARD_TEST_STRAIGHT", nullptr, 0) > 0;
    const float throttle = clock < (straight ? 20.0f : 12.0f) ? 1.0f : 0.0f;
    const float turn = straight ? 0.0f
        : (clock < 4.0f ? 0.0f : (clock < 8.0f ? 1.0f : -1.0f));
    g_tankTestInputActive = true;
    g_tankTestThrottle = throttle;
    g_tankTestTurn = turn;
    // Look at the nearest other live tank, as a player would, so the turret
    // lays on it and the rounds show whether the gun hurts armour.
    const EnemyTankState* mark = nullptr;
    float markSq = FLT_MAX;
    for (const EnemyTankState& other : g_enemyTanks) {
        if (&other == tank || other.dead) continue;
        const float dx = other.position.x - tank->position.x;
        const float dz = other.position.z - tank->position.z;
        if (dx * dx + dz * dz < markSq) { markSq = dx * dx + dz * dz; mark = &other; }
    }
    if (mark) {
        XMStoreFloat3(&scene.camera.Front, XMVector3Normalize(
            XMLoadFloat3(&mark->position) - XMLoadFloat3(&scene.camera.Position)));
        FirePlayerTankShell();
    }
    logTimer -= dt;
    if (logTimer > 0.0f) return;
    logTimer = 1.0f;
    XMFLOAT3 forward;
    XMStoreFloat3(&forward, XMVector3Rotate(
        XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), XMLoadFloat4(&tank->rotation)));
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "TankBoardTest t " + std::to_string(clock) + " turn " +
        std::to_string(turn) + " pos " + std::to_string(tank->position.x) +
        ", " + std::to_string(tank->position.y) + ", " +
        std::to_string(tank->position.z) + " terrain " +
        std::to_string(GroundHeightAt(tank->position.x, tank->position.z)) +
        " heading " +
        std::to_string(std::atan2(forward.z, forward.x)) + " turret " +
        std::to_string(tank->turretYaw) + " health " +
        std::to_string(tank->health) + " playerHealth " +
        std::to_string(scene.player.health));
}

// Chase camera, turret laying and the tank's end: run every frame after the
// physics sync. A tank that is wrecked with the player aboard throws them out
// hurt; one that is gone (the prefab rebuild restarts every tank) or no longer
// captured just lets them out.
static void UpdatePlayerTank(float dt) {
    RunTankBoardTest(dt);
    if (g_playerTankEntity == 0) return;
    EnemyTankState* tank = PlayerTank();
    if (!tank || !tank->captured || tank->dead) {
        const bool destroyed = tank && tank->dead;
        ExitPlayerTank();
        if (destroyed) {
            scene.DamagePlayer(45.0f);
            SGE_LOG("LogGameplay", EngineLog::Level::Display,
                "Player's tank destroyed; thrown clear at health " +
                std::to_string(scene.player.health));
        }
        return;
    }
    g_playerTankReload = (std::max)(0.0f, g_playerTankReload - dt);
    const XMMATRIX hullWorld = EnemyTankHullWorld(*tank);

    // Orbit like the Humvee's chase camera, further out for the bigger hull,
    // looking over the turret roof.
    XMFLOAT3 roof = tank->boxCenterLocal;
    roof.y = tank->boxCenterLocal.y * 2.0f + 0.6f;
    const XMVECTOR target =
        XMVector3TransformCoord(XMLoadFloat3(&roof), hullWorld);
    const float distance = tank->spec.chassisHalfExtents.x * 2.0f + 4.5f;
    const XMVECTOR orbitView =
        XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    XMFLOAT3 desired;
    XMStoreFloat3(&desired, target - orbitView * distance);
    desired.y = (std::max)(desired.y,
        GroundHeightAt(desired.x, desired.z) + 0.6f);
    const float follow = 1.0f - std::exp(-8.0f * (std::max)(0.0f, dt));
    const XMVECTOR cameraPosition = XMVectorLerp(
        XMLoadFloat3(&scene.camera.Position), XMLoadFloat3(&desired), follow);
    XMStoreFloat3(&scene.camera.Position, cameraPosition);
    XMStoreFloat3(&scene.camera.Front,
        XMVector3Normalize(target - cameraPosition));
    scene.camera.Up = { 0.0f, 1.0f, 0.0f };

    // Turret onto the crosshair, in the hull's frame, faster than the AI's
    // warning-paced traverse.
    const XMFLOAT3 aim = HumveeScreenCenterAimPoint();
    XMFLOAT3 pivot;
    XMStoreFloat3(&pivot, XMVector3TransformCoord(
        XMLoadFloat3(&tank->turretPivot), hullWorld));
    const XMMATRIX orientation =
        XMMatrixRotationQuaternion(XMLoadFloat4(&tank->rotation));
    const XMVECTOR local = XMVector3TransformNormal(
        XMVectorSet(aim.x - pivot.x, 0.0f, aim.z - pivot.z, 0.0f),
        XMMatrixTranspose(orientation));
    if (XMVectorGetX(XMVector3LengthSq(local)) > 0.01f) {
        const float desiredYaw =
            std::atan2(-XMVectorGetZ(local), XMVectorGetX(local));
        const float error = std::atan2(
            std::sin(desiredYaw - tank->turretYaw),
            std::cos(desiredYaw - tank->turretYaw));
        const float step =
            (std::max)(1.2f, tank->turretRate * 2.0f) * (std::max)(0.0f, dt);
        tank->turretYaw += (std::max)(-step, (std::min)(step, error));
    }
}
