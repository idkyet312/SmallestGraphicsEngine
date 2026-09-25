#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Enemy tanks: a prefab carrying an "enemyTank" component drives on the same
// wheel-joint car physics as the Humvee, hunts the player, traverses its turret
// and fires slow, dodgeable high-explosive shells.
//
//   "enemyTank": {
//     "turret": "props/abrams_turret",   // child prefab drawn as the turret
//     "muzzle": [5.93, -0.27, 0.0],      // in turret space, metres
//     "hitHeight": 2.71,                 // hull hit box top, model space
//     "health": 1500, "detectRange": 140, "fireRange": 110,
//     "standoff": 35, "reload": 6.0, "shellSpeed": 45
//   }
//
// The hull and turret stay ordinary prefab render batches -- the editor places
// and previews them like any prop -- and each frame the tank writes its
// simulated pose into their `transforms`. `baseTransforms` keeps the authored
// placement, as it does for rigid-body props and the objective aircraft.
//
// Small arms do nothing to it: bullets strike the hull collider, which has no
// "destructible" health to spend. Explosives are resolved here against the hull
// box (DamageEnemyTanksFromBlast), not by distance to the prefab origin, which
// sits metres from the glacis a rocket actually hits.

static constexpr float kEnemyTankRocketDamage = 500.0f;
// Water a hull will drive through. Deeper than this it holds position rather
// than wade on: the terrain heightfield runs on under the sea, and a tank
// allowed to follow it out ends up beyond the physics ground and falls.
static constexpr float kEnemyTankFordDepth = 1.2f;
// How close a tank with no line of fire will push in before giving up on the
// standoff and simply holding.
static constexpr float kEnemyTankMinimumRange = 12.0f;
// The wheel joints roll the chassis toward -X for positive spin: measured, a
// hull facing the player drove away from them at full throttle. The model's
// gun and glacis face +X, so the tank drives with the spin sign flipped. The
// Humvee shares the joints and keeps its own convention untouched.
static constexpr float kEnemyTankThrottleSign = -1.0f;
// A burning wreck keeps smoking this long before it is left to sit.
static constexpr float kEnemyTankWreckSmokeSeconds = 40.0f;

struct EnemyTankState {
    uint64_t entityId = 0;
    std::string hullPrefabId;
    std::string turretPrefabId;
    uint32_t physicsHandle = 0;
    GroundVehicleSpec spec;
    // Chassis centre in the placement's scaled model space. The body sits
    // there; the model draws from its origin, so the offset is undone on sync.
    XMFLOAT3 chassisOffset{};
    // Placement scale, re-applied under the simulated rotation.
    XMFLOAT4X4 scaleTransform{};
    // Turret child: its authored scale/rotation, and the traverse pivot in the
    // hull's unscaled model space (the child's local translation).
    XMFLOAT4X4 turretLocal{};
    XMFLOAT3 turretPivot{};
    XMFLOAT3 muzzleLocal{ 5.93f, -0.27f, 0.0f };
    // Hull hit box in model space, the collider's source.
    XMFLOAT3 boxCenterLocal{};
    // Last simulated pose, for the draw and for recreating the body if the
    // physics world is rebuilt under it.
    XMFLOAT3 position{};
    XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    XMFLOAT3 velocity{};
    // Last pose that was resting on the terrain. A body that ends up below the
    // world (off the edge of the physics heightfield) is put back here.
    XMFLOAT3 safePosition{};
    XMFLOAT4 safeRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    // Hull-relative traverse, radians; 0 looks down the hull's +X.
    float turretYaw = 0.0f;
    float health = 1500.0f;
    float maxHealth = 1500.0f;
    bool dead = false;
    float reload = 2.0f;
    // Seconds the gun has held on target; it fires only after a short settle,
    // so a turret swinging onto the player is the warning.
    float aimSettle = 0.0f;
    // Last frame's gun and drive solution, for the trace.
    float yawError = 0.0f;
    float headingError = 0.0f;
    bool lineOfFire = false;
    XMFLOAT3 lastMuzzle{}, lastAim{}, lastBlocked{};
    float stuckTime = 0.0f;
    float reverseTime = 0.0f;
    float reverseSteer = 1.0f;
    float wreckTime = 0.0f;
    float smokeCooldown = 0.0f;
    // Authored behaviour.
    float detectRange = 140.0f;
    float fireRange = 110.0f;
    float standoff = 35.0f;
    float reloadSeconds = 6.0f;
    float shellSpeed = 45.0f;
    // Scales the shell's blast on the player and its crater, 1 = main gun.
    float shellDamage = 1.0f;
    float turretRate = 0.55f;   // rad/s
    // Multiplayer. The host drives every tank; a client draws the host's pose
    // and runs no physics body or AI of its own. `killer` is the player the
    // host credits with the wreck (net::kInvalidPlayerId for nobody).
    net::PlayerId killer = net::kInvalidPlayerId;
    // Client-side: the host's latest pose, eased toward each frame so a tank
    // does not step at the net tick rate.
    XMFLOAT3 netPosition{};
    XMFLOAT4 netRotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    float netTurretYaw = 0.0f;
    // Whether any armor state has named this tank yet. A tank that is already
    // a wreck in the first one is laid down silently: it died before this
    // machine joined, and the explosion belongs to that moment, not this one.
    bool netSeen = false;
};
static std::vector<EnemyTankState> g_enemyTanks;

// A placement seen by the prefab rebuild before the physics world exists. On a
// level start the rebuild runs while DestructionDX12 is still initializing on
// its loader thread -- measured, the first rebuild of a level registered no
// tanks at all -- so the placement waits here and registers once the world is
// up (RegisterPendingEnemyTanks).
struct PendingEnemyTank {
    uint64_t entityId = 0;
    std::string prefabId;
    nlohmann::json settings;
    XMFLOAT4X4 world{};
    XMFLOAT3 boundsMinimum{};
    XMFLOAT3 boundsMaximum{};
};
static std::vector<PendingEnemyTank> g_pendingEnemyTanks;

static void ReleaseEnemyTanks() {
    for (const EnemyTankState& tank : g_enemyTanks)
        g_destruction.DestroyGroundVehicle(tank.physicsHandle);
    g_enemyTanks.clear();
    g_pendingEnemyTanks.clear();
}

