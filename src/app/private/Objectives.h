#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Base position and height of a comm-tower entity, or false when the id is not
// one. Height comes from the prefab's authored targetSize; the level transform
// carries the base, since LoadPrefabModel grounds the mesh at local Y=0.
static bool FindCommTower(uint64_t entityId, XMFLOAT3& base) {
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (entity.id != entityId) continue;
        if (entity.type != LevelEntityType::Prefab ||
            entity.prefabId != kCommTowerPrefabId) return false;
        base = { entity.transform.position[0], entity.transform.position[1],
                 entity.transform.position[2] };
        return true;
    }
    return false;
}

// Brings the tower down: a blast at the base, a column of smoke up the mast so
// the collapse reads at distance, a crater, and a physics shove that knocks over
// whatever is standing nearby. Reuses the barrel-grade FX rather than inventing
// any new system -- there is no real fracture here, and this does not pretend
// otherwise (see the note in the summary).
static void CollapseCommTower(const XMFLOAT3& base) {
    constexpr float kTowerHeight = 26.0f;   // matches prefab targetSize
    const XMFLOAT3 mid(base.x, base.y + kTowerHeight * 0.45f, base.z);

    scene.SpawnExplosionFX(XMFLOAT3(base.x, base.y + 1.2f, base.z), 8.0f, 1.0f);
    // Dust climbing the mast: five bursts up the height, growing as they rise so
    // the top of the tower is the last thing to disappear.
    for (int step = 0; step < 5; ++step) {
        const float t = (float)step / 4.0f;
        scene.SpawnSmokeBurst(
            XMFLOAT3(base.x, base.y + kTowerHeight * t, base.z),
            1.1f + t * 1.5f, 1.2f + t * 1.1f);
    }
    AddExplosionTerrainCrater(base);
    // Cut the mast's own chunks loose so the sections fall under Blast/Box3D
    // rather than the mesh simply disappearing. The tower is drawn from these
    // chunks, not from a prefab batch (RebuildPrefabRenderBatches skips it), so
    // this -- not the prefab health hitting zero -- is what the player sees.
    if (g_destruction.IsInitialized()) {
        // The one authorised demolition: reached only after the tower's health
        // was spent by a charge planted on it. Release the whole mast at once
        // rather than walking DestroyChunkAt up it -- that call frees only the
        // single nearest chunk, so six calls left half the twelve bands standing,
        // and still protected, which meant standing forever. The sphere is
        // centred halfway up so it covers the mast from base to mast head.
        const XMFLOAT3 mastCenter(base.x, base.y + kTowerHeight * 0.5f, base.z);
        g_destruction.ReleaseProtectedChunks(mastCenter, kTowerHeight * 0.5f + 4.0f);
    }
    // Shove the surroundings: nearby structures shed chunks, ragdolls and
    // standing bandits are thrown clear.
    g_destruction.ApplyExplosion(mid, 9.0f, 4.0f, 200.0f);
    g_destruction.ApplyRagdollExplosion(mid, 9.0f, 130.0f);
    for (const auto& bandit : g_bandits)
        if (bandit) bandit->ApplyExplosion(base, 8.0f, 320.0f, 9.0f);
    g_pendingExplosionAudio.push_back({ 0.0f, 1.0f, 0.72f, false });
}

// The comm tower is a player objective: only the player may bring it down.
// Enemy rifle fire, enemy grenades, stray explosions, and spreading fire all
// reach the same prefab-damage entry points, so without this the tower could
// collapse on its own while the player was elsewhere -- and the mission would
// credit them for destruction they never caused.
//
// Every damage source funnels through the two wrappers below plus the burning-
// material tick, so gating those three covers bullets, blasts, and fire alike.
// Live comm-tower health for the HUD readout. Reports the first enabled tower on
// the level; false when there is none, so maps without one show no bar and a
// felled tower's bar disappears with it.
// Declared in EngineUI.h. The HUD needs the live wallet, and the UI header
// cannot include GameRuntime.h without dragging the whole runtime into every
// panel that draws a checkbox.
MoneySystem& PlayerMoney() { return g_game.money; }

bool CommTowerObjectiveStatus(float& health, float& maxHealth) {
    const PrefabRuntimeState& prefabs = g_game.world.Prefabs();
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (!entity.enabled || entity.type != LevelEntityType::Prefab ||
            entity.prefabId != kCommTowerPrefabId) continue;
        // The authored value is the maximum; the runtime map holds what is left
        // and is only populated once the tower has actually taken a hit.
        const auto definition = std::find_if(
            prefabs.destructibles.begin(), prefabs.destructibles.end(),
            [&entity](const PrefabDestructibleInstance& value) {
                return value.entityId == entity.id;
            });
        if (definition == prefabs.destructibles.end()) continue;
        maxHealth = definition->health;
        const auto live = prefabs.health.find(entity.id);
        health = live != prefabs.health.end() ? live->second : definition->health;
        health = (std::max)(0.0f, health);
        return true;
    }
    return false;
}

// Where one aircraft is being drawn this frame.
//
// A crashing or landed airframe carries its own position; one still on its
// takeoff run is re-derived from the same integration UpdateObjectivePlanes
// uses, so this never disagrees with what the player sees.
//
// Shared by the HUD marker and the explosive hit test -- a blast has to be
// measured against where the plane actually is, not where it was authored,
// or a charge thrown at a taxiing aircraft would test against the apron.
static XMFLOAT3 ObjectivePlaneLivePosition(const ObjectivePlaneState& plane) {
    if (plane.crashing || plane.crashed) return plane.crashPosition;
    const float t = plane.takeoffTimer;
    constexpr float kRotateAt = 4.0f;
    const float distance = t <= kRotateAt
        ? 0.5f * kObjectivePlaneTaxiSpeed * (t * t) / kRotateAt
        : 0.5f * kObjectivePlaneTaxiSpeed * kRotateAt +
          kObjectivePlaneTaxiSpeed * (t - kRotateAt);
    const float airborne = (std::max)(0.0f, t - kRotateAt);
    const XMVECTOR travel =
        XMVectorScale(XMLoadFloat3(&plane.forward), distance);
    return { plane.basePosition.x + XMVectorGetX(travel),
             plane.basePosition.y + kObjectivePlaneClimbRate * airborne,
             plane.basePosition.z + XMVectorGetZ(travel) };
}

