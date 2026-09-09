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
            for (auto& bandit : g_bandits) {
                if (!bandit || bandit->Dead() || bandit->turretGunner) continue;
                XMFLOAT3 impact;
                if (!bandit->ApplyDebrisImpact(vehicleImpact, &impact)) continue;
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
    for (const CollisionMeshInstance& instance : g_prefabMeshColliders) {
        const XMFLOAT3 base(bandit.position.x, feet, bandit.position.z);
        const CollisionMeshPushout pushout = CollisionMeshInstanceResolveCapsule(
            instance, base, kActorRadius, kActorHeight, kBanditMaxStepDown);
        if (!pushout.touched) continue;
        bandit.position.x += pushout.displacement.x;
        bandit.position.z += pushout.displacement.z;
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