static void DropEnemyTank(uint64_t entityId) {
    for (size_t i = g_enemyTanks.size(); i-- > 0;) {
        if (g_enemyTanks[i].entityId != entityId) continue;
        g_destruction.DestroyGroundVehicle(g_enemyTanks[i].physicsHandle);
        g_enemyTanks.erase(g_enemyTanks.begin() + i);
    }
    g_pendingEnemyTanks.erase(
        std::remove_if(g_pendingEnemyTanks.begin(), g_pendingEnemyTanks.end(),
            [entityId](const PendingEnemyTank& pending) {
                return pending.entityId == entityId;
            }),
        g_pendingEnemyTanks.end());
}

static float JsonFloat(const nlohmann::json& object, const char* key,
                       float fallback) {
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<float>() : fallback;
}

// The chassis is sized from the hull's measured bounds, so any wheeled or
// tracked model gets a body its own size rather than the Humvee's. Wheels sit
// on the bounds floor, three axles along the hull with the rear counter-
// steering, which lets an 8 m hull turn inside its own length.
static GroundVehicleSpec EnemyTankSpecFromBounds(const XMFLOAT3& minimum,
                                                 const XMFLOAT3& maximum,
                                                 XMFLOAT3& chassisCenter) {
    GroundVehicleSpec spec;
    const float halfX = (maximum.x - minimum.x) * 0.5f;
    const float halfZ = (maximum.z - minimum.z) * 0.5f;
    const float height = maximum.y - minimum.y;
    const float radius = (std::max)(0.2f, (std::min)(0.55f, height * 0.3f));
    // The box rides a wheel diameter clear of the ground and ends just past
    // the outer axles. A full-length box with the model's own clearance has a
    // ~13 degree approach angle: measured, it grounded its nose on the first
    // beach bank and sat there. The hit box the player shoots at is the
    // collider, not this, so the body can be shorter than the hull it drives.
    const float boxBottom = minimum.y + radius * 1.6f;
    const float halfY = (std::max)(0.15f, (maximum.y - boxBottom) * 0.5f);
    chassisCenter = { (minimum.x + maximum.x) * 0.5f, boxBottom + halfY,
                      (minimum.z + maximum.z) * 0.5f };
    const float axleX = (std::max)(0.5f, halfX - radius * 2.2f);
    spec.chassisHalfExtents = { (std::min)(halfX * 0.95f, axleX + radius),
                                halfY, halfZ * 0.9f };
    spec.wheelRadius = radius;
    const float wheelY = minimum.y + radius - chassisCenter.y;
    const float trackZ = halfZ * 0.8f;
    const float axles[3] = { axleX, 0.0f, -axleX };
    const float steer[3] = { 1.0f, 0.0f, -1.0f };
    spec.wheelCount = 6;
    for (uint32_t axle = 0; axle < 3; ++axle) {
        for (uint32_t side = 0; side < 2; ++side) {
            const uint32_t i = axle * 2 + side;
            spec.wheelOffsets[i] = { axles[axle], wheelY,
                                     side == 0 ? trackZ : -trackZ };
            spec.wheelSteer[i] = steer[axle];
        }
    }
    // Same chassis-to-wheel mass ratio as the Humvee (~35:1), which is what
    // keeps the wheel joints stable; heavier wheels scale the torque with them.
    spec.chassisDensity = 110.0f;
    spec.wheelDensity = 170.0f;
    spec.maxSteerAngle = 0.45f;
    spec.steeringTorque = 2400.0f;
    // ~7 m/s flat out: a tank the player can outrun and outflank on foot.
    spec.maxWheelSpin = 7.0f / radius;
    // Enough to climb a 30% bank: 6 wheels x 1500 N m / 0.53 m = 17 kN against
    // the ~13 kN a 4.6 t hull needs on that grade.
    spec.driveTorque = 1500.0f;
    spec.idleTorque = 260.0f;
    spec.brakeTorque = 2600.0f;
    return spec;
}