// Live aircraft-objective state for the HUD marker.
//
// Reports the first plane still worth marking -- one that has neither escaped
// nor already hit the ground -- and where it is right now. Unlike the tower's
// fixed corner block this has to be a world-space marker, because the aircraft
// moves: the position returned is the airframe's current draw position, taken
// from the same crash/flight state that drives its transform, so the marker
// tracks it down the runway and through the crash.
//
// False when there is nothing to mark, so a level without an aircraft, and a
// run where it is already down, draw no marker.
bool ObjectivePlaneStatus(XMFLOAT3& position, float& health, float& maxHealth,
                          bool& down) {
    const PrefabRuntimeState& prefabs = g_game.world.Prefabs();
    for (const ObjectivePlaneState& plane : g_objectivePlanes) {
        if (plane.escaped) continue;
        const auto definition = std::find_if(
            prefabs.destructibles.begin(), prefabs.destructibles.end(),
            [&plane](const PrefabDestructibleInstance& value) {
                return value.entityId == plane.entityId;
            });
        if (definition == prefabs.destructibles.end()) continue;
        maxHealth = definition->health;
        const auto live = prefabs.health.find(plane.entityId);
        health = live != prefabs.health.end() ? live->second : definition->health;
        health = (std::max)(0.0f, health);
        down = plane.crashing || plane.crashed;
        position = ObjectivePlaneLivePosition(plane);
        return true;
    }
    return false;
}

// Comm towers standing on the level right now. The deployment briefing reads it
// to state the objective, and the run arms the mission counter from it -- so a
// map that authors two towers grades against two without anything being hardcoded.
static uint32_t CountStandingCommTowers() {
    uint32_t count = 0;
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (entity.enabled && entity.type == LevelEntityType::Prefab &&
            entity.prefabId == kCommTowerPrefabId)
            ++count;
    }
    return count;
}

// Stands the AA gun up beside the first comm tower on the level: the emplacement
// is there to defend the relay, so it belongs with it rather than at some
// unrelated authored point. Levels without a tower get no gun.
//
// Offset far enough from the mast that the tower's collapse radius does not
// swallow it, but inside the range where flying at the tower means flying at
// the gun -- which is the whole tactical point of it being here.
static void PlaceAATurretNearCommTower() {
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (!entity.enabled || entity.type != LevelEntityType::Prefab ||
            entity.prefabId != kCommTowerPrefabId) continue;
        constexpr float kOffset = 14.0f;
        const float x = entity.transform.position[0] + kOffset;
        const float z = entity.transform.position[2] + kOffset;
        float groundY = entity.transform.position[1];
        if (scene.useMeshTerrain && g_terrain.supported) {
            auto params = CurrentTerrainParams();
            params.heightScale = scene.terrainHeightScale;
            groundY = (std::max)(0.0f,
                TerrainRendererDX12::HeightAt(params, x, z));
        }
        g_game.vehicles.PlaceAATurret(XMFLOAT3(x, groundY, z));
        SGE_LOG("LogGameplay", EngineLog::Level::Display,
            "AA turret emplaced at " + std::to_string(x) + ", " +
            std::to_string(groundY) + ", " + std::to_string(z));
        return;
    }
}

// Picks what the AA gun shoots at and feeds the mount. Aircraft first -- it is
// an anti-air gun, and the inbound BlackHawk is the threat it exists to answer
// -- then the player, but only while they are airborne. A player on foot is not
// a target: walking up to the emplacement is how it is meant to be silenced.
//
// Runs one emplacement: target selection, slew and firing. Each turret solves
// independently, so several can engage the same helicopter or split between it
// and an airborne player.
static void UpdateOneAATurret(size_t turretIndex, float deltaTime) {
    VehicleSystem& vehicles = g_game.vehicles;
    if (turretIndex >= vehicles.aaTurrets.size()) return;
    VehicleSystem::AATurret& emplacement = vehicles.aaTurrets[turretIndex];
    if (!emplacement.Active()) return;

    XMFLOAT3 target{};
    XMFLOAT3 velocity{};
    bool hasTarget = false;
    bool targetIsAircraft = false;

    const XMFLOAT3 turret = emplacement.position;
    const auto rangeSq = [&turret](const XMFLOAT3& p) {
        const float dx = p.x - turret.x, dy = p.y - turret.y, dz = p.z - turret.z;
        return dx * dx + dy * dy + dz * dz;
    };

    // The insertion BlackHawk, while it is still flying.
    if (vehicles.BlackHawkIsFlying()) {
        const XMFLOAT3& p = vehicles.blackHawkPosition;
        if (rangeSq(p) <= VehicleSystem::AATurretAirRange *
                          VehicleSystem::AATurretAirRange) {
            target = p;
            velocity = vehicles.BlackHawkVelocity();
            hasTarget = true;
            targetIsAircraft = true;
        }
    }

    // Otherwise the player, but only while they are airborne: this is an
    // anti-air gun, and its barrels do not depress onto a man on foot. Walking
    // up to the emplacement is the way it is meant to be taken out, so a player
    // on the ground is deliberately safe from it.
    //
    // "Airborne" is not simply !IsGrounded. A jump leaves the ground for a
    // fraction of a second, and letting that draw AA fire would mean the gun
    // engages a player who is sprinting past it on foot. What counts is being
    // carried or roped by the BlackHawk, or standing clear of the terrain by
    // more than a jump's height -- a cliff, a rooftop, a fall.
    if (!hasTarget && scene.player.health > 0.0f && !g_insertionChoicePending) {
        const XMFLOAT3& p = scene.camera.Position;
        // Camera sits PlayerHeight above whatever it stands on, so that has to
        // come off before the remainder reads as clearance above the terrain.
        const float altitude = p.y - scene.camera.PlayerHeight -
                               GroundHeightAt(p.x, p.z);
        // Swimming is never airborne, whatever the numbers say. The camera
        // clears IsGrounded in water (nothing to stand on), and altitude is
        // measured against the seabed -- so a swimmer over deep water reads as
        // high above ground and would draw fire from a gun whose whole premise
        // is that it cannot depress onto someone at surface level.
        const bool playerAirborne =
            !scene.camera.IsSwimming &&
            (vehicles.blackHawkCarryingPlayer ||
             vehicles.BlackHawkIsRappelling() ||
             (!scene.camera.IsGrounded &&
              altitude >= VehicleSystem::AATurretMinTargetAltitude));
        // Engaged out to the same 85 m it always used against the player. The
        // gun reaches much further against aircraft, but a player -- airborne
        // or not -- is a small target, and the shorter range keeps the
        // emplacement a local threat rather than one that covers the map.
        const float d2 = rangeSq(p);
        if (playerAirborne &&
            d2 <= VehicleSystem::AATurretGroundRange *
                  VehicleSystem::AATurretGroundRange &&
            d2 >= VehicleSystem::AATurretGroundMinRange *
                  VehicleSystem::AATurretGroundMinRange) {
            target = p;
            // Lead the fall/ride rather than the last position: a player under
            // canopy or on a rope is moving, and a gun that aims where they
            // were would never connect.
            //
            // Full 3D velocity. This was vertical-only because the camera
            // exposes just VerticalVelocity, which meant a player drifting
            // sideways under canopy -- or riding the BlackHawk across the
            // gun's front -- was led straight down and never sideways.
            velocity = g_playerVelocity;
            hasTarget = true;
            targetIsAircraft = true;
        }
    }

    const float shellSpeed =
        scene.projectileSpeed * VehicleSystem::AATurretShellSpeed;
    const XMFLOAT3 aim = hasTarget
        ? vehicles.AATurretLeadPoint(emplacement, target, velocity, shellSpeed)
        : XMFLOAT3{};

    const bool fired =
        vehicles.UpdateAATurret(emplacement, deltaTime, aim, hasTarget);
    if (!fired) return;

    // A shell left the barrel this frame, and it leaves along the barrel.
    //
    // Firing at the lead point instead let the shot and the model disagree: the
    // mount slews at a finite rate and is allowed to fire while still slightly
    // off (see the onTarget tolerance), so a round aimed at the solution came
    // out at an angle to the visible barrel -- most obvious as the gun swings
    // onto a new target. Deriving the direction from the turret's own yaw and
    // pitch keeps what it hits and where it points the same thing; the slew
    // limit now genuinely governs accuracy rather than only the animation.
    const XMFLOAT3 muzzle = emplacement.Muzzle();
    const float barrelHorizontal = std::cos(emplacement.pitch);
    XMVECTOR direction = XMVectorSet(
        std::sin(emplacement.yaw) * barrelHorizontal,
        std::sin(emplacement.pitch),
        std::cos(emplacement.yaw) * barrelHorizontal, 0.0f);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 0.001f) return;
    // Dispersion, so a burst walks around the target instead of stacking four
    // rounds on the same point. Wider against aircraft, where the lead solution
    // is already approximate and pinpoint accuracy would be unsurvivable.
    const float spread = targetIsAircraft ? 0.020f : 0.011f;
    const float jitterX = ((float)std::rand() / RAND_MAX - 0.5f) * spread;
    const float jitterY = ((float)std::rand() / RAND_MAX - 0.5f) * spread;
    const float jitterZ = ((float)std::rand() / RAND_MAX - 0.5f) * spread;
    direction = XMVector3Normalize(direction) +
                XMVectorSet(jitterX, jitterY, jitterZ, 0.0f);
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));

    scene.SpawnHostileProjectile(muzzle, shotDirection,
                                 VehicleSystem::AATurretShellDamage,
                                 VehicleSystem::AATurretShellSpeed);
    // Muzzle flash as a small, short-lived world explosion: TriggerMuzzleFlash
    // drives the player's own viewmodel and has no world position, so it cannot
    // represent a gun firing across the map.
    scene.SpawnExplosionFX(muzzle, 0.9f, 0.09f);
    scene.SpawnWeaponSmoke(muzzle, shotDirection, 1.1f);

    g_gunAudio.PlayAt(muzzle.x, muzzle.y, muzzle.z, 0.85f,
                      0.62f + ((float)std::rand() / RAND_MAX) * 0.06f,
                      VehicleSystem::AATurretAirRange);
}

static void UpdateAATurret(float deltaTime) {
    for (size_t i = 0; i < g_game.vehicles.aaTurrets.size(); ++i)
        UpdateOneAATurret(i, deltaTime);
    // No posing here: each turret's model clone is aimed where the render
    // batches are built, from that turret's own pitch and yaw.
}

// The comm-tower entity whose mast contains `position`, or 0 for none. Used to
// route a hit on the tower's destruction chunks back to the prefab health that
// governs it, so the structure only comes apart when that health is spent.
static uint64_t CommTowerEntityAt(const XMFLOAT3& position) {
    constexpr float kTowerHeight = 26.0f;   // matches the prefab targetSize
    // Generous horizontal reach: the mast is a wide lattice at the base and the
    // hit lands on whichever leg the round struck.
    constexpr float kReach = 9.0f;
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (!entity.enabled || entity.type != LevelEntityType::Prefab ||
            entity.prefabId != kCommTowerPrefabId) continue;
        const float dx = position.x - entity.transform.position[0];
        const float dz = position.z - entity.transform.position[2];
        const float dy = position.y - entity.transform.position[1];
        if (dx * dx + dz * dz > kReach * kReach) continue;
        if (dy < -2.0f || dy > kTowerHeight + 2.0f) continue;
        return entity.id;
    }
    return 0;
}

// Comm towers that had a charge stuck to them when the detonator was pressed.
//
// This is the whole rule: a tower is invulnerable until the player walks up and
// plants a charge on it. Filtering by damage *source* was not enough -- every
// attempt leaked, because damage reaches the tower from paths that do not know
// what caused them (physics contacts, structural passes, blast radii). Requiring
// an attached charge inverts the test into something the tower itself owns, so a
// path nobody has thought of still cannot hurt it.
//
// Populated at detonation time because DetonateRemoteCharges clears
// scene.remoteCharges before the blast it queues is resolved. Cleared on level
// reset so a rigged tower does not stay demolishable across a restart.
static std::unordered_set<uint64_t> g_commTowersRiggedForDemolition;