// Registers one placement. Called from the prefab rebuild, which has already
// emitted the hull and turret render batches and the hull's box collider.
static void RegisterEnemyTank(uint64_t entityId, const PrefabAsset& prefab,
                              const nlohmann::json& settings,
                              const XMMATRIX& world,
                              const XMFLOAT3& boundsMinimum,
                              const XMFLOAT3& boundsMaximum) {
    if (!g_destruction.IsInitialized()) {
        PendingEnemyTank pending;
        pending.entityId = entityId;
        pending.prefabId = prefab.id;
        pending.settings = settings;
        XMStoreFloat4x4(&pending.world, world);
        pending.boundsMinimum = boundsMinimum;
        pending.boundsMaximum = boundsMaximum;
        g_pendingEnemyTanks.push_back(std::move(pending));
        return;
    }
    EnemyTankState tank;
    tank.entityId = entityId;
    tank.hullPrefabId = prefab.id;
    tank.turretPrefabId = settings.value("turret", std::string());
    tank.maxHealth = tank.health =
        (std::max)(1.0f, JsonFloat(settings, "health", tank.maxHealth));
    tank.detectRange = JsonFloat(settings, "detectRange", tank.detectRange);
    tank.fireRange = JsonFloat(settings, "fireRange", tank.fireRange);
    tank.standoff = JsonFloat(settings, "standoff", tank.standoff);
    tank.reloadSeconds =
        (std::max)(0.5f, JsonFloat(settings, "reload", tank.reloadSeconds));
    tank.shellSpeed =
        (std::max)(5.0f, JsonFloat(settings, "shellSpeed", tank.shellSpeed));
    tank.shellDamage = (std::max)(0.0f,
        JsonFloat(settings, "shellDamage", tank.shellDamage));
    tank.turretRate = JsonFloat(settings, "turretRate", tank.turretRate);
    const auto muzzle = settings.find("muzzle");
    if (muzzle != settings.end() && muzzle->is_array() && muzzle->size() == 3)
        tank.muzzleLocal = { (*muzzle)[0].get<float>(),
                             (*muzzle)[1].get<float>(),
                             (*muzzle)[2].get<float>() };

    XMStoreFloat4x4(&tank.turretLocal, XMMatrixIdentity());
    for (const PrefabChildAsset& child : prefab.children) {
        if (child.prefabId != tank.turretPrefabId) continue;
        XMStoreFloat4x4(&tank.turretLocal,
            XMMatrixScaling(child.scale[0], child.scale[1], child.scale[2]) *
            EulerDegreesToMatrix(child.rotation));
        tank.turretPivot = { child.position[0], child.position[1],
                             child.position[2] };
        break;
    }

    const float scaleX = XMVectorGetX(XMVector3Length(world.r[0]));
    const float scaleY = XMVectorGetX(XMVector3Length(world.r[1]));
    const float scaleZ = XMVectorGetX(XMVector3Length(world.r[2]));
    XMStoreFloat4x4(&tank.scaleTransform,
                    XMMatrixScaling(scaleX, scaleY, scaleZ));
    const XMFLOAT3 scaledMinimum{ boundsMinimum.x * scaleX,
        boundsMinimum.y * scaleY, boundsMinimum.z * scaleZ };
    const XMFLOAT3 scaledMaximum{ boundsMaximum.x * scaleX,
        boundsMaximum.y * scaleY, boundsMaximum.z * scaleZ };
    tank.spec = EnemyTankSpecFromBounds(scaledMinimum, scaledMaximum,
                                        tank.chassisOffset);

    // The hull collider was sized from the hull bounds alone, which stop at
    // the deck; stretch it up over the turret so rockets aimed at the turret
    // meet armour instead of flying through it.
    const float hitTop = (std::max)(boundsMaximum.y,
        JsonFloat(settings, "hitHeight", boundsMaximum.y));
    tank.boxCenterLocal = { (boundsMinimum.x + boundsMaximum.x) * 0.5f,
                            (boundsMinimum.y + hitTop) * 0.5f,
                            (boundsMinimum.z + boundsMaximum.z) * 0.5f };
    for (PrefabCollider& collider : g_prefabColliders) {
        if (collider.entityId != entityId || collider.prefabId != prefab.id)
            continue;
        collider.halfExtents.y = (hitTop - boundsMinimum.y) * 0.5f * scaleY;
    }

    // Yaw only, like every prefab collider: placements are authored upright.
    XMFLOAT3 worldX;
    XMStoreFloat3(&worldX, XMVector3TransformNormal(
        XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), world));
    const float yaw = std::atan2(-worldX.z, worldX.x);
    const XMMATRIX yawMatrix = XMMatrixRotationY(yaw);
    XMFLOAT3 origin;
    XMStoreFloat3(&origin, XMVector3TransformCoord(XMVectorZero(), world));
    // Rest the tracks on the terrain rather than trusting the gizmo height:
    // a hull authored into a slope would start inside the heightfield.
    origin.y = (std::max)(origin.y,
        GroundHeightAt(origin.x, origin.z) - scaledMinimum.y);
    XMVECTOR center = XMVector3TransformNormal(
        XMLoadFloat3(&tank.chassisOffset), yawMatrix) + XMLoadFloat3(&origin);
    center = XMVectorAdd(center, XMVectorSet(0.0f, 0.15f, 0.0f, 0.0f));
    XMStoreFloat3(&tank.position, center);
    XMStoreFloat4(&tank.rotation, XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f));
    tank.safePosition = tank.position;
    tank.safeRotation = tank.rotation;
    tank.netPosition = tank.position;
    tank.netRotation = tank.rotation;
    // A client drives nothing: the host's solver owns where the tank is, and a
    // second body here would roll off on its own and shove the player about.
    const bool hostOwned = ClientOwnedByHost();
    if (!hostOwned)
        tank.physicsHandle = g_destruction.CreateGroundVehicle(
            tank.spec, tank.position, yaw);
    if (tank.physicsHandle == 0 && !hostOwned) {
        SGE_LOG("LogGameplay", EngineLog::Level::Warning,
            "Enemy tank " + prefab.id + " declined: no physics body");
        return;
    }
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Enemy tank " + prefab.id + " at " + std::to_string(origin.x) + ", " +
        std::to_string(origin.y) + ", " + std::to_string(origin.z) +
        ", chassis " + std::to_string(tank.spec.chassisHalfExtents.x * 2.0f) +
        " x " + std::to_string(tank.spec.chassisHalfExtents.y * 2.0f) +
        " x " + std::to_string(tank.spec.chassisHalfExtents.z * 2.0f) +
        " m, wheel r " + std::to_string(tank.spec.wheelRadius) +
        ", turret " + (tank.turretPrefabId.empty() ? "none"
                                                   : tank.turretPrefabId));
    g_enemyTanks.push_back(std::move(tank));
}

static void RegisterPendingEnemyTanks() {
    if (g_pendingEnemyTanks.empty() || !g_destruction.IsInitialized()) return;
    std::vector<PendingEnemyTank> pending;
    pending.swap(g_pendingEnemyTanks);
    for (const PendingEnemyTank& placement : pending) {
        const PrefabAsset* prefab = g_prefabRegistry.Find(placement.prefabId);
        if (!prefab) continue;
        RegisterEnemyTank(placement.entityId, *prefab, placement.settings,
                          XMLoadFloat4x4(&placement.world),
                          placement.boundsMinimum, placement.boundsMaximum);
    }
}

// The hull's model-to-world transform at the tank's current pose.
static XMMATRIX EnemyTankHullWorld(const EnemyTankState& tank) {
    const XMMATRIX orientation =
        XMMatrixRotationQuaternion(XMLoadFloat4(&tank.rotation));
    const XMVECTOR offset = XMVector3TransformNormal(
        XMLoadFloat3(&tank.chassisOffset), orientation);
    const XMVECTOR origin = XMLoadFloat3(&tank.position) - offset;
    return XMLoadFloat4x4(&tank.scaleTransform) * orientation *
           XMMatrixTranslationFromVector(origin);
}

// Unscaled hull model space: the turret child's local space is authored there,
// and the placement scale is applied once on top by the hull world.
static XMMATRIX EnemyTankTurretWorld(const EnemyTankState& tank,
                                     const XMMATRIX& hullWorld) {
    return XMLoadFloat4x4(&tank.turretLocal) *
           XMMatrixRotationY(tank.turretYaw) *
           XMMatrixTranslation(tank.turretPivot.x, tank.turretPivot.y,
                               tank.turretPivot.z) *
           hullWorld;
}

// Every non-hostile explosion. Measured to the hull box's surface, so a rocket
// that detonated on the glacis counts as a direct hit however far the prefab
// origin is from it.
static void RouteEnemyTankDamage(EnemyTankState& tank, float damage,
                                 const XMFLOAT3& hit, bool fromPlayer,
                                 bool hostAuthored);