static void MarkCommTowersRiggedForDemolition() {
    for (const RemoteCharge& charge : scene.remoteCharges) {
        const uint64_t tower = CommTowerEntityAt(charge.position);
        if (tower != 0) g_commTowersRiggedForDemolition.insert(tower);
    }
}

// Only a charge planted on the tower can damage it. Everything else -- rifle
// fire, rockets, frag grenades, enemy fire, fire, a crashing helicopter, a barrel
// chain, debris impacts -- leaves it standing, including a C4 blast that went off
// somewhere else on the map.
static bool CommTowerDamageAllowed(uint64_t entityId, bool fromRemoteCharge) {
    XMFLOAT3 ignored{};
    if (!FindCommTower(entityId, ignored)) return true;   // not a tower
    return fromRemoteCharge &&
        g_commTowersRiggedForDemolition.count(entityId) != 0;
}

// True when this entity is an aircraft objective still in play.
static bool IsObjectivePlaneEntity(uint64_t entityId) {
    return std::any_of(g_objectivePlanes.begin(), g_objectivePlanes.end(),
                       [entityId](const ObjectivePlaneState& plane) {
                           return plane.entityId == entityId;
                       });
}

// The aircraft is a player objective, exactly like the comm tower: bringing it
// down has to be the player's doing. Every damage source funnels through the
// prefab-damage entry points, so an enemy burst that happened to be walking
// across the runway, a stray grenade, or a fire could otherwise shoot down the
// plane and credit the player with a kill they never made.
static bool ObjectivePlaneDamageAllowed(uint64_t entityId, bool fromPlayer) {
    if (!IsObjectivePlaneEntity(entityId)) return true;   // not an aircraft
    return fromPlayer;
}

// Aircraft whose airframe an explosion at `center` actually touches, given a
// blast of `radius`. Returns 0 when the blast misses every plane.
//
// Exists because the generic blast path measures centre-to-entity-origin, and
// an aircraft is far too big for that to mean anything: this asset spans 29.0 m
// across the wings against a 5.4 m C4 blast, so a charge planted on a wing sat
// three times outside the test and the plane ignored a demolition charge stuck
// to it. Rockets hit the same wall -- they share the grenade blast radius.
//
// Same shape of fix the comm tower needed, and for the same reason: a prop whose
// geometry is much larger than the blast has to be tested as a volume rather
// than a point. Modelled as an upright cylinder around the live position --
// generous, but the alternative is an explosive that visibly detonates against
// the fuselage and does nothing.
static uint64_t ObjectivePlaneHitByExplosion(const XMFLOAT3& center,
                                             float radius) {
    for (const ObjectivePlaneState& plane : g_objectivePlanes) {
        // A wreck on the ground and a plane that already left are not targets.
        if (plane.escaped || plane.crashing || plane.crashed) continue;
        const XMFLOAT3 position = ObjectivePlaneLivePosition(plane);
        const float dx = center.x - position.x;
        const float dz = center.z - position.z;
        const float reach = plane.blastReachHorizontal + radius;
        if (dx * dx + dz * dz > reach * reach) continue;
        const float dy = center.y - position.y;
        if (std::abs(dy) > plane.blastReachVertical + radius) continue;
        return plane.entityId;
    }
    return 0;
}

// What one rocket takes off an aircraft: half its authored health, so two bring
// it down however the prefab is tuned. Mirrors kRocketHelicopterDamage rather
// than hardcoding a number against whatever the prefab currently authors.
static float ObjectivePlaneRocketDamage(uint64_t entityId) {
    const PrefabRuntimeState& prefabs = g_game.world.Prefabs();
    const auto definition = std::find_if(
        prefabs.destructibles.begin(), prefabs.destructibles.end(),
        [entityId](const PrefabDestructibleInstance& value) {
            return value.entityId == entityId;
        });
    // No authored destructible to read: fall back to a proportion of the
    // authored default, rather than silently doing nothing.
    if (definition == prefabs.destructibles.end()) return 1800.0f;
    return definition->health * 0.5f;
}

// Ray test against an aircraft where it actually is this frame.
//
// Needed because prefab colliders are baked from the authored placement and
// never move: the moment the plane starts its takeoff run it slides out of its
// own collider, so rounds passed straight through an airframe that was visibly
// right there. Shooting one down in the air was impossible for that reason, not
// because anything forbade it.
//
// A sphere around the live position rather than an oriented box: the airframe is
// banking and tumbling through this, and a sphere sized to the real bounds is
// both cheaper and more forgiving than chasing the pose. Slightly generous
// against a distant, fast-moving target, which is the right way to be wrong.
//
// Returns the entity and the point on the sphere the segment first crosses, so
// the impact effect lands on the hull instead of at its centre.
static uint64_t HitObjectivePlaneSegment(const XMFLOAT3& start,
                                         const XMFLOAT3& end, float radius,
                                         XMFLOAT3& hit) {
    uint64_t bestEntity = 0;
    float bestDistanceSquared = FLT_MAX;
    for (const ObjectivePlaneState& plane : g_objectivePlanes) {
        // A wreck and a plane that already left are not targets. The wreck keeps
        // its authored collider, so rounds still stop on it the normal way.
        if (plane.escaped || plane.crashing || plane.crashed) continue;
        const XMFLOAT3 center = ObjectivePlaneLivePosition(plane);
        // Horizontal reach is the wingspan half-extent; that is the radius that
        // matters for a plane presenting its planform to a shooter below.
        const float sphereRadius = plane.blastReachHorizontal + radius;

        const XMVECTOR origin = XMLoadFloat3(&start);
        const XMVECTOR segment = XMLoadFloat3(&end) - origin;
        const XMVECTOR toCenter = XMLoadFloat3(&center) - origin;
        const float segmentLengthSquared =
            XMVectorGetX(XMVector3LengthSq(segment));
        if (segmentLengthSquared < 1e-6f) continue;
        // Closest approach of the segment to the sphere centre, clamped to the
        // segment so a shot that stops short does not register a hit.
        float t = XMVectorGetX(XMVector3Dot(toCenter, segment)) /
                  segmentLengthSquared;
        t = (std::min)(1.0f, (std::max)(0.0f, t));
        const XMVECTOR closest = origin + XMVectorScale(segment, t);
        const XMVECTOR offset = closest - XMLoadFloat3(&center);
        if (XMVectorGetX(XMVector3LengthSq(offset)) >
            sphereRadius * sphereRadius) continue;

        const float distanceSquared =
            XMVectorGetX(XMVector3LengthSq(closest - origin));
        if (distanceSquared >= bestDistanceSquared) continue;
        bestDistanceSquared = distanceSquared;
        bestEntity = plane.entityId;
        // Pull the reported impact back onto the hull surface so the effect does
        // not spawn inside the fuselage.
        XMVECTOR surface = closest;
        const float offsetLength = XMVectorGetX(XMVector3Length(offset));
        if (offsetLength > 1e-4f)
            surface = XMLoadFloat3(&center) +
                      XMVectorScale(offset, sphereRadius / offsetLength);
        XMStoreFloat3(&hit, surface);
    }
    return bestEntity;
}