// `hostAuthored`: a blast this machine is replaying from the host (a
// replicated grenade), which the host has already applied to every tank.
static void DamageEnemyTanksFromBlast(const XMFLOAT3& center, float reach,
                                      bool remoteCharge, bool rocket,
                                      bool missile, float fragDamage,
                                      bool fromPlayer, bool hostAuthored) {
    for (EnemyTankState& tank : g_enemyTanks) {
        if (tank.dead) continue;
        const auto collider = std::find_if(g_prefabColliders.begin(),
            g_prefabColliders.end(), [&](const PrefabCollider& value) {
                return value.entityId == tank.entityId &&
                       value.prefabId == tank.hullPrefabId;
            });
        if (collider == g_prefabColliders.end()) continue;
        const XMFLOAT3 local = PrefabColliderToLocal(*collider, center);
        const float outX = (std::max)(0.0f,
            std::abs(local.x) - collider->halfExtents.x);
        const float outY = (std::max)(0.0f,
            std::abs(local.y) - collider->halfExtents.y);
        const float outZ = (std::max)(0.0f,
            std::abs(local.z) - collider->halfExtents.z);
        const float surface = std::sqrt(outX * outX + outY * outY + outZ * outZ);
        if (surface >= reach) continue;
        const float falloff = 1.0f - surface / reach;
        float damage = 0.0f;
        if (remoteCharge) {
            // A charge stuck to the hull is a kill; one nearby still guts it.
            damage = surface < 1.5f ? tank.maxHealth
                                    : tank.maxHealth * 0.6f * falloff;
        } else if (rocket) {
            damage = kEnemyTankRocketDamage * (surface < 1.0f ? 1.0f : falloff);
        } else if (missile) {
            damage = tank.maxHealth * 0.6f * falloff;
        } else {
            // Frag: scratches the paint. Enough to confirm a hit, never a kill
            // strategy.
            damage = fragDamage * 0.2f * falloff;
        }
        RouteEnemyTankDamage(tank, damage, center, fromPlayer, hostAuthored);
    }
}

// The wreck: the explosion, the crater, the payout and the entity switched off.
// Runs on the machine that killed the tank and, in a session, on every client
// when the host's armor state first says it is dead. `localKill` gates the
// payout only; `hostAuthored` is a client replaying the host's kill, whose
// crater the host has already cut and sent.
static void WreckEnemyTank(EnemyTankState& tank, bool localKill,
                           bool hostAuthored) {
    tank.dead = true;
    tank.health = 0.0f;
    tank.wreckTime = 0.0f;
    g_destruction.SetGroundVehicleInput(tank.physicsHandle, 0.0f, 0.0f, true);
    XMFLOAT3 center;
    XMStoreFloat3(&center, XMVector3TransformCoord(
        XMLoadFloat3(&tank.boxCenterLocal), EnemyTankHullWorld(tank)));
    scene.SpawnExplosionFX(center, 9.0f, 1.2f);
    scene.SpawnSmokeBurst(center, 3.0f, 2.6f);
    AddExplosionTerrainCrater(center, 1.0f, hostAuthored);
    if (g_destruction.IsInitialized()) {
        g_destruction.ApplyExplosion(center, 7.0f, 60.0f, 14.0f);
        g_destruction.ApplyRagdollExplosion(center, 7.0f, 110.0f);
    }
    g_pendingExplosionAudio.push_back({ 0.0f, 1.0f, 0.7f, false });
    if (localKill) CreditPlayerDestruction();
    if (g_game.session.TimerRunning()) {
        g_game.mission.RecordDestruction();
        if (localKill) AwardCombatEvent(MoneyEvent::PropDestroyed);
    }
    // The wreck keeps drawing out of the live batches. Disabling the entity
    // means a later prefab rebuild leaves it gone rather than resurrecting a
    // full-health tank where it was placed.
    for (LevelEntity& entity : g_game.world.Level().entities)
        if (entity.id == tank.entityId) entity.enabled = false;
    SGE_LOG("LogGameplay", EngineLog::Level::Display, "Enemy tank destroyed");
}

// Authoritative damage: offline, or on the host. `killer` is who the host
// credits in a session; offline it goes unused.
static void DamageEnemyTank(EnemyTankState& tank, float damage,
                            const XMFLOAT3& hit, bool fromPlayer,
                            net::PlayerId killer) {
    if (tank.dead || damage <= 0.0f) return;
    tank.health -= damage;
    PlayMetalHitAudio(hit, 0.8f);
    scene.SpawnSmokeBurst(hit, 0.6f, 0.35f);
    const bool destroyed = tank.health <= 0.0f;
    if (fromPlayer) scene.TriggerHitMarker(destroyed);
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Enemy tank hit for " + std::to_string(damage) + ", health " +
        std::to_string((std::max)(0.0f, tank.health)) + "/" +
        std::to_string(tank.maxHealth));
    if (!destroyed) return;
    tank.killer = killer;
    WreckEnemyTank(tank, fromPlayer, /*hostAuthored=*/false);
}

// Every explosive hit on a tank goes through here. In a session the host owns
// the tank's health: a client reports only its own player's explosives and
// leaves everything else -- a host grenade it is replaying, an enemy blast --
// to the host, which sees those itself. The host applies directly.
static void RouteEnemyTankDamage(EnemyTankState& tank, float damage,
                                 const XMFLOAT3& hit, bool fromPlayer,
                                 bool hostAuthored) {
    if (tank.dead || damage <= 0.0f) return;
    if (!g_netSession.Active()) {
        DamageEnemyTank(tank, damage, hit, fromPlayer, net::kInvalidPlayerId);
        return;
    }
    if (g_netSession.CurrentRole() == net::Role::Host) {
        DamageEnemyTank(tank, damage, hit, fromPlayer,
                        fromPlayer ? g_netSession.LocalId()
                                   : net::kInvalidPlayerId);
        return;
    }
    if (!fromPlayer || hostAuthored) return;
    // The shooter sees its hit now; the outcome arrives in the armor state.
    PlayMetalHitAudio(hit, 0.8f);
    scene.SpawnSmokeBurst(hit, 0.6f, 0.35f);
    scene.TriggerHitMarker(false);
    g_netSession.ReportWorldImpact(tank.entityId, damage, hit.x, hit.y, hit.z,
                                   /*kind=*/4);
}

// Host-side: a client's reported hit, drained from the world-impact queue.
static void ApplyReportedEnemyTankDamage(uint64_t entityId, float damage,
                                         const XMFLOAT3& hit,
                                         net::PlayerId shooter) {
    for (EnemyTankState& tank : g_enemyTanks) {
        if (tank.entityId != entityId) continue;
        DamageEnemyTank(tank, damage, hit,
                        shooter == g_netSession.LocalId(), shooter);
        return;
    }
}