// Starts (or restarts) the aircraft objective's countdown and tells the mission
// how many are in play.
//
// Called from two places that must behave identically: the deployment screen in
// the normal game, and BeginEditorPlaytest. A playtest skips deployment
// entirely, so without the second call an authored aircraft would sit inert and
// a designer could never see the takeoff they placed it for.
static void ArmObjectivePlanes() {
    g_objectivePlaneEscaped = false;
    for (ObjectivePlaneState& plane : g_objectivePlanes) {
        plane.holdTimer = 0.0f;
        plane.takeoffTimer = 0.0f;
        plane.rolling = false;
        plane.escaped = false;
        plane.destroyed = false;
        plane.crashing = false;
        plane.crashed = false;
        plane.crashVelocity = { 0.0f, 0.0f, 0.0f };
        plane.crashPitch = 0.0f;
        plane.crashRoll = 0.0f;
        plane.crashYaw = 0.0f;
    }
    // An aircraft flies its own wreck down, so it has to survive its own death:
    // a destructible is normally disabled the moment it dies, and the next
    // prefab rebuild would drop the falling airframe out of the render batch.
    g_game.combat.keepEnabledOnDestroy = [](uint64_t entityId) {
        return std::any_of(g_objectivePlanes.begin(), g_objectivePlanes.end(),
                           [entityId](const ObjectivePlaneState& plane) {
                               return plane.entityId == entityId;
                           });
    };
    g_game.mission.SetObjectivePlaneCount(
        static_cast<uint32_t>(g_objectivePlanes.size()));
}

// Pushes one aircraft's world matrix into every batch instance that draws it.
//
// The batch is rebuilt whenever a prefab changes, so the entity has to be
// re-found by id each frame rather than caching an index into `transforms`.
static void WriteObjectivePlaneTransform(const ObjectivePlaneState& plane,
                                         const XMMATRIX& world) {
    for (PrefabRenderBatch& batch : g_prefabRenderBatches) {
        if (batch.prefabId != kObjectivePlanePrefabId) continue;
        for (size_t i = 0; i < batch.entityIds.size(); ++i)
            if (batch.entityIds[i] == plane.entityId &&
                i < batch.transforms.size())
                batch.transforms[i] = world;
    }
}

// Composes the tumbling wreck pose: authored scale/orientation, then the crash
// rotation, then the falling position.
static void WriteObjectivePlaneCrashTransform(const ObjectivePlaneState& plane) {
    const XMMATRIX fall =
        XMMatrixRotationRollPitchYaw(plane.crashPitch, plane.crashYaw,
                                     plane.crashRoll) *
        XMMatrixTranslation(plane.crashPosition.x, plane.crashPosition.y,
                            plane.crashPosition.z);
    XMMATRIX oriented = XMLoadFloat4x4(&plane.baseTransform);
    oriented.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    WriteObjectivePlaneTransform(plane, oriented * fall);
}

// Takes an aircraft out of its takeoff and drops it, seeded with the position
// and velocity it was carrying so the wreck continues along its flight path
// rather than stopping dead in the air and falling straight down.
static void BeginObjectivePlaneCrash(ObjectivePlaneState& plane) {
    if (plane.crashing || plane.crashed) return;
    plane.destroyed = true;
    plane.crashing = true;

    // Re-derive where the takeoff had it, using the same integration
    // UpdateObjectivePlanes runs, so the wreck starts exactly where the
    // airframe was being drawn on the frame it was hit.
    const float t = plane.takeoffTimer;
    constexpr float kRotateAt = 4.0f;
    const float distance = t <= kRotateAt
        ? 0.5f * kObjectivePlaneTaxiSpeed * (t * t) / kRotateAt
        : 0.5f * kObjectivePlaneTaxiSpeed * kRotateAt +
          kObjectivePlaneTaxiSpeed * (t - kRotateAt);
    const float airborne = (std::max)(0.0f, t - kRotateAt);
    const XMVECTOR heading = XMLoadFloat3(&plane.forward);
    const XMVECTOR travel = XMVectorScale(heading, distance);
    plane.crashPosition = {
        plane.basePosition.x + XMVectorGetX(travel),
        plane.basePosition.y + kObjectivePlaneClimbRate * airborne,
        plane.basePosition.z + XMVectorGetZ(travel) };

    // Speed along the runway is the derivative of that same distance curve;
    // vertical speed is the climb rate only once it has actually rotated.
    const float speed = t <= kRotateAt
        ? kObjectivePlaneTaxiSpeed * (t / kRotateAt)
        : kObjectivePlaneTaxiSpeed;
    plane.crashVelocity = {
        XMVectorGetX(heading) * speed,
        airborne > 0.0f ? kObjectivePlaneClimbRate : 0.0f,
        XMVectorGetZ(heading) * speed };

    // Carry the pitch it was holding into the tumble so the nose does not snap
    // level at the moment it is hit.
    plane.crashPitch = -0.28f * (std::min)(1.0f, airborne / 2.0f);
    plane.crashRoll = 0.0f;
    plane.crashYaw = 0.0f;
    plane.crashGroundY =
        GroundHeightAt(plane.crashPosition.x, plane.crashPosition.z);
}