// A hostile shell meeting the player's body. The rocket sweep checks enemies,
// aircraft and the world, but never the player it is fired at, so a shell
// that lined up perfectly would otherwise pass through and burst behind them.
static bool HostileShellHitsPlayer(const XMFLOAT3& start, const XMFLOAT3& end,
                                   float radius, XMFLOAT3& hit) {
    if (scene.player.health <= 0.0f) return false;
    const XMFLOAT3 eye = scene.camera.Position;
    const XMVECTOR top = XMVectorSet(eye.x, eye.y, eye.z, 0.0f);
    const XMVECTOR bottom = XMVectorSet(
        eye.x, eye.y - scene.camera.PlayerHeight, eye.z, 0.0f);
    const XMVECTOR a = XMLoadFloat3(&start);
    const XMVECTOR b = XMLoadFloat3(&end);
    // Closest points between the shell segment and the body's axis.
    const XMVECTOR d1 = b - a, d2 = top - bottom, r = a - bottom;
    const float aa = XMVectorGetX(XMVector3Dot(d1, d1));
    const float ee = XMVectorGetX(XMVector3Dot(d2, d2));
    const float ff = XMVectorGetX(XMVector3Dot(d2, r));
    if (aa < 1e-8f) return false;
    const float cc = XMVectorGetX(XMVector3Dot(d1, r));
    const float bb = XMVectorGetX(XMVector3Dot(d1, d2));
    const float denom = aa * ee - bb * bb;
    float s = denom > 1e-8f
        ? (std::max)(0.0f, (std::min)(1.0f, (bb * ff - cc * ee) / denom))
        : 0.0f;
    float t = (bb * s + ff) / ee;
    if (t < 0.0f || t > 1.0f) {
        t = (std::max)(0.0f, (std::min)(1.0f, t));
        s = (std::max)(0.0f, (std::min)(1.0f, (t * bb - cc) / aa));
    }
    const XMVECTOR onShell = a + d1 * s;
    const XMVECTOR onBody = bottom + d2 * t;
    constexpr float kBodyRadius = 0.4f;
    const float reach = kBodyRadius + radius;
    if (XMVectorGetX(XMVector3LengthSq(onShell - onBody)) > reach * reach)
        return false;
    XMStoreFloat3(&hit, onShell);
    return true;
}

// The shell and its report. `hostReplica` is a client flying the host's round:
// its blast is the host's, so the crater it would dig is already on the way
// as a replicated terrain cut.
// `start` is where the round begins its sweep, which can be ahead of the
// muzzle the flash comes from (see EnemyTankShellStart).
static void SpawnEnemyTankShell(const XMFLOAT3& muzzle, const XMFLOAT3& start,
                                const XMFLOAT3& direction, float speed,
                                float lifetime, float damageScale,
                                bool hostReplica) {
    Projectile shell = {};
    shell.position = shell.previousPosition = start;
    shell.direction = direction;
    shell.speed = speed;
    shell.lifetime = lifetime;
    shell.active = true;
    shell.rocket = true;
    shell.hostile = true;
    shell.netHostRound = hostReplica;
    shell.blastDamageScale = damageScale;
    scene.projectiles.push_back(shell);

    scene.SpawnExplosionFX(muzzle, 2.4f, 0.14f);
    scene.SpawnWeaponSmoke(muzzle, direction, 3.2f);
    g_rpgFireAudio.PlayAt(muzzle.x, muzzle.y, muzzle.z, 1.0f, 0.5f, 320.0f);
}

// Where a shell starts sweeping: past the firing tank's own hull box. The
// rocket sweep tests every prefab collider from the round's first frame, and
// a box test from a point inside the box hits at once. The Bradley's short
// gun leaves its muzzle inside its hull box (stretched up over the turret)
// whenever the turret is within ~37 degrees of the hull axis, so its shells
// burst on its own hull. The sweep starts at the box's far side instead.
static XMFLOAT3 EnemyTankShellStart(const EnemyTankState& tank,
                                    const XMFLOAT3& muzzle,
                                    const XMFLOAT3& direction) {
    const auto collider = std::find_if(g_prefabColliders.begin(),
        g_prefabColliders.end(), [&](const PrefabCollider& value) {
            return value.entityId == tank.entityId &&
                   value.prefabId == tank.hullPrefabId;
        });
    if (collider == g_prefabColliders.end()) return muzzle;
    const XMFLOAT3 origin = PrefabColliderToLocal(*collider, muzzle);
    const XMFLOAT3 ahead = PrefabColliderToLocal(*collider,
        { muzzle.x + direction.x, muzzle.y + direction.y,
          muzzle.z + direction.z });
    const float o[3] = { origin.x, origin.y, origin.z };
    const float d[3] = { ahead.x - origin.x, ahead.y - origin.y,
                         ahead.z - origin.z };
    // The sweep's own shell radius, plus a margin so the first step is clear.
    constexpr float kClearance = 0.22f + 0.1f;
    const float half[3] = { collider->halfExtents.x + kClearance,
                            collider->halfExtents.y + kClearance,
                            collider->halfExtents.z + kClearance };
    float entry = 0.0f;
    float exit = FLT_MAX;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (std::abs(o[axis]) > half[axis]) return muzzle;
            continue;
        }
        float t0 = (-half[axis] - o[axis]) / d[axis];
        float t1 = (half[axis] - o[axis]) / d[axis];
        if (t0 > t1) std::swap(t0, t1);
        entry = (std::max)(entry, t0);
        exit = (std::min)(exit, t1);
        if (entry > exit) return muzzle;
    }
    return { muzzle.x + direction.x * exit, muzzle.y + direction.y * exit,
             muzzle.z + direction.z * exit };
}

// The player a tank should fight: the nearest living one inside `range`. On
// the host that includes every other player's body -- a tank that only ever
// hunted whoever happened to be hosting would let everyone else walk past it.
// Returns the target's eye point, which is what the local player's camera is.
static bool NearestEnemyTankTarget(const XMFLOAT3& from, float range,
                                   XMFLOAT3& eye) {
    float bestSq = range * range;
    bool found = false;
    const bool localTargetable = scene.player.health > 0.0f &&
        !scene.player.downed && !g_insertionChoicePending &&
        !g_game.vehicles.blackHawkCarryingPlayer;
    if (localTargetable) {
        const XMFLOAT3 p = scene.camera.Position;
        const float dx = p.x - from.x, dz = p.z - from.z;
        const float dSq = dx * dx + dz * dz;
        if (dSq <= bestSq) { bestSq = dSq; eye = p; found = true; }
    }
    if (!g_netSession.Active() ||
        g_netSession.CurrentRole() != net::Role::Host) return found;
    for (const auto& actor : g_bandits) {
        if (!actor || !actor->networkControlled || actor->netDowned ||
            actor->netHealth <= 0.0f) continue;
        const XMFLOAT3 feet = actor->position;
        // Well clear of the ground is a player still riding their insertion
        // in, the remote equivalent of blackHawkCarryingPlayer above.
        if (feet.y - GroundHeightAt(feet.x, feet.z) > 4.0f) continue;
        const float dx = feet.x - from.x, dz = feet.z - from.z;
        const float dSq = dx * dx + dz * dz;
        if (dSq > bestSq) continue;
        bestSq = dSq;
        eye = { feet.x, feet.y + scene.camera.PlayerHeight, feet.z };
        found = true;
    }
    return found;
}