// Drives every simulated prop from its rigid body: the render transform so it
// is drawn where it has moved to, and the prefab collider so the player, the AI
// and bullets meet it there too.
//
// Like the aircraft, motion is written into the batch's `transforms` and never
// `baseTransforms`: the authored placement stays the rebuild-safe copy, so a
// prefab rebuild re-seeds from where the container was placed rather than
// inheriting a half-simulated pose.
//
// A sleeping body is skipped. Box3D puts a resting container to sleep within a
// second or so, so a yard of them costs one IsPropBodyAwake call each per frame
// once they have settled, not a pose read and two matrix writes.
static void UpdatePrefabRigidBodies() {
    if (g_prefabRigidBodies.empty()) return;
    for (PrefabRigidBodyState& state : g_prefabRigidBodies) {
        const bool awake = g_destruction.IsPropBodyAwake(state.physicsHandle);
        // One final sync on the frame it falls asleep, so the resting pose is
        // the simulated one rather than the last frame's mid-motion guess.
        if (!awake && state.asleep) continue;
        state.asleep = !awake;

        DestructionBodyPose pose;
        if (!g_destruction.GetPropBodyPose(state.physicsHandle, pose)) continue;

        const XMVECTOR rotation = XMLoadFloat4(&pose.rotation);
        const XMMATRIX orientation = XMMatrixRotationQuaternion(rotation);
        // The body sits at the bounds centre; the model draws from its origin.
        // Rotating the stored offset and subtracting it puts the origin back
        // where the rotated box wants it, which for a corner-pivoted model like
        // the container is the difference between resting on the ground and
        // hovering half a container above it.
        const XMVECTOR center = XMVectorSet(
            pose.position.x, pose.position.y, pose.position.z, 0.0f);
        const XMVECTOR offset = XMVector3TransformNormal(
            XMLoadFloat3(&state.centerOffset), orientation);
        const XMVECTOR origin = XMVectorSubtract(center, offset);
        const XMMATRIX world = XMLoadFloat4x4(&state.scaleTransform) *
            orientation * XMMatrixTranslationFromVector(origin);

        for (PrefabRenderBatch& batch : g_prefabRenderBatches) {
            if (batch.prefabId != state.prefabId) continue;
            for (size_t i = 0; i < batch.entityIds.size(); ++i)
                if (batch.entityIds[i] == state.entityId &&
                    i < batch.transforms.size())
                    batch.transforms[i] = world;
        }

        // The collider is an axis-aligned-in-yaw box, so a container tumbling
        // end over end cannot be represented exactly. Track position and yaw,
        // which is what a shoved or blast-thrown container mostly does, and
        // accept that a fully upended one is approximated -- the alternative is
        // rebuilding every consumer of PrefabCollider around a full rotation.
        for (PrefabCollider& collider : g_prefabColliders) {
            if (collider.entityId != state.entityId) continue;
            XMStoreFloat3(&collider.center, center);
            XMFLOAT3 axis;
            XMStoreFloat3(&axis, XMVector3TransformNormal(
                XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), orientation));
            collider.yawRadians = std::atan2(axis.z, axis.x);
        }
    }
}