static void FireEnemyTankShell(EnemyTankState& tank, const XMMATRIX& turretWorld,
                               const XMFLOAT3& target) {
    XMFLOAT3 muzzle;
    XMStoreFloat3(&muzzle, XMVector3TransformCoord(
        XMLoadFloat3(&tank.muzzleLocal), turretWorld));
    XMVECTOR direction = XMLoadFloat3(&target) - XMLoadFloat3(&muzzle);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 1.0f) return;
    direction = XMVector3Normalize(direction);
    // Aimed where the player is, not where they are going, plus a little
    // scatter: at 45 m/s the round is a couple of seconds out at range, and
    // moving is the answer to it.
    const float spread = 0.012f;
    direction += XMVectorSet(
        ((float)std::rand() / RAND_MAX - 0.5f) * spread,
        ((float)std::rand() / RAND_MAX - 0.5f) * spread,
        ((float)std::rand() / RAND_MAX - 0.5f) * spread, 0.0f);
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));

    const float lifetime = tank.fireRange * 1.6f / tank.shellSpeed;
    const XMFLOAT3 start = EnemyTankShellStart(tank, muzzle, shotDirection);
    SpawnEnemyTankShell(muzzle, start, shotDirection, tank.shellSpeed,
                        lifetime, tank.shellDamage, /*hostReplica=*/false);
    // Every client flies the same shell. Each one can only hurt its own
    // player, which is what the host already trusts a client to report.
    // Sent from the cleared start: a client's copy of the hull box is where
    // the host's tank was, so it would stop the round the same way.
    if (g_netSession.Active() &&
        g_netSession.CurrentRole() == net::Role::Host)
        g_netSession.PublishEnemyFire(net::EnemyFireKind::TankShell,
            start.x, start.y, start.z,
            shotDirection.x, shotDirection.y, shotDirection.z,
            tank.shellSpeed, lifetime, tank.shellDamage);
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Enemy tank fired from " + std::to_string(muzzle.x) + ", " +
        std::to_string(muzzle.y) + ", " + std::to_string(muzzle.z) +
        " at " + std::to_string(target.x) + ", " + std::to_string(target.y) +
        ", " + std::to_string(target.z));
}

// SGE_TANK_TRACE=1: each tank's pose, speed, turret and intent once a second.
// Driving is a physics outcome, so whether the hull is actually going where
// the AI steers is only answerable from the solver's own numbers.
static void TraceEnemyTanks(float dt) {
    static const bool enabled =
        GetEnvironmentVariableA("SGE_TANK_TRACE", nullptr, 0) > 0;
    static float timer = 0.0f;
    if (!enabled || g_enemyTanks.empty()) return;
    timer += dt;
    if (timer < 1.0f) return;
    timer = 0.0f;
    for (const EnemyTankState& tank : g_enemyTanks) {
        const float dx = scene.camera.Position.x - tank.position.x;
        const float dz = scene.camera.Position.z - tank.position.z;
        XMFLOAT3 up, forward;
        XMStoreFloat3(&up, XMVector3Rotate(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                                           XMLoadFloat4(&tank.rotation)));
        XMStoreFloat3(&forward, XMVector3Rotate(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), XMLoadFloat4(&tank.rotation)));
        const float pitchSin = forward.y;
        const float flat = (std::max)(1e-4f, std::sqrt(
            forward.x * forward.x + forward.z * forward.z));
        const float flatX = forward.x / flat, flatZ = forward.z / flat;
        SGE_LOG("LogGameplay", EngineLog::Level::Display,
            "Tank " + std::to_string(tank.entityId) +
            " pos " + std::to_string(tank.position.x) + ", " +
            std::to_string(tank.position.y) + ", " +
            std::to_string(tank.position.z) +
            " ground " + std::to_string(
                GroundHeightAt(tank.position.x, tank.position.z)) +
            " up.y " + std::to_string(up.y) +
            " speed " + std::to_string(std::sqrt(
                tank.velocity.x * tank.velocity.x +
                tank.velocity.z * tank.velocity.z)) +
            " player " + std::to_string(std::sqrt(dx * dx + dz * dz)) +
            " turret " + std::to_string(tank.turretYaw) +
            " reload " + std::to_string(tank.reload) +
            " aimErr " + std::to_string(tank.yawError) +
            " headErr " + std::to_string(tank.headingError) +
            " pitch " + std::to_string(pitchSin) +
            " ahead4 " + std::to_string(GroundHeightAt(
                tank.position.x + flatX * 4.0f, tank.position.z + flatZ * 4.0f)) +
            " settle " + std::to_string(tank.aimSettle) +
            (tank.lineOfFire ? " LOF" : " noLOF") +
            " muzzle " + std::to_string(tank.lastMuzzle.x) + "," +
            std::to_string(tank.lastMuzzle.y) + "," +
            std::to_string(tank.lastMuzzle.z) + " (ground " +
            std::to_string(GroundHeightAt(tank.lastMuzzle.x,
                                          tank.lastMuzzle.z)) + ")" +
            " aim " + std::to_string(tank.lastAim.x) + "," +
            std::to_string(tank.lastAim.y) + "," +
            std::to_string(tank.lastAim.z) +
            " blocked " + std::to_string(tank.lastBlocked.x) + "," +
            std::to_string(tank.lastBlocked.y) + "," +
            std::to_string(tank.lastBlocked.z) +
            " health " + std::to_string(tank.health) +
            (tank.dead ? " DEAD" : "") +
            (tank.reverseTime > 0.0f ? " REVERSING" : ""));
    }
}