// Drives the aircraft objective: counts down, then flies it out.
//
// Motion is written into the render batch's `transforms` only. `baseTransforms`
// keeps the authored placement, so a prefab rebuild does not teleport a plane
// mid-climb, and the collider (built from the authored transform) stays where
// the aircraft started -- shooting it is a ranged objective, not a dogfight.
static void UpdateObjectivePlanes(float dt) {
    if (g_objectivePlanes.empty() || dt <= 0.0f) return;
    // Only while a run is actually live. The mission timer covers the normal
    // game (it is stopped during deployment planning and after the mission
    // ends), but an editor playtest deliberately runs with the timer stopped --
    // gating on the timer alone left the aircraft inert in exactly the mode a
    // designer would use to check it.
    if (!g_game.session.TimerRunning() && !IsEditorPlaying()) return;

    for (ObjectivePlaneState& plane : g_objectivePlanes) {
        if (plane.escaped) continue;

        // Shot down: fall on the last pose it was flying, tumbling, until it
        // meets the terrain. Runs ahead of the escape/takeoff logic and skips
        // it entirely -- a downed aircraft neither climbs nor escapes.
        if (plane.crashing) {
            plane.crashVelocity.y -= kObjectivePlaneCrashGravity * dt;
            const float drag =
                (std::max)(0.0f, 1.0f - kObjectivePlaneCrashDrag * dt);
            plane.crashVelocity.x *= drag;
            plane.crashVelocity.z *= drag;
            plane.crashPosition.x += plane.crashVelocity.x * dt;
            plane.crashPosition.y += plane.crashVelocity.y * dt;
            plane.crashPosition.z += plane.crashVelocity.z * dt;
            // Tumble, but bounded. The Black Hawk uses these same rates and gets
            // away with them because it falls from a low hover; this aircraft
            // can be hit at 110 m, and measured against that fall the unclamped
            // tumble reached 128 deg of pitch and 267 deg of roll by impact --
            // the airframe landed inverted and nose-buried. Clamping to a steep
            // but survivable-looking attitude keeps the wreck readable however
            // far it fell. Yaw is left free: spinning about the vertical axis
            // looks right at any angle.
            constexpr float kMaxCrashPitch = 1.05f;  // ~60 deg nose-down
            constexpr float kMaxCrashRoll = 1.22f;   // ~70 deg
            plane.crashPitch = (std::min)(kMaxCrashPitch,
                plane.crashPitch + kObjectivePlaneCrashPitchRate * dt);
            plane.crashRoll = (std::min)(kMaxCrashRoll,
                plane.crashRoll + kObjectivePlaneCrashRollRate * dt);
            plane.crashYaw += kObjectivePlaneCrashYawRate * dt;
            // Sampled every frame rather than once at the hit: the wreck drifts
            // while it falls, so the impact height belongs to where it lands.
            // Offset by the airframe's own reach so it comes to rest ON the
            // terrain -- dropping the origin onto it buried the fuselage.
            plane.crashGroundY =
                GroundHeightAt(plane.crashPosition.x, plane.crashPosition.z) +
                plane.groundClearance;
            if (plane.crashPosition.y <= plane.crashGroundY) {
                plane.crashPosition.y = plane.crashGroundY;
                plane.crashing = false;
                plane.crashed = true;
                plane.crashVelocity = { 0.0f, 0.0f, 0.0f };
                scene.SpawnSmokeBurst(plane.crashPosition, 3.2f, 1.4f);
                SGE_LOG("LogGameplay", EngineLog::Level::Display,
                    "Objective aircraft hit the ground");
            }
            WriteObjectivePlaneCrashTransform(plane);
            continue;
        }
        // Already down and settled: leave the wreck exactly where it landed.
        if (plane.crashed) {
            WriteObjectivePlaneCrashTransform(plane);
            continue;
        }
        if (plane.destroyed) continue;

        if (!plane.rolling) {
            plane.holdTimer += dt;
            if (plane.holdTimer < kObjectivePlaneHoldSeconds) continue;
            plane.rolling = true;
            SGE_LOG("LogGameplay", EngineLog::Level::Display,
                "Objective aircraft beginning takeoff run");
        }

        plane.takeoffTimer += dt;
        const float t = plane.takeoffTimer;
        // Rotation (nose-up) is the moment the wheels leave the ground; before
        // it the aircraft only accelerates along its heading.
        constexpr float kRotateAt = 4.0f;
        // Integrated rather than sampled so speed changes cannot make the
        // aircraft jump backwards along its own runway.
        const float distance = t <= kRotateAt
            ? 0.5f * kObjectivePlaneTaxiSpeed * (t * t) / kRotateAt
            : 0.5f * kObjectivePlaneTaxiSpeed * kRotateAt +
              kObjectivePlaneTaxiSpeed * (t - kRotateAt);
        const float airborne = (std::max)(0.0f, t - kRotateAt);
        const float climb = kObjectivePlaneClimbRate * airborne;
        // Ease the pitch in over two seconds so it rotates rather than snapping
        // nose-up the instant it leaves the runway.
        const float pitch = -0.28f * (std::min)(1.0f, airborne / 2.0f);
        // No bank, no yaw drift: it departs wings-level straight off the
        // runway on the heading it was authored with.

        // Travel straight down the model's own nose axis. Rebuilding a heading
        // from sin/cos of an assumed +Z nose is what sent it sideways.
        const XMVECTOR heading = XMLoadFloat3(&plane.forward);
        const XMVECTOR travel = XMVectorScale(heading, distance);
        const XMMATRIX flight =
            XMMatrixRotationRollPitchYaw(pitch, 0.0f, 0.0f) *
            XMMatrixTranslation(
                plane.basePosition.x + XMVectorGetX(travel),
                plane.basePosition.y + climb,
                plane.basePosition.z + XMVectorGetZ(travel));
        // Authored scale/orientation live in baseTransform; strip its
        // translation and re-apply the flight pose on top so a scaled or
        // rotated placement still flies correctly.
        XMMATRIX oriented = XMLoadFloat4x4(&plane.baseTransform);
        oriented.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
        const XMMATRIX world = oriented * flight;
        WriteObjectivePlaneTransform(plane, world);

        if (t >= kObjectivePlaneTakeoffSeconds) {
            plane.escaped = true;
            g_objectivePlaneEscaped = true;
            g_game.mission.RecordObjectivePlaneEscaped();
            SGE_LOG("LogGameplay", EngineLog::Level::Warning,
                "Objective aircraft escaped");
            // An escape resolves the aircraft too. The mission is failed on
            // that count and the report says so, but the boat still has to come
            // -- it is the only way to end a run, and letting the plane go must
            // not leave the player stranded with nothing to do.
            OnObjectivePlaneResolved();
        }
    }
}

// `fromPlayer` distinguishes the player's own fire from everything else that
// reaches this entry point (enemy rounds, blasts, spreading fire). It gates the
// aircraft objective and drives its hitmarker; it defaults true so the many
// existing world-damage callers keep their behaviour for ordinary props, which
// are damageable by anything.
static void DamagePrefabEntity(uint64_t entityId, float damage,
                               const XMFLOAT3& hit, bool fromRemoteCharge,
                               bool fromPlayer = true) {
    if (!CommTowerDamageAllowed(entityId, fromRemoteCharge)) return;
    if (!ObjectivePlaneDamageAllowed(entityId, fromPlayer)) return;
    const bool isObjectivePlane = IsObjectivePlaneEntity(entityId);
    XMFLOAT3 towerBase{};
    const bool isCommTower = FindCommTower(entityId, towerBase);
    const CombatSystem::PrefabDamageResult result =
        g_game.combat.DamagePrefab(g_game.world, entityId, damage, hit);
    // Any damage that reaches a tower is worth a line in the log: it should only
    // ever happen with a charge planted on it, so an entry that appears at level
    // start (or from anything else) names the path that is still leaking.
    //
    // Logged after the call and carrying `applied`, because a line printed
    // beforehand only says damage was attempted. An unregistered destructible
    // makes DamagePrefab a no-op, and reading "taking 1000000 damage" on a tower
    // that was never in the health map sent two investigations the wrong way.
    if (isCommTower)
        SGE_LOG("LogPrefab", EngineLog::Level::Display,
            "Comm tower taking " + std::to_string(damage) +
            " damage (fromRemoteCharge=" + std::to_string(fromRemoteCharge) +
            ", rigged=" +
            std::to_string(g_commTowersRiggedForDemolition.count(entityId)) +
            ", applied=" + std::to_string(result.applied) +
            ", destroyed=" + std::to_string(result.destroyed) + ")");
    // Steel lattice: every round that lands on it rings, destroyed or not.
    if (isCommTower && result.applied) PlayMetalHitAudio(hit, 0.9f);
    // Aircraft skin is metal too, and an objective the player is deliberately
    // shooting owes them feedback: the marker confirms the round connected, and
    // reads lethal on the hit that finally brings it down. Gated on `applied`
    // so rounds into an already-downed wreck (health <= 0) do not keep marking.
    if (isObjectivePlane && result.applied) {
        PlayMetalHitAudio(hit, 1.05f);
        scene.TriggerHitMarker(result.destroyed);
    }
    if (result.destroyed) {
        if (g_game.session.TimerRunning()) {
            g_game.mission.RecordDestruction();
            g_game.money.Award(MoneyEvent::PropDestroyed);
            if (isCommTower) {
                g_game.mission.RecordCommTowerDestroyed();
                g_game.money.Award(MoneyEvent::CommTowerDestroyed);
                // Levelling the tower is what brings the reinforcements: the
                // garrison notices the moment it goes off the air.
                CallInReinforcementWave();
            }
        }
        // Tower down: the score opens up to full and stays there, and the
        // commander calls the ride in a beat later.
        if (isCommTower) {
            g_commTowerMusicSwell = true;
            g_exfilHereDelay = kExfilHereDelay;
        }
        // Aircraft shot down: hand it to the crash path, which flies it into the
        // ground from wherever it was, and mark it so the escape check ignores
        // it. Deliberately no prefab rebuild here -- the wreck keeps drawing out
        // of the existing batch, and rebuilding mid-kill is what stalled the
        // frame.
        bool downedPlane = false;
        for (ObjectivePlaneState& plane : g_objectivePlanes) {
            if (plane.entityId != entityId) continue;
            downedPlane = true;
            BeginObjectivePlaneCrash(plane);
            scene.SpawnSmokeBurst(hit, 2.4f, 0.9f);
            if (g_game.session.TimerRunning()) {
                g_game.mission.RecordObjectivePlaneDestroyed();
                g_game.money.Award(MoneyEvent::ObjectivePlaneDestroyed);
            }
            SGE_LOG("LogGameplay", EngineLog::Level::Display,
                "Objective aircraft destroyed");
        }
        // Outside the loop: the response to the aircraft going down is called
        // once, not once per matching plane.
        if (downedPlane) OnObjectivePlaneResolved();
        if (isCommTower) CollapseCommTower(towerBase);
        else if (!downedPlane) scene.SpawnSmokeBurst(hit, 1.2f, 0.45f);
        // Same targeted removal as the radius path: a felled comm tower still
        // needs the full rebuild, since its geometry is handed to the
        // destruction model, but an ordinary prop only has to stop drawing.
        if (!downedPlane) {
            if (isCommTower || !RemovePrefabEntityFromRuntime(entityId)) {
                g_prefabRebuildRequested = true;
                g_prefabRebuildReason = isCommTower
                    ? "comm tower felled (direct)"
                    : "prefab destroyed (direct, removal missed)";
            }
        }
        SGE_LOG("LogPrefab", EngineLog::Level::Display,
            "Destroyed prefab entity " + std::to_string(entityId));
    }
}

static void DamagePrefabsInRadius(const XMFLOAT3& center, float radius,
                                  float damage, bool fromRemoteCharge,
                                  bool fromPlayer = true) {
    const auto results = g_game.combat.DamagePrefabsInRadius(
        g_game.world, center, radius, damage,
        [fromRemoteCharge, fromPlayer](uint64_t entityId) {
            return CommTowerDamageAllowed(entityId, fromRemoteCharge) &&
                   ObjectivePlaneDamageAllowed(entityId, fromPlayer);
        });
    for (const CombatSystem::PrefabDamageResult& result : results) {
        // Marked before the destroyed-only skip below, so a blast that damages
        // the aircraft without downing it still confirms the hit.
        if (result.applied && IsObjectivePlaneEntity(result.entityId))
            scene.TriggerHitMarker(result.destroyed);
        if (!result.destroyed) continue;
        // A tower felled through the radius path still needs its collapse, not
        // the generic puff of smoke every other prefab gets.
        XMFLOAT3 towerBase{};
        if (FindCommTower(result.entityId, towerBase)) {
            if (g_game.session.TimerRunning()) {
                g_game.mission.RecordDestruction();
                g_game.mission.RecordCommTowerDestroyed();
                g_game.money.Award(MoneyEvent::PropDestroyed);
                g_game.money.Award(MoneyEvent::CommTowerDestroyed);
                CallInReinforcementWave();
            }
            g_commTowerMusicSwell = true;
            g_exfilHereDelay = kExfilHereDelay;
            CollapseCommTower(towerBase);
            g_prefabRebuildRequested = true;
            g_prefabRebuildReason = "comm tower felled (radius)";
            continue;
        }
        if (g_game.session.TimerRunning()) {
            g_game.mission.RecordDestruction();
            g_game.money.Award(MoneyEvent::PropDestroyed);
        }
        // An aircraft caught in a blast goes down the same way one shot out of
        // the sky does: crash it rather than rebuilding it out of existence.
        bool downedPlane = false;
        for (ObjectivePlaneState& plane : g_objectivePlanes) {
            if (plane.entityId != result.entityId) continue;
            downedPlane = true;
            BeginObjectivePlaneCrash(plane);
            scene.SpawnSmokeBurst(result.effectPosition, 2.4f, 0.9f);
            if (g_game.session.TimerRunning()) {
                g_game.mission.RecordObjectivePlaneDestroyed();
                g_game.money.Award(MoneyEvent::ObjectivePlaneDestroyed);
            }
            SGE_LOG("LogGameplay", EngineLog::Level::Display,
                "Objective aircraft destroyed");
        }
        if (downedPlane) {
            OnObjectivePlaneResolved();
            continue;
        }
        scene.SpawnSmokeBurst(result.effectPosition, 1.2f, 0.45f);
        // Remove just this entity instead of requesting a full rebuild. The
        // rebuild reloads every model and re-inits every audio player in the
        // level -- hundreds of ms to seconds -- to accomplish what erasing one
        // entry does. Fall back to the rebuild only if the entity was not found
        // in the runtime lists, so anything the targeted path cannot express
        // still resolves correctly rather than leaving a ghost prop.
        if (!RemovePrefabEntityFromRuntime(result.entityId)) {
            g_prefabRebuildRequested = true;
            g_prefabRebuildReason = "prefab destroyed (radius, removal missed)";
        }
        SGE_LOG("LogPrefab", EngineLog::Level::Display,
            "Destroyed prefab entity " + std::to_string(result.entityId));
    }
}