// Drives and fights every live tank. Physics input only: the pose comes back
// from the solver and is applied by SyncEnemyTankPoses after the step.
static void UpdateEnemyTanks(float dt) {
    if (g_enemyTanks.empty() || dt <= 0.0f || IsEditorEditing()) return;
    // A client's tanks are the host's. Only the wreck smoke runs here.
    const bool hostOwned = ClientOwnedByHost();

    for (EnemyTankState& tank : g_enemyTanks) {
        if (tank.dead) {
            tank.wreckTime += dt;
            tank.smokeCooldown -= dt;
            if (tank.wreckTime < kEnemyTankWreckSmokeSeconds &&
                tank.smokeCooldown <= 0.0f) {
                tank.smokeCooldown = 0.35f;
                XMFLOAT3 center;
                XMStoreFloat3(&center, XMVector3TransformCoord(
                    XMLoadFloat3(&tank.boxCenterLocal),
                    EnemyTankHullWorld(tank)));
                scene.SpawnSmokeBurst(center, 1.4f, 0.8f);
            }
            continue;
        }
        if (hostOwned) continue;

        const XMMATRIX orientation =
            XMMatrixRotationQuaternion(XMLoadFloat4(&tank.rotation));
        XMFLOAT3 forward;
        XMStoreFloat3(&forward, XMVector3TransformNormal(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), orientation));
        XMFLOAT3 player{};
        const bool engaged = NearestEnemyTankTarget(
            tank.position, tank.detectRange, player);
        const float dx = player.x - tank.position.x;
        const float dz = player.z - tank.position.z;
        const float distance = std::sqrt(dx * dx + dz * dz);

        // ---- Turret -----------------------------------------------------
        // Traverse toward the player in the hull's own frame, at a finite rate:
        // the slewing gun is what tells the player it is about to fire.
        const XMMATRIX hullWorld = EnemyTankHullWorld(tank);
        float desiredYaw = 0.0f;
        if (engaged) {
            const XMVECTOR local = XMVector3TransformNormal(
                XMVectorSet(dx, 0.0f, dz, 0.0f), XMMatrixTranspose(orientation));
            desiredYaw = std::atan2(-XMVectorGetZ(local), XMVectorGetX(local));
        }
        float yawError = std::atan2(std::sin(desiredYaw - tank.turretYaw),
                                    std::cos(desiredYaw - tank.turretYaw));
        const float step = tank.turretRate * dt;
        tank.turretYaw += (std::max)(-step, (std::min)(step, yawError));
        yawError -= (std::max)(-step, (std::min)(step, yawError));

        // ---- Gun --------------------------------------------------------
        tank.reload = (std::max)(0.0f, tank.reload - dt);
        const bool onTarget = engaged && std::abs(yawError) < 0.035f &&
                              distance <= tank.fireRange;
        tank.aimSettle = onTarget ? tank.aimSettle + dt : 0.0f;
        tank.yawError = yawError;
        tank.lineOfFire = false;
        if (engaged) {
            const XMMATRIX turretWorld = EnemyTankTurretWorld(tank, hullWorld);
            XMFLOAT3 muzzle;
            XMStoreFloat3(&muzzle, XMVector3TransformCoord(
                XMLoadFloat3(&tank.muzzleLocal), turretWorld));
            // Chest height, and only with the terrain out of the way: a tank
            // below a ridge holds its fire rather than shelling the hill, and
            // the drive below keeps it closing until it has a shot.
            const XMFLOAT3 aim{ player.x,
                player.y - scene.camera.PlayerHeight * 0.4f, player.z };
            XMFLOAT3 blocked;
            tank.lineOfFire = !HitTerrainSegment(muzzle, aim, 0.1f, blocked);
            tank.lastMuzzle = muzzle;
            tank.lastAim = aim;
            tank.lastBlocked = tank.lineOfFire ? XMFLOAT3{} : blocked;
            if (onTarget && tank.lineOfFire && tank.reload <= 0.0f &&
                tank.aimSettle >= 0.6f) {
                FireEnemyTankShell(tank, turretWorld, aim);
                tank.reload = tank.reloadSeconds;
                tank.aimSettle = 0.0f;
            }
        }

        // ---- Drive ------------------------------------------------------
        float throttle = 0.0f;
        float steering = 0.0f;
        bool brake = !engaged;
        const float speed = std::sqrt(tank.velocity.x * tank.velocity.x +
                                      tank.velocity.z * tank.velocity.z);
        // Water is judged by depth and by trend: shallower than the fording
        // depth is always fine, and deeper is fine only while it is getting
        // shallower -- which is what lets a hull that starts in the surf climb
        // out, while one facing open sea stops instead of following the seabed.
        const float hereGround = GroundHeightAt(tank.position.x,
                                                tank.position.z);
        const auto drivable = [&](float x, float z) {
            const float ground = GroundHeightAt(x, z);
            return ground >= -kEnemyTankFordDepth || ground > hereGround + 0.3f;
        };
        const float flatForward = std::sqrt(forward.x * forward.x +
                                            forward.z * forward.z);
        const float fx = flatForward > 1e-4f ? forward.x / flatForward : 1.0f;
        const float fz = flatForward > 1e-4f ? forward.z / flatForward : 0.0f;
        if (tank.reverseTime > 0.0f) {
            // Backing off whatever stopped it, wheels turned the other way so
            // the next attempt comes at the obstacle from a new angle.
            tank.reverseTime -= dt;
            if (drivable(tank.position.x - fx * 9.0f,
                         tank.position.z - fz * 9.0f)) {
                throttle = -0.7f;
                steering = tank.reverseSteer;
                brake = false;
            } else {
                tank.reverseTime = 0.0f;
                brake = true;
            }
        } else if (engaged && (distance > tank.standoff ||
                               (!tank.lineOfFire &&
                                distance > kEnemyTankMinimumRange))) {
            const float length = (std::max)(0.001f, distance);
            const float wantX = dx / length, wantZ = dz / length;
            const float dot = fx * wantX + fz * wantZ;
            const float cross = fz * wantX - fx * wantZ;
            const float headingError = std::atan2(cross, dot);
            tank.headingError = headingError;
            steering = headingError * 1.45f;

            // A building ahead is not in the physics world, so the hull would
            // drive straight through it. Look a few metres past the bow and
            // swing away from whatever is there.
            const XMVECTOR bow = XMVector3TransformCoord(XMVectorSet(
                tank.boxCenterLocal.x + tank.spec.chassisHalfExtents.x + 0.5f,
                tank.boxCenterLocal.y, tank.boxCenterLocal.z, 1.0f), hullWorld);
            XMFLOAT3 probeStart, probeEnd, probeHit;
            XMStoreFloat3(&probeStart, bow);
            probeEnd = { probeStart.x + fx * 7.0f, probeStart.y,
                         probeStart.z + fz * 7.0f };
            uint64_t probeEntity = 0;
            if (HitPrefabColliderSegment(probeStart, probeEnd, 1.2f, probeHit,
                                         &probeEntity) &&
                probeEntity != tank.entityId)
                steering = cross >= 0.0f ? 1.0f : -1.0f;

            // Turn in place-ish when the player is behind: full lock, slow.
            throttle = std::abs(headingError) > 1.2f ? 0.45f
                : (std::min)(1.0f, 0.4f + (std::max)(0.0f,
                      distance - tank.standoff) / 30.0f);
            brake = false;
            // Never out to sea. Braking alone would leave a tank that faces
            // the water parked there for good, so it backs off with the wheels
            // turned and comes round -- a three-point turn toward the player.
            // With deep water behind as well it simply holds.
            if (!drivable(tank.position.x + fx * 9.0f,
                          tank.position.z + fz * 9.0f)) {
                throttle = 0.0f;
                brake = true;
                if (drivable(tank.position.x - fx * 9.0f,
                             tank.position.z - fz * 9.0f)) {
                    tank.reverseTime = 2.5f;
                    tank.reverseSteer = cross >= 0.0f ? -1.0f : 1.0f;
                }
            }

            // Stuck: pushing with nothing to show for it.
            if (throttle > 0.3f && speed < 0.4f) tank.stuckTime += dt;
            else tank.stuckTime = (std::max)(0.0f, tank.stuckTime - dt);
            if (tank.stuckTime > 2.5f) {
                tank.stuckTime = 0.0f;
                tank.reverseTime = 2.2f;
                tank.reverseSteer = steering >= 0.0f ? -1.0f : 1.0f;
            }
        } else {
            tank.stuckTime = 0.0f;
        }
        steering = (std::max)(-1.0f, (std::min)(1.0f, steering));
        g_destruction.SetGroundVehicleInput(tank.physicsHandle,
            throttle * kEnemyTankThrottleSign, steering, brake);
    }
    TraceEnemyTanks(dt);
}

// After the physics step: pull each tank's pose back and write it into the
// hull and turret draws and the hull collider, so what the player sees, shoots
// and walks into stay the same object.
static void SyncEnemyTankPoses(float dt) {
    if (IsEditorEditing()) return;
    RegisterPendingEnemyTanks();
    if (g_enemyTanks.empty()) return;
    const bool hostOwned = ClientOwnedByHost();
    // Eases the drawn pose toward the host's latest over ~70 ms: the armor
    // state arrives at the net tick, and snapping to it steps visibly.
    const float ease = 1.0f - std::exp(-14.0f * (std::max)(0.0f, dt));
    for (EnemyTankState& tank : g_enemyTanks) {
        if (hostOwned) {
            // A body registered before the session began (single player,
            // then joined) is dropped: the host's solver owns this tank now.
            if (tank.physicsHandle != 0) {
                g_destruction.DestroyGroundVehicle(tank.physicsHandle);
                tank.physicsHandle = 0;
            }
            XMStoreFloat3(&tank.position, XMVectorLerp(
                XMLoadFloat3(&tank.position),
                XMLoadFloat3(&tank.netPosition), ease));
            XMStoreFloat4(&tank.rotation, XMQuaternionSlerp(
                XMLoadFloat4(&tank.rotation),
                XMLoadFloat4(&tank.netRotation), ease));
            tank.turretYaw += std::atan2(
                std::sin(tank.netTurretYaw - tank.turretYaw),
                std::cos(tank.netTurretYaw - tank.turretYaw)) * ease;
        }
        DestructionBodyPose pose;
        bool haveBody = !hostOwned &&
            g_destruction.GetGroundVehiclePose(tank.physicsHandle, pose);
        if (haveBody) {
            tank.position = pose.position;
            tank.rotation = pose.rotation;
            tank.velocity = pose.linearVelocity;
            const float clearance = tank.position.y -
                GroundHeightAt(tank.position.x, tank.position.z);
            if (clearance > -1.0f && clearance < 4.0f) {
                tank.safePosition = tank.position;
                tank.safeRotation = tank.rotation;
            } else if (clearance < -5.0f) {
                // Through the world, or off the edge of the physics ground.
                SGE_LOG("LogGameplay", EngineLog::Level::Warning,
                    "Enemy tank " + std::to_string(tank.entityId) +
                    " fell out of the world; restoring last safe pose");
                g_destruction.DestroyGroundVehicle(tank.physicsHandle);
                tank.position = tank.safePosition;
                tank.rotation = tank.safeRotation;
                tank.velocity = {};
                haveBody = false;
            }
        }
        // No body: the physics world was rebuilt under it (a destruction
        // reset), or it was just pulled back from below the world. Recreate it
        // where the tank is rather than losing it.
        if (!haveBody && !hostOwned && g_destruction.IsInitialized()) {
            XMFLOAT3 axis;
            XMStoreFloat3(&axis, XMVector3Rotate(
                XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f),
                XMLoadFloat4(&tank.rotation)));
            tank.physicsHandle = g_destruction.CreateGroundVehicle(
                tank.spec, tank.position, std::atan2(-axis.z, axis.x));
            if (tank.dead)
                g_destruction.SetGroundVehicleInput(
                    tank.physicsHandle, 0.0f, 0.0f, true);
        }

        const XMMATRIX hullWorld = EnemyTankHullWorld(tank);
        const XMMATRIX turretWorld = EnemyTankTurretWorld(tank, hullWorld);
        for (PrefabRenderBatch& batch : g_prefabRenderBatches) {
            const bool hull = batch.prefabId == tank.hullPrefabId;
            const bool turret = !tank.turretPrefabId.empty() &&
                                batch.prefabId == tank.turretPrefabId;
            if (!hull && !turret) continue;
            for (size_t i = 0; i < batch.entityIds.size(); ++i)
                if (batch.entityIds[i] == tank.entityId &&
                    i < batch.transforms.size())
                    batch.transforms[i] = hull ? hullWorld : turretWorld;
        }

        XMFLOAT3 axis;
        XMStoreFloat3(&axis, XMVector3TransformNormal(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), hullWorld));
        for (PrefabCollider& collider : g_prefabColliders) {
            if (collider.entityId != tank.entityId ||
                collider.prefabId != tank.hullPrefabId) continue;
            XMStoreFloat3(&collider.center, XMVector3TransformCoord(
                XMLoadFloat3(&tank.boxCenterLocal), hullWorld));
            collider.yawRadians = std::atan2(axis.z, axis.x);
        }
    }
}
