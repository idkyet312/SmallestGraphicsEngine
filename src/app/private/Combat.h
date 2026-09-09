#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Reload click. Pitched per weapon so the heavier guns sound heavier: the RPG
// and shotgun drop below unity, the AK sits near it, the SVD just above.
static void PlayReloadSound() {
    float pitch = 1.0f;
    if (GunModel::HarpoonSelected())       pitch = 0.68f;
    else if (GunModel::FlamethrowerSelected())  pitch = 0.74f;
    else if (GunModel::C4Selected())       pitch = 1.18f;
    else if (GunModel::LaserSelected())    pitch = 1.28f;
    else if (GunModel::RPGSelected())      pitch = 0.72f;
    else if (GunModel::ShotgunSelected())  pitch = 0.85f;
    else if (GunModel::R700Selected())     pitch = 1.06f;
    pitch += ((float)std::rand() / RAND_MAX) * 0.05f;
    g_reloadAudio.Play(0.85f, pitch);
}

// Fires the selected weapon if it has a round chambered. Returns false when the
// shot was blocked (empty magazine or mid-reload) so callers can skip arming the
// fire cooldown. Ammo is only enforced outside god mode -- see Scene::ConsumeAmmo.
static bool ShootPlayerWeapon() {
    const int slot = GunModel::SelectedWeapon();
    const SGE::ResolvedWeaponStats weaponStats =
        scene.player.ResolveWeaponStats(slot);
    if (!scene.ConsumeAmmo(slot)) {
        // Dry fire: auto-reload if there are spare rounds, so the player is not
        // stuck clicking an empty gun without knowing why. BeginReload returns
        // true only when a reload actually starts, so the click sound fires once
        // rather than on every held-trigger frame.
        if (scene.BeginReload(slot)) PlayReloadSound();
        return false;
    }
    const size_t projectileStart = scene.projectiles.size();
    if (GunModel::HarpoonSelected()) {
        scene.ShootHarpoonProjectile();
        const float pitch = 0.62f + ((float)std::rand() / RAND_MAX) * 0.05f;
        g_gunAudio.Play(1.0f, pitch);
    } else if (GunModel::C4Selected()) {
        scene.ThrowRemoteCharge();
        g_reloadAudio.Play(0.38f, 1.24f);
    } else if (GunModel::FlamethrowerSelected()) {
        scene.ShootFlameBurst();
    } else if (GunModel::LaserSelected()) {
        scene.ShootLaserProjectile();
        const float pitch = 1.65f + ((float)std::rand() / RAND_MAX) * 0.10f;
        g_gunAudio.Play(0.30f, pitch);
    } else if (GunModel::R700Selected()) {
        // Same round and the same damage either way -- the suppressor changes
        // what the shot sounds like, not what it does. Quieter and pitched up:
        // a can takes the bass out of a report, leaving a flat crack rather
        // than the boom the unsuppressed rifle makes.
        scene.ShootSniperProjectile(weaponStats);
        const float pitch = (weaponStats.suppressed ? 1.24f : 0.70f) +
            ((float)std::rand() / RAND_MAX) * 0.04f;
        g_gunAudio.Play(weaponStats.suppressed ? 0.34f : 1.0f, pitch);
    } else if (GunModel::RPGSelected()) {
        scene.ShootRocket();
        const float pitch = 0.68f + ((float)std::rand() / RAND_MAX) * 0.05f;
        g_rpgFireAudio.Play(1.0f, pitch);
    } else if (GunModel::ShotgunSelected()) {
        scene.ShootShotgun();
        const float pitch = 0.78f + ((float)std::rand() / RAND_MAX) * 0.08f;
        g_gunAudio.Play(0.96f, pitch);
    } else {
        scene.ShootProjectile(weaponStats);
        const float pitch = (weaponStats.suppressed ? 1.18f : 0.96f) +
            ((float)std::rand() / RAND_MAX) * 0.08f;
        g_gunAudio.Play(weaponStats.suppressed ? 0.30f : 0.82f, pitch);
    }
    const uint32_t projectileCount = static_cast<uint32_t>(
        scene.projectiles.size() - projectileStart);
    for (size_t index = projectileStart; index < scene.projectiles.size(); ++index)
        scene.projectiles[index].playerOwned = true;
    if (g_game.session.TimerRunning())
        g_game.mission.RecordWeaponFired(slot, projectileCount);
    // Gunfire is loud enough for nearby enemies to hear through walls, even
    // ones that can't currently see the player. Same radius as the squad-alert
    // broadcast so "heard the shot" and "saw a squadmate get hit" read as the
    // same kind of event.
    //
    // The suppressed SVD cuts that radius hard. This is the whole weapon: it
    // fires the same round for the same damage as the standard rifle, and buys
    // its advantage entirely in how far the shot carries. A quarter radius
    // means a kill at range no longer wakes the squad around the target, which
    // is what lets a sniper work through a patrol one man at a time.
    //
    // Not silent, deliberately. A suppressed rifle is still loud enough to
    // notice from close by, so a man standing next to the one you drop will
    // still turn -- picking targets on the edge of a group remains the skill.
    const float noiseRadius = SkinnedEnemy::AlertBroadcastRadius() *
        weaponStats.noiseRadiusMultiplier;
    g_enemyNoiseEvents.push_back({ scene.camera.Position, noiseRadius });
    return true;
}

static float PlayerFireInterval() {
    const int slot = GunModel::SelectedWeapon();
    const SGE::ResolvedWeaponStats stats =
        scene.player.ResolveWeaponStats(slot);
    // Keep the AK's live debug tuning authoritative when it is uncustomised;
    // every fixed-cadence weapon is now described by its immutable definition.
    return slot == 0 ? scene.fireInterval : stats.fireIntervalSeconds;
}

static void UpdateHarpoonAttachments() {
    for (Projectile& projectile : scene.projectiles) {
        if (!projectile.active || !projectile.harpoon) continue;
        if (projectile.harpoonExpired) {
            // Missed every pin surface. Corpses keep flying forward; never
            // reverse toward player.
            g_destruction.ReleaseHarpoonRagdolls(
                projectile.harpoonId, projectile.direction, 12.0f);
            projectile.active = false;
            continue;
        }
        g_destruction.MoveHarpoonRagdolls(
            projectile.harpoonId, projectile.position, projectile.direction);
    }
    for (PinnedHarpoonFX& pin : scene.pinnedHarpoons) {
        if (pin.harpoonId == 0) continue;
        g_destruction.GetPinnedHarpoonPose(
            pin.harpoonId, pin.position, pin.direction);
    }
}

static bool HitTerrainSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                              float radius, XMFLOAT3& hit) {
    if (!scene.useMeshTerrain || !g_terrain.supported) return false;
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float dz = end.z - start.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int steps = (std::max)(4, (std::min)(32,
        static_cast<int>(std::ceil(length / 0.15f))));
    float previousT = 0.0f;
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float x = start.x + dx * t;
        const float y = start.y + dy * t;
        const float z = start.z + dz * t;
        if (y > TerrainRendererDX12::HeightAt(params, x, z) + radius) {
            previousT = t;
            continue;
        }
        float lo = previousT, hi = t;
        for (int refine = 0; refine < 6; ++refine) {
            const float mid = (lo + hi) * 0.5f;
            const float mx = start.x + dx * mid;
            const float my = start.y + dy * mid;
            const float mz = start.z + dz * mid;
            if (my > TerrainRendererDX12::HeightAt(params, mx, mz) + radius)
                lo = mid;
            else
                hi = mid;
        }
        hit = { start.x + dx * hi, start.y + dy * hi, start.z + dz * hi };
        return true;
    }
    return false;
}

// hitNormal is last and defaulted so the call sites that do not want a surface
// normal compile unchanged. It is written only for triangle-mesh hits; a bounds
// box has no meaningful normal, so callers keep their own fallback.
static bool HitPrefabColliderSegment(const XMFLOAT3& start,
                                     const XMFLOAT3& end, float radius,
                                     XMFLOAT3& hit,
                                     uint64_t* hitEntityId = nullptr,
                                     XMFLOAT3* hitNormal = nullptr);

static bool BanditHasLineOfSight(const SkinnedEnemy& shooter,
                                 const XMFLOAT3& target) {
    const XMFLOAT3 origin = shooter.AimRayOrigin();
    constexpr float rayRadius = 0.04f;
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(origin, target, rayRadius, hit))
        return false;
    if (HitPrefabColliderSegment(origin, target, rayRadius, hit)) return false;
    if (HitTerrainSegment(origin, target, rayRadius, hit)) return false;
    if (g_trees.BlocksSegment(origin, target, rayRadius)) return false;
    for (const auto& bandit : g_bandits) {
        if (!bandit || bandit.get() == &shooter || bandit->Dead()) continue;
        // Human shield must not stop enemies from taking the shot. The hostile
        // projectile collision path damages the held Bandit before the player.
        if (bandit->Held()) continue;
        // The actor being aimed at cannot occlude the shot at itself. When the
        // target is another actor (marine aiming at a bandit, or vice versa)
        // the ray ends inside that actor's own ragdoll, which would otherwise
        // report a block every frame and stall the aim-up before any shot.
        // The tolerance is body-sized on purpose: the target position is a
        // snapshot taken earlier in the frame, so the actor has usually moved
        // a little by now, and anyone standing that close to the endpoint is
        // the target rather than something meaningfully in the way.
        const float tdx = bandit->position.x - target.x;
        const float tdz = bandit->position.z - target.z;
        if (tdx * tdx + tdz * tdz < 1.0f) continue;
        if (bandit->BlocksProjectile(origin, target, rayRadius)) return false;
    }
    return true;
}

EnemyLineOfSightFn g_enemyLineOfSightFn = &BanditHasLineOfSight;
std::vector<EnemyNoiseEvent> g_enemyNoiseEvents;
std::vector<EnemyAlertEvent> g_enemyAlertEvents;
// Clear daylight until a time of day says otherwise. ApplyTimeOfDay overwrites
// this, and every level start runs through there, so the default only stands
// for the frames before the first preset lands.
float g_enemyVisionScale = 1.0f;

// Would an ally's grenade at `target` catch someone on the player's side?
//
// Grenade blast damage is indiscriminate by design -- the explosion handler
// damages every actor in radius and the player too, regardless of who threw
// it. That is correct for the player's own throws, but an ally choosing to
// throw has to answer for it, so check the impact point against the player
// and every other live marine first. The thrower itself is excluded: it is
// already standing where it is, and the range band keeps it clear.
static bool AllyGrenadeBlastIsSafe(const SkinnedEnemy& thrower,
                                   const XMFLOAT3& target) {
    const float safeSq = kAllyGrenadeSafeRadius * kAllyGrenadeSafeRadius;
    const float px = scene.camera.Position.x - target.x;
    const float pz = scene.camera.Position.z - target.z;
    if (px * px + pz * pz < safeSq) return false;

    for (const auto& other : g_bandits) {
        if (!other || other.get() == &thrower || other->Dead()) continue;
        if (other->faction != Faction::Marine) continue;
        const float mx = other->position.x - target.x;
        const float mz = other->position.z - target.z;
        if (mx * mx + mz * mz < safeSq) return false;
    }
    return true;
}

// Ballistic lob from the thrower toward the target. Solves the launch elevation
// for a fixed speed under gravity, so grenades arc onto the target instead of
// being flung flat past them.
//
// Target is passed in rather than read from the camera: marines throw at the
// bandit they are fighting, not at the player. hostile follows the thrower --
// a hostile grenade's projectile body can strike the player directly, while a
// marine's passes them by. The blast itself is deliberately indiscriminate for
// both (see the explosion handler), so anyone standing too close still pays.
static void BanditThrowGrenade(const SkinnedEnemy& bandit,
                               const XMFLOAT3& target, bool hostile) {
    const XMFLOAT3 origin = bandit.AimRayOrigin();

    const float dx = target.x - origin.x;
    const float dz = target.z - origin.z;
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    if (horizontal < 0.01f) return;
    const float dy = target.y - origin.y;

    // Aim slightly ahead of the player so a moving target still gets bracketed.
    const float speed = scene.grenadeThrowSpeed;
    const float gravity = 9.81f * scene.grenadeGravityScale;

    // Projectile solution: pick the low-arc launch angle that lands at (h, dy).
    // v^4 - g(g*h^2 + 2*dy*v^2) < 0 means the target is out of range; fall back
    // to a 45-degree heave so the grenade still travels sensibly.
    const float v2 = speed * speed;
    const float discriminant =
        v2 * v2 - gravity * (gravity * horizontal * horizontal + 2.0f * dy * v2);
    float angle;
    if (discriminant < 0.0f) {
        angle = XM_PIDIV4;
    } else {
        angle = std::atan((v2 - std::sqrt(discriminant)) / (gravity * horizontal));
    }

    const float horizontalSpeed = speed * std::cos(angle);
    const float verticalSpeed = speed * std::sin(angle);
    const float inverseHorizontal = 1.0f / horizontal;

    Projectile grenade = {};
    grenade.position = grenade.previousPosition = origin;
    grenade.direction = { dx * inverseHorizontal, 0.0f, dz * inverseHorizontal };
    grenade.grenade = true;
    grenade.hostile = hostile;
    grenade.active = true;
    grenade.fuse = scene.grenadeFuse;
    grenade.grenadeCollisionGrace = 0.18f;
    grenade.velocity = { dx * inverseHorizontal * horizontalSpeed,
                         verticalSpeed,
                         dz * inverseHorizontal * horizontalSpeed };
    scene.projectiles.push_back(grenade);
}

// Fires an impact-fused round from the deployment camera to a map coordinate.
//
// It launches from where the player is looking from and flies to where they
// clicked, so the shot reads as coming off the screen rather than materialising
// overhead. The camera orbits high above the island, so this is a steep
// descending flight rather than a lobbed throw.
//
// Flight time is fixed rather than derived from a launch speed: given a start,
// an end and a duration, the required velocity is exact, so the round lands on
// the clicked point regardless of how far the camera happens to be orbiting.
// Solving for an angle instead would miss whenever the target was out of range.
static void LaunchMissileStrike(const XMFLOAT3& origin,
                                const XMFLOAT3& target) {
    // Long enough to watch it travel, short enough not to leave the player
    // waiting on the planning screen.
    constexpr float kFlightSeconds = 1.6f;
    const float gravity = 9.81f * scene.grenadeGravityScale;

    Projectile missile = {};
    missile.position = missile.previousPosition = origin;
    missile.grenade = true;
    missile.hostile = false;
    missile.active = true;
    // Impact-fused: it bursts on first contact instead of bouncing like a
    // thrown frag. The fuse is only a failsafe for a round that somehow
    // contacts nothing.
    missile.impactFuse = true;
    missile.missile = true;
    missile.fuse = kFlightSeconds + 8.0f;
    // Launched from the camera, which is where the player's own body would be
    // -- without the grace period the round detonates on its own launch point.
    missile.grenadeCollisionGrace = 0.18f;

    // Displacement under constant gravity: d = v*t + 0.5*g*t^2, so the velocity
    // that puts it exactly on target at t is v = (d - 0.5*g*t^2) / t. The
    // vertical term carries the gravity correction; horizontal is just d/t.
    const float dx = target.x - origin.x;
    const float dy = target.y - origin.y;
    const float dz = target.z - origin.z;
    missile.velocity = {
        dx / kFlightSeconds,
        dy / kFlightSeconds + 0.5f * gravity * kFlightSeconds,
        dz / kFlightSeconds };

    const float horizontal = std::sqrt(dx * dx + dz * dz);
    missile.direction = horizontal > 0.01f
        ? XMFLOAT3{ dx / horizontal, 0.0f, dz / horizontal }
        : XMFLOAT3{ 0.0f, -1.0f, 0.0f };
    scene.projectiles.push_back(missile);
}

// Red targeting beam a charging sniper paints on the player. Drawn as a thin
// additive box stretched from muzzle to target, same technique as bullet
// tracers, so it stays bright in shadow instead of reading as a red stick.
// The beam is the entire counterplay to the sniper's damage: it has to be
// impossible to miss, so it brightens and thickens as the shot approaches.
static void DrawSniperLaser(const SkinnedEnemy& bandit, Scene& scene,
                            ShaderDX12& shader, const GeometryBuffers& geo,
                            const XMMATRIX& lightSpace) {
    if (!bandit.LaserActive()) return;

    const XMFLOAT3 originF = bandit.AimRayOrigin();
    const XMFLOAT3 targetF = bandit.LaserTarget();
    const XMVECTOR origin = XMLoadFloat3(&originF);
    const XMVECTOR target = XMLoadFloat3(&targetF);
    XMVECTOR along = target - origin;
    const float length = XMVectorGetX(XMVector3Length(along));
    if (length < 0.05f) return;

    XMVECTOR forward = along / length;
    XMVECTOR up = std::fabs(XMVectorGetY(forward)) > 0.95f
        ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
        : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
    up = XMVector3Cross(forward, right);

    XMMATRIX basis = XMMatrixIdentity();
    basis.r[0] = XMVectorSetW(right, 0.0f);
    basis.r[1] = XMVectorSetW(up, 0.0f);
    basis.r[2] = XMVectorSetW(forward, 0.0f);
    basis.r[3] = XMVectorSetW(origin + forward * (length * 0.5f), 1.0f);

    // Ramp from a faint sighting line to a hot, obviously-about-to-fire beam,
    // with a pulse over the last stretch so the final second reads as urgent
    // even in the corner of the eye. Driven off the charge itself rather than a
    // wall clock, so every sniper's beam pulses in step with its own timer.
    const float charge = bandit.LaserCharge();
    const float pulse = charge > 0.6f
        ? 1.0f + 0.35f * std::sin(charge * SkinnedEnemy::kSniperLaserWarning * 26.0f)
        : 1.0f;
    const float intensity = (0.35f + 1.65f * charge * charge) * pulse;
    // Cross-section is a sixth of the original beam: at full size the box read
    // as a translucent slab head-on, which is the angle a beam aimed at the
    // player is always seen from. Length and brightness are unchanged.
    constexpr float kThickness = 1.0f / 6.0f;
    const float radius = (0.012f + 0.016f * charge) * kThickness;

    // Capsule mesh is built along its own local Y (radius 0.25, half-length
    // 0.5), so swing that axis onto the beam's forward Z and undo those base
    // dimensions before applying the real radius and length.
    const XMMATRIX capsuleToBeam = XMMatrixRotationX(XM_PIDIV2);
    auto capsuleModel = [&](float r, float len) {
        return XMMatrixScaling(r * 4.0f, len, r * 4.0f) * capsuleToBeam * basis;
    };

    shader.UseAdditive();
    XMMATRIX model = capsuleModel(radius * 2.4f, length);
    shader.SetMatrices(model, scene.GetViewMatrix(),
                       scene.GetProjectionMatrix(), lightSpace);
    shader.SetEmissiveMaterial(
        XMFLOAT3(3.4f * intensity, 0.05f, 0.03f), 0.20f + 0.16f * charge);
    DrawCapsule(geo);
    shader.NextDrawCall();

    // Thin white-hot core keeps the line crisp at distance, where the soft halo
    // alone would smear into a wide pink band.
    model = capsuleModel(radius * 0.85f, length);
    shader.SetMatrices(model, scene.GetViewMatrix(),
                       scene.GetProjectionMatrix(), lightSpace);
    shader.SetEmissiveMaterial(
        XMFLOAT3(9.0f * intensity, 0.6f, 0.45f), 0.85f);
    DrawCapsule(geo);
    shader.NextDrawCall();

    // Dot on the player. Sells the beam as aimed AT them rather than past them.
    // Sphere mesh is radius 0.5, so double the scale to get the wanted radius.
    const float dot = (0.045f + 0.03f * charge) * kThickness * 2.0f;
    model = XMMatrixScaling(dot, dot, dot) *
            XMMatrixTranslation(targetF.x, targetF.y, targetF.z);
    shader.SetMatrices(model, scene.GetViewMatrix(),
                       scene.GetProjectionMatrix(), lightSpace);
    shader.SetEmissiveMaterial(
        XMFLOAT3(9.0f * intensity, 0.35f, 0.25f), 0.95f);
    DrawSphere(geo);
    shader.NextDrawCall();
    shader.Use(scene.wireframeMode);
}

static bool GrabPathClear(const XMFLOAT3& target) {
    constexpr float rayRadius = 0.05f;
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(scene.camera.Position, target, rayRadius, hit))
        return false;
    if (HitTerrainSegment(scene.camera.Position, target, rayRadius, hit)) return false;
    if (g_trees.BlocksSegment(scene.camera.Position, target, rayRadius)) return false;
    return true;
}

static Projectile* HeldGrenade() {
    for (Projectile& projectile : scene.projectiles) {
        if (projectile.active && projectile.grenade && projectile.held)
            return &projectile;
    }
    return nullptr;
}

static XMFLOAT3 GrenadeHoldPosition() {
    const XMFLOAT3& eye = scene.camera.Position;
    const XMFLOAT3& front = scene.camera.Front;
    return { eye.x + front.x * 1.35f,
             eye.y + front.y * 1.35f - 0.28f,
             eye.z + front.z * 1.35f };
}

static void ReleaseGrenadePhysicsBody(Projectile& grenade) {
    if (grenade.grenadePhysicsHandle != 0 && g_destruction.IsInitialized())
        g_destruction.DestroyGrenadeBody(grenade.grenadePhysicsHandle);
    grenade.grenadePhysicsHandle = 0;
}

static void UpdateHeldGrenade() {
    Projectile* grenade = HeldGrenade();
    if (!grenade) return;
    ReleaseGrenadePhysicsBody(*grenade);
    grenade->position = GrenadeHoldPosition();
    grenade->previousPosition = grenade->position;
    grenade->velocity = { 0.0f, 0.0f, 0.0f };
}

static void ThrowHeldGrenade() {
    Projectile* grenade = HeldGrenade();
    if (!grenade) return;

    XMVECTOR direction = XMVector3Normalize(
        XMLoadFloat3(&scene.camera.Front) +
        XMVectorSet(0.0f, 0.12f, 0.0f, 0.0f));
    XMFLOAT3 directionF;
    XMStoreFloat3(&directionF, direction);
    grenade->position = GrenadeHoldPosition();
    grenade->previousPosition = grenade->position;
    grenade->direction = directionF;
    XMStoreFloat3(&grenade->velocity,
        direction * 19.0f + XMVectorSet(0.0f, 1.4f, 0.0f, 0.0f));
    grenade->held = false;
    // The returned grenade must not immediately collide with the player who
    // caught it. Blast damage remains indiscriminate once its original fuse ends.
    grenade->hostile = false;
    grenade->grenadeCollisionGrace = 0.18f;
}

static bool GrabEnemyGrenade() {
    const XMVECTOR eye = XMLoadFloat3(&scene.camera.Position);
    const XMVECTOR front = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    Projectile* best = nullptr;
    float bestScore = FLT_MAX;
    for (Projectile& grenade : scene.projectiles) {
        if (!grenade.active || !grenade.grenade || !grenade.hostile ||
            grenade.held || grenade.detonate)
            continue;
        const XMVECTOR toTarget = XMLoadFloat3(&grenade.position) - eye;
        const float forward = XMVectorGetX(XMVector3Dot(toTarget, front));
        if (forward < 0.15f || forward > 4.0f) continue;
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(toTarget));
        const float sideSq = (std::max)(0.0f, distanceSq - forward * forward);
        if (sideSq > 0.90f * 0.90f || !GrabPathClear(grenade.position)) continue;
        const float score = sideSq * 5.0f + forward * 0.05f;
        if (score < bestScore) {
            bestScore = score;
            best = &grenade;
        }
    }
    if (!best) return false;

    g_heldBandit = nullptr;
    g_heldBarrelIndex = SIZE_MAX;
    ReleaseGrenadePhysicsBody(*best);
    best->held = true;
    best->velocity = { 0.0f, 0.0f, 0.0f };
    best->position = GrenadeHoldPosition();
    best->previousPosition = best->position;
    return true;
}

static void GrabOrThrowBandit() {
    using namespace DirectX;
    if (g_heldBandit && g_heldBandit->Held()) {
        XMVECTOR throwDirection = XMVector3Normalize(
            XMLoadFloat3(&scene.camera.Front) + XMVectorSet(0.0f, 0.16f, 0.0f, 0.0f));
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, throwDirection);
        g_heldBandit->Throw(direction, 16.0f);
        g_heldBandit = nullptr;
        PlayBanditDeathEvents();
        return;
    }
    g_heldBandit = nullptr;

    const XMVECTOR eye = XMLoadFloat3(&scene.camera.Position);
    const XMVECTOR front = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    SkinnedEnemy* best = nullptr;
    float bestScore = FLT_MAX;
    for (const auto& bandit : g_bandits) {
        if (!bandit || bandit->Dead()) continue;
        if (bandit->faction != Faction::Bandit) continue; // never grab a marine as a shield
        const XMFLOAT3 chest = {
            bandit->position.x,
            bandit->position.y + bandit->footOffset + 1.1f,
            bandit->position.z };
        const XMVECTOR toTarget = XMLoadFloat3(&chest) - eye;
        const float forward = XMVectorGetX(XMVector3Dot(toTarget, front));
        if (forward < 0.35f || forward > 4.5f) continue;
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(toTarget));
        const float sideSq = (std::max)(0.0f, distanceSq - forward * forward);
        if (sideSq > 0.85f * 0.85f || !GrabPathClear(chest)) continue;
        const float score = sideSq * 4.0f + forward * 0.05f;
        if (score < bestScore) {
            bestScore = score;
            best = bandit.get();
        }
    }
    if (best) {
        best->SetHeld(true);
        g_heldBandit = best;
    }
}

static ExplosiveBarrel* HeldBarrel() {
    if (g_heldBarrelIndex >= scene.explosiveBarrels.size()) return nullptr;
    ExplosiveBarrel& barrel = scene.explosiveBarrels[g_heldBarrelIndex];
    return barrel.active && barrel.held ? &barrel : nullptr;
}

static bool EnsureExplosiveBarrelBody(ExplosiveBarrel& barrel) {
    if (!g_destruction.IsInitialized()) return false;
    DestructionBodyPose pose;
    if (barrel.physicsHandle != 0 &&
        g_destruction.GetExplosiveBarrelPose(barrel.physicsHandle, pose))
        return true;
    barrel.physicsHandle =
        g_destruction.CreateExplosiveBarrelBody(barrel.position);
    return barrel.physicsHandle != 0;
}

static void DestroyExplosiveBarrelBody(ExplosiveBarrel& barrel) {
    if (barrel.physicsHandle != 0)
        g_destruction.DestroyExplosiveBarrelBody(barrel.physicsHandle);
    barrel.physicsHandle = 0;
}

static void ThrowHeldBarrel() {
    ExplosiveBarrel* barrel = HeldBarrel();
    if (!barrel) {
        g_heldBarrelIndex = SIZE_MAX;
        return;
    }
    XMVECTOR direction = XMVector3Normalize(
        XMLoadFloat3(&scene.camera.Front) + XMVectorSet(0.0f, 0.14f, 0.0f, 0.0f));
    XMVECTOR velocity = direction * 17.5f + XMVectorSet(0.0f, 1.8f, 0.0f, 0.0f);
    XMStoreFloat3(&barrel->velocity, velocity);
    barrel->held = false;
    barrel->thrown = true;
    g_heldBarrelIndex = SIZE_MAX;
}

static void GrabOrThrowObject() {
    if (HeldGrenade()) {
        ThrowHeldGrenade();
        return;
    }
    if (HeldBarrel()) {
        ThrowHeldBarrel();
        return;
    }
    if (g_heldBandit && g_heldBandit->Held()) {
        GrabOrThrowBandit();
        return;
    }

    // Live hostile grenades get first refusal: catching one is timing-sensitive,
    // while barrels and actors remain available after it passes.
    if (GrabEnemyGrenade()) return;

    const XMVECTOR eye = XMLoadFloat3(&scene.camera.Position);
    const XMVECTOR front = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    size_t best = SIZE_MAX;
    float bestScore = FLT_MAX;
    for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
        const ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
        if (!barrel.active || barrel.held) continue;
        const XMVECTOR toTarget = XMLoadFloat3(&barrel.position) - eye;
        const float forward = XMVectorGetX(XMVector3Dot(toTarget, front));
        if (forward < 0.25f || forward > 3.5f) continue;
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(toTarget));
        const float sideSq = (std::max)(0.0f, distanceSq - forward * forward);
        if (sideSq > 0.75f * 0.75f || !GrabPathClear(barrel.position)) continue;
        const float score = sideSq * 5.0f + forward * 0.05f;
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best != SIZE_MAX) {
        g_heldBandit = nullptr;
        ExplosiveBarrel& barrel = scene.explosiveBarrels[best];
        DestroyExplosiveBarrelBody(barrel);
        barrel.held = true;
        barrel.thrown = false;
        barrel.vortexHoldTime = 0.0f;
        barrel.velocity = { 0.0f, 0.0f, 0.0f };
        barrel.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_heldBarrelIndex = best;
        return;
    }
    GrabOrThrowBandit();
}

static void SpawnBarrelExplosionFX(const XMFLOAT3& center) {
    scene.SpawnExplosionFX(
        { center.x, center.y + 1.1f, center.z }, 5.5f);
}

static void DetonateBarrel(size_t firstBarrel) {
    if (firstBarrel >= scene.explosiveBarrels.size() ||
        !scene.explosiveBarrels[firstBarrel].active) return;

    if (g_heldBarrelIndex == firstBarrel) g_heldBarrelIndex = SIZE_MAX;
    std::vector<size_t> pending{ firstBarrel };
    DestroyExplosiveBarrelBody(scene.explosiveBarrels[firstBarrel]);
    scene.explosiveBarrels[firstBarrel].active = false;
    scene.explosiveBarrels[firstBarrel].held = false;
    scene.explosiveBarrels[firstBarrel].thrown = false;
    scene.explosiveBarrels[firstBarrel].vortexHoldTime = 0.0f;
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const XMFLOAT3 center = scene.explosiveBarrels[pending[cursor]].position;
        AddExplosionTerrainCrater(center);
        SpawnBarrelExplosionFX(center);

        for (auto& bandit : g_bandits) {
            if (bandit) bandit->ApplyExplosion(center, 6.5f, 500.0f, 12.0f);
        }
        PlayBanditDeathEvents();
        if (!g_helicopterDead && g_helicopterModel) {
            const float hx = g_helicopterPosition.x - center.x;
            const float hy = g_helicopterPosition.y - center.y;
            const float hz = g_helicopterPosition.z - center.z;
            const float distance = std::sqrt(hx*hx + hy*hy + hz*hz);
            // Hull hit-sphere is ~5 m, so a barrel bursting on the hull
            // registers as a near-full-strength hit.
            const float reach = 11.5f;
            if (distance < reach)
                DamageHelicopter(120.0f * (1.0f - distance / reach),
                                 g_helicopterPosition);
        }
        if (SecondaryHelicopterPresent() && !g_secondaryHelicopterDead &&
            g_helicopterModel) {
            const float hx = g_secondaryHelicopterPosition.x - center.x;
            const float hy = g_secondaryHelicopterPosition.y - center.y;
            const float hz = g_secondaryHelicopterPosition.z - center.z;
            const float distance = std::sqrt(hx*hx + hy*hy + hz*hz);
            const float reach = 11.5f;
            if (distance < reach)
                DamageSecondaryHelicopter(
                    120.0f * (1.0f - distance / reach),
                    g_secondaryHelicopterPosition);
        }
        // Blast takes the emplacement too, so C4 or a rocket is a valid answer
        // to it rather than the gun being immune to everything but bullets.
        for (size_t i = 0; i < g_game.vehicles.aaTurrets.size(); ++i) {
            if (!g_game.vehicles.aaTurrets[i].Active()) continue;
            const XMFLOAT3 turret = g_game.vehicles.aaTurrets[i].position;
            const float hx = turret.x - center.x;
            const float hy = turret.y - center.y;
            const float hz = turret.z - center.z;
            const float distance = std::sqrt(hx*hx + hy*hy + hz*hz);
            const float reach = 9.0f;
            if (distance < reach)
                DamageAATurret(i, 320.0f * (1.0f - distance / reach), turret);
        }
        if (scene.useDestruction && g_destruction.IsInitialized()) {
            g_destruction.ApplyExplosion(center, 5.0f, 3.0f, 180.0f);
            g_destruction.ApplyRagdollExplosion(center, 6.5f, 110.0f);
        }

        const float px = scene.camera.Position.x - center.x;
        const float py = scene.camera.Position.y - center.y;
        const float pz = scene.camera.Position.z - center.z;
        const float playerDistance = std::sqrt(px*px + py*py + pz*pz);
        if (playerDistance < 6.0f)
            scene.DamagePlayer(85.0f * (1.0f - playerDistance / 6.0f));

        // Nearby barrels chain-react. Mark on enqueue to prevent duplicates.
        for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
            ExplosiveBarrel& other = scene.explosiveBarrels[i];
            if (!other.active) continue;
            const float dx = other.position.x - center.x;
            const float dy = other.position.y - center.y;
            const float dz = other.position.z - center.z;
            if (dx*dx + dy*dy + dz*dz > 4.5f * 4.5f) continue;
            DestroyExplosiveBarrelBody(other);
            other.active = false;
            other.held = false;
            other.thrown = false;
            other.vortexHoldTime = 0.0f;
            if (g_heldBarrelIndex == i) g_heldBarrelIndex = SIZE_MAX;
            pending.push_back(i);
        }
    }
    if (g_game.session.TimerRunning() && !pending.empty()) {
        g_game.mission.RecordDestruction(static_cast<uint32_t>(pending.size()));
        g_game.money.Award(MoneyEvent::PropDestroyed,
                           static_cast<int>(pending.size()));
    }
}

static bool HitExplosiveBarrelSegment(const XMFLOAT3& start,
                                      const XMFLOAT3& end, float radius,
                                      size_t& barrelIndex, XMFLOAT3& hit) {
    if (g_emptyLevelMode) return false;
    const float direction[3] = {
        end.x - start.x, end.y - start.y, end.z - start.z };
    const float origin[3] = { start.x, start.y, start.z };
    float closestT = FLT_MAX;
    bool found = false;
    for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
        const ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
        if (!barrel.active) continue;
        const float center[3] = {
            barrel.position.x, barrel.position.y, barrel.position.z };
        const float extent[3] = {
            0.44f + radius, 0.78f + radius, 0.44f + radius };
        float tMin = 0.0f, tMax = 1.0f;
        bool intersects = true;
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = center[axis] - extent[axis];
            const float hi = center[axis] + extent[axis];
            if (std::abs(direction[axis]) < 1e-6f) {
                if (origin[axis] < lo || origin[axis] > hi) intersects = false;
                continue;
            }
            float a = (lo - origin[axis]) / direction[axis];
            float b = (hi - origin[axis]) / direction[axis];
            if (a > b) std::swap(a, b);
            tMin = (std::max)(tMin, a);
            tMax = (std::min)(tMax, b);
            if (tMin > tMax) { intersects = false; break; }
        }
        if (!intersects || tMin >= closestT) continue;
        closestT = tMin;
        barrelIndex = i;
        found = true;
    }
    if (found) {
        hit = { start.x + direction[0] * closestT,
                start.y + direction[1] * closestT,
                start.z + direction[2] * closestT };
    }
    return found;
}

static void UpdateExplosiveBarrels(float dt) {
    if (g_destruction.IsInitialized())
        g_destruction.DrainExplosiveBarrelImpactEvents();
    for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
        ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
        if (!barrel.active) continue;
        const float previousVortexHoldTime = barrel.vortexHoldTime;
        barrel.vortexHoldTime = (std::max)(
            0.0f, barrel.vortexHoldTime - dt);
        if (barrel.held) {
            DestroyExplosiveBarrelBody(barrel);
            const XMFLOAT3& eye = scene.camera.Position;
            const XMFLOAT3& front = scene.camera.Front;
            barrel.position = {
                eye.x + front.x * 1.85f,
                eye.y + front.y * 1.85f - 0.42f,
                eye.z + front.z * 1.85f };
            barrel.velocity = { 0.0f, 0.0f, 0.0f };
        } else if (barrel.physicsHandle != 0) {
            DestructionBodyPose pose;
            if (g_destruction.GetExplosiveBarrelPose(
                    barrel.physicsHandle, pose)) {
                barrel.position = pose.position;
                barrel.rotation = pose.rotation;
                barrel.velocity = pose.linearVelocity;
            } else {
                barrel.physicsHandle = 0;
            }
            if (barrel.physicsHandle != 0 &&
                barrel.vortexHoldTime <= 0.0f) {
                // Leave Box3D exactly when capture ends. Preserve orbital speed
                // and add the vortex's final radial/upward release impulse; normal
                // barrel flight resumes through the lightweight manual path.
                if (previousVortexHoldTime > 0.0f) {
                    XMVECTOR radial = XMLoadFloat3(&barrel.position) -
                        XMLoadFloat3(&barrel.vortexCenter);
                    if (XMVectorGetX(XMVector3LengthSq(radial)) < 0.0025f)
                        radial = XMVectorSet(1.0f, 0.35f, 0.0f, 0.0f);
                    radial = XMVector3Normalize(radial);
                    XMVECTOR release = XMLoadFloat3(&barrel.velocity) +
                        radial * 9.0f + XMVectorSet(0.0f, 3.0f, 0.0f, 0.0f);
                    XMStoreFloat3(&barrel.velocity, release);
                }
                DestroyExplosiveBarrelBody(barrel);
            }
        } else if (barrel.thrown) {
            const XMFLOAT3 previous = barrel.position;
            barrel.velocity.y -= 9.81f * dt;
            barrel.position.x += barrel.velocity.x * dt;
            barrel.position.y += barrel.velocity.y * dt;
            barrel.position.z += barrel.velocity.z * dt;

            bool impact = false;
            XMFLOAT3 impactPoint = barrel.position;
            if (scene.useMeshTerrain && g_terrain.supported) {
                auto params = CurrentTerrainParams();
                params.heightScale = scene.terrainHeightScale;
                const float ground = TerrainRendererDX12::HeightAt(
                    params, barrel.position.x, barrel.position.z);
                if (barrel.position.y <= ground + 0.75f) {
                    barrel.position.y = ground + 0.75f;
                    impact = true;
                }
            } else if (barrel.position.y <= 0.75f) {
                barrel.position.y = 0.75f;
                impact = true;
            }
            if (!impact && g_destruction.IsInitialized())
                impact = g_destruction.HitTestSegment(
                    previous, barrel.position, 0.44f, impactPoint);
            // Thrown barrels detonate on the helicopter's hull.
            if (!impact &&
                HitHelicopterSegment(previous, barrel.position, 0.44f, impactPoint))
                impact = true;
            if (!impact && HitSecondaryHelicopterSegment(
                    previous, barrel.position, 0.44f, impactPoint))
                impact = true;
            if (!impact && HitBoatSegment(
                    previous, barrel.position, 0.44f, impactPoint))
                impact = true;
            if (!impact && g_banditLoaded) {
                for (const auto& bandit : g_bandits) {
                    if (bandit && !bandit->Dead() && bandit->BlocksProjectile(
                            previous, barrel.position, 0.44f, &impactPoint)) {
                        impact = true;
                        break;
                    }
                }
            }
            if (impact) {
                barrel.position = impactPoint;
                DetonateBarrel(i);
                continue;
            }
        }
        if (!barrel.burning) continue;
        barrel.fuse -= dt;
        barrel.fireFxCooldown -= dt;
        if (barrel.fuse <= 0.0f) {
            DetonateBarrel(i);
            continue;
        }
        if (barrel.fireFxCooldown > 0.0f) continue;
        barrel.fireFxCooldown = 0.11f;

        const XMFLOAT3 flameBase = {
            barrel.position.x, barrel.position.y + 0.78f, barrel.position.z };
        auto randomSigned = []() {
            return ((float)std::rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        };
        for (int sparkIndex = 0; sparkIndex < 2; ++sparkIndex) {
            ImpactParticle flame;
            flame.position = {
                flameBase.x + randomSigned() * 0.12f,
                flameBase.y,
                flameBase.z + randomSigned() * 0.12f };
            flame.velocity = {
                randomSigned() * 0.35f,
                3.0f + std::abs(randomSigned()) * 1.8f,
                randomSigned() * 0.35f };
            flame.maxLife = flame.life = 0.18f + std::abs(randomSigned()) * 0.14f;
            flame.size = 0.045f + std::abs(randomSigned()) * 0.055f;
            flame.growth = -0.08f;
            flame.color = { 1.0f, 0.20f + std::abs(randomSigned()) * 0.28f, 0.01f };
            flame.spark = true;
            scene.impactParticles.push_back(flame);
        }
        if ((std::rand() % 4) == 0)
            scene.SpawnSmokeBurst(flameBase, 0.12f, 0.12f);
    }
}

extern PalmTrees g_trees;

static void UpdateMolotovFireDamage() {
    constexpr float tickSeconds = 0.25f;

    for (auto material = scene.burningMaterials.begin();
         material != scene.burningMaterials.end();) {
        const auto entity = std::find_if(
            g_game.world.Level().entities.begin(),
            g_game.world.Level().entities.end(),
            [material](const LevelEntity& value) {
                return value.id == material->entityId;
            });
        if (entity == g_game.world.Level().entities.end() || !entity->enabled) {
            material = scene.burningMaterials.erase(material);
            continue;
        }

        if (scene.burningTargets.size() < 32) {
            const float fade = (std::min)(1.0f, material->life / 0.65f);
            scene.burningTargets.push_back({
                material->position, material->size, fade,
                material->maxLife - material->life });
        }

        if (material->damageCooldown > 0.0f) {
            ++material;
            continue;
        }
        material->damageCooldown = tickSeconds;
        // Fire spreads and burns unattended, so it is not a player action even
        // when the player lit it. The comm tower is a player-only objective and
        // must not burn down on its own; stop tracking it as a burning material.
        // The aircraft objective is player-only for the same reason and must not
        // burn down unattended either.
        {
            XMFLOAT3 ignoredBase{};
            if (FindCommTower(material->entityId, ignoredBase) ||
                IsObjectivePlaneEntity(material->entityId)) {
                material = scene.burningMaterials.erase(material);
                continue;
            }
        }
        const CombatSystem::PrefabDamageResult result =
            g_game.combat.DamagePrefab(
                g_game.world, material->entityId,
                scene.molotovMaterialDamagePerSecond * tickSeconds,
                material->position);
        if (!result.applied) {
            material = scene.burningMaterials.erase(material);
            continue;
        }
        if (result.destroyed) {
            scene.SpawnSmokeBurst(material->position, 1.35f, 0.55f);
            g_prefabRebuildRequested = true;
            g_prefabRebuildReason = "fire destroyed prefab";
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                "Fire destroyed prefab entity " +
                std::to_string(material->entityId));
            material = scene.burningMaterials.erase(material);
            continue;
        }
        ++material;
    }

    if (scene.firePatches.empty() || scene.molotovDamageCooldown > 0.0f)
        return;
    scene.molotovDamageCooldown = tickSeconds;
    std::unordered_set<SkinnedEnemy*> testedBandits;
    std::unordered_set<uint64_t> destructibleIds;
    for (const PrefabDestructibleInstance& destructible :
         g_prefabDestructibles)
        destructibleIds.insert(destructible.entityId);
    bool playerIgnited = false;
    for (FirePatch& fire : scene.firePatches) {
        if (fire.life <= 0.0f) continue;
        const float reach = fire.radius + 0.58f;

        if (g_banditLoaded) {
            for (auto& bandit : g_bandits) {
                if (!bandit || bandit->Dead() ||
                    testedBandits.find(bandit.get()) != testedBandits.end())
                    continue;
                const float dx = bandit->position.x - fire.position.x;
                const float dz = bandit->position.z - fire.position.z;
                const float dy = bandit->position.y + bandit->footOffset +
                    1.0f - fire.position.y;
                if (std::abs(dy) >= 2.3f ||
                    dx * dx + dz * dz >= reach * reach) continue;
                bandit->Ignite(5.5f);
                testedBandits.insert(bandit.get());
            }
        }

        const float playerX = scene.camera.Position.x - fire.position.x;
        const float playerZ = scene.camera.Position.z - fire.position.z;
        const float playerHeight = std::abs(
            scene.camera.Position.y - fire.position.y);
        if (!playerIgnited && playerHeight < 2.2f &&
            playerX * playerX + playerZ * playerZ < reach * reach) {
            scene.playerBurnTime = (std::max)(scene.playerBurnTime, 3.2f);
            playerIgnited = true;
        }

        if (!g_emptyLevelMode) {
            if (scene.useDestruction && g_destruction.IsInitialized() &&
                fire.structureDamageCooldown <= 0.0f) {
                const XMFLOAT3 materialHit{
                    fire.position.x, fire.position.y + 0.65f,
                    fire.position.z };
                g_destruction.ApplyRadialDamage(
                    materialHit, reach + 0.35f,
                    scene.molotovMaterialDamagePerSecond * tickSeconds,
                    /*sparesProtected=*/true);   // spreading fire is indirect
                g_destruction.IgniteChunkAt(materialHit);
                fire.structureDamageCooldown = 1.25f;
            }
            g_trees.IgniteNear(fire.position, reach + 0.8f);
            // Deliberately the bounds box, not the triangle mesh, and so this
            // loop does not filter g_meshCollisionEntities. Per-triangle
            // proximity would cost a tree walk per fire per tick to decide
            // whether a fire near a wall is "close enough" to spread -- a
            // question the coarse volume already answers well enough to look
            // right.
            for (const PrefabCollider& collider : g_prefabColliders) {
                if (destructibleIds.find(collider.entityId) ==
                    destructibleIds.end()) continue;
                const XMFLOAT3 local =
                    PrefabColliderToLocal(collider, fire.position);
                const float outsideX = (std::max)(
                    std::abs(local.x) - collider.halfExtents.x, 0.0f);
                const float outsideY = (std::max)(
                    std::abs(local.y) - collider.halfExtents.y, 0.0f);
                const float outsideZ = (std::max)(
                    std::abs(local.z) - collider.halfExtents.z, 0.0f);
                if (outsideX * outsideX + outsideY * outsideY +
                    outsideZ * outsideZ > reach * reach) continue;
                // Never set the comm tower alight: fire cannot damage it (it is
                // a player-only objective), so burning it would only paint
                // flames on a mast that never loses health.
                XMFLOAT3 ignoredTowerBase{};
                if (FindCommTower(collider.entityId, ignoredTowerBase)) continue;
                const XMFLOAT3 flamePosition{
                    fire.position.x,
                    fire.position.y + 0.62f,
                    fire.position.z };
                scene.IgniteMaterial(
                    collider.entityId, flamePosition,
                    (std::min)(2.1f, 1.15f + fire.radius * 0.55f));
            }
            for (ExplosiveBarrel& barrel : scene.explosiveBarrels) {
                if (!barrel.active || barrel.burning) continue;
                const float dx = barrel.position.x - fire.position.x;
                const float dz = barrel.position.z - fire.position.z;
                const float barrelReach = reach + 0.5f;
                if (dx * dx + dz * dz > barrelReach * barrelReach) continue;
                barrel.burning = true;
                barrel.fuse = 3.0f;
                barrel.fireFxCooldown = 0.0f;
            }
        }
    }
}

// One-line Bandit status for the debug HUD (declared in EngineUI.h).
void BanditDebugText() {
    if (!g_banditLoaded) { ImGui::Text("Bandit: NOT LOADED"); return; }
    int textured = 0, parts = 0;
    if (g_banditModel.node && g_banditModel.node->mesh) {
        for (const auto& p : g_banditModel.node->mesh->primitives) {
            ++parts;
            if (p.material && p.material->baseColorTexture) ++textured;
        }
    }
    ImGui::Text("Bandits: live=%zu marines=%zu total=%zu bones=%zu parts=%d tex=%d",
                LiveBanditCount(), LiveMarineCount(), g_bandits.size(),
                g_banditModel.skeleton.BoneCount(), parts, textured);
    // TEMP DEBUG: per-marine firing gate state.
    for (const auto& actor : g_bandits) {
        if (!actor || actor->faction != Faction::Marine || actor->Dead()) continue;
        const char* state =
            actor->Awareness() == SkinnedEnemy::AwarenessState::Combat ? "COMBAT"
            : actor->Awareness() == SkinnedEnemy::AwarenessState::Alert ? "Alert"
            : "Patrol";
        ImGui::Text("  marine %s cd=%.2f prep=%d aim=%.2f cover=%d/%d pose=%d burst=%d",
                    state, actor->fireCooldown, (int)actor->DebugPreparingShot(),
                    actor->DebugStationaryAimTime(), (int)actor->DebugHasCoverTarget(),
                    (int)actor->DebugInCover(), (int)actor->DebugHasGunPose(),
                    actor->DebugBurstShots());
    }
    ImGui::Text("Weapon: %s  (mouse wheel)", GunModel::SelectedWeaponName());
    if (GunModel::C4Selected())
        ImGui::Text("C4: LMB throw | RMB detonate | armed=%zu",
                    scene.remoteCharges.size());
    ImGui::SliderFloat("Head/torso yaw offset",
                       &g_banditHeadYawOffsetDegrees,
                       -90.0f, 90.0f, "%.1f deg");

    if (ImGui::CollapsingHeader("Rifle grip")) {
        ImGui::TextDisabled("Trigger hand (from the shoulder)");
        ImGui::SliderFloat("Rear forward", &g_banditGunRearGripForward,
                           -0.20f, 0.50f, "%.3f m");
        ImGui::SliderFloat("Rear inboard", &g_banditGunRearGripInboard,
                           -0.30f, 0.30f, "%.3f m");
        ImGui::SliderFloat("Rear drop", &g_banditGunRearGripDrop,
                           -0.50f, 0.20f, "%.3f m");

        ImGui::TextDisabled("Support hand (from the trigger hand)");
        ImGui::SliderFloat("Fore reach", &g_banditLeftArmReach,
                           0.20f, 1.10f, "%.3f m");
        ImGui::SliderFloat("Fore lateral", &g_banditGunForeGripLateral,
                           -0.30f, 0.30f, "%.3f m");
        ImGui::SliderFloat("Fore rise", &g_banditGunForeGripRise,
                           -0.30f, 0.30f, "%.3f m");

        ImGui::TextDisabled("Mesh seating in the hands");
        ImGui::SliderFloat("Gun along barrel", &g_banditGunGripForward,
                           -0.40f, 0.60f, "%.3f m");
        ImGui::SliderFloat("Gun rise", &g_banditGunGripRise,
                           -0.30f, 0.30f, "%.3f m");
        ImGui::SliderFloat("Gun scale", &g_banditGunScale,
                           0.20f, 1.50f, "%.2f x");
        if (ImGui::Button("Reset rifle grip")) {
            g_banditLeftArmReach = 0.85f;
            g_banditGunScale = 0.62f;
            g_banditGunGripForward = -0.183f;
            g_banditGunGripRise = -0.04f;
            g_banditGunRearGripForward = 0.16f;
            g_banditGunRearGripInboard = -0.06f;
            g_banditGunRearGripDrop = -0.18f;
            g_banditGunForeGripLateral = 0.253f;
            g_banditGunForeGripRise = -0.206f;
        }
    }
    ImGui::Checkbox("Show enemy vision cones", &g_showEnemyVisionCones);
    if (ImGui::CollapsingHeader("Impact Holes")) {
        if (ImGui::Checkbox("Enable bullet holes",
                            &g_impactDecalsEnabled) &&
            !g_impactDecalsEnabled) {
            g_impactDecals.clear();
            g_impactDecalCutouts = false;
            g_showDecalDebug = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("cutout only; off by default");
        ImGui::BeginDisabled(!g_impactDecalsEnabled);
        ImGui::Text("Active: %d / %d", (int)g_impactDecals.size(),
                    (int)kMaxImpactDecals);
        // The buffer evicts oldest-first, so a full list means marks are being
        // dropped -- worth seeing before wondering why an old one vanished.
        if (g_impactDecals.size() >= kMaxImpactDecals)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Full: oldest marks are being evicted");
        ImGui::Checkbox("Draw decal volumes", &g_showDecalDebug);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Green disc = the surface plane the shader tests against, "
                "drawn in world space so orientation errors are visible. "
                "Blue line = projection normal, at the 0.6 x radius depth "
                "band. Amber = already fading.");
        ImGui::Checkbox("Cut holes through surfaces", &g_impactDecalCutouts);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Experimental visual cutout. Removes primary visibility but "
                "does not change collision or ray-tracing geometry.");
        ImGui::Checkbox("Freeze aging", &g_freezeDecalAging);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Stops the 45 s lifetime so a mark can be walked around "
                "and inspected without fading mid-investigation.");
        if (ImGui::Button("Clear all holes")) g_impactDecals.clear();
        ImGui::SameLine();
        // Spawns one straight ahead: reproduces a mark without needing a
        // weapon, ammo, or a surface the destruction system reports a hit on.
        if (ImGui::Button("Spawn test hole")) {
            const XMFLOAT3 origin = scene.camera.Position;
            const XMFLOAT3 forward = scene.camera.Front;
            const XMFLOAT3 spot{ origin.x + forward.x * 2.0f,
                                 origin.y + forward.y * 2.0f,
                                 origin.z + forward.z * 2.0f };
            SpawnImpactDecal(spot,
                             XMFLOAT3(-forward.x, -forward.y, -forward.z),
                             0.12f);
        }
        if (!g_impactDecals.empty() &&
            ImGui::TreeNode("Hole list")) {
            int index = 0;
            for (const ImpactDecal& decal : g_impactDecals) {
                ImGui::Text("%2d  pos %.2f %.2f %.2f  n %.2f %.2f %.2f",
                            index, decal.position.x, decal.position.y,
                            decal.position.z, decal.normal.x, decal.normal.y,
                            decal.normal.z);
                ImGui::SameLine();
                ImGui::TextDisabled("r%.3f  %.1fs", decal.radius, decal.age);
                ++index;
            }
            ImGui::TreePop();
        }
        ImGui::EndDisabled();
    }
    if (ImGui::CollapsingHeader("Enemy Scatter (test mode)")) {
        ImGui::Checkbox("Randomize enemies on navmesh",
                        &g_scatterEnemiesOnNavmesh);
        ImGui::TextWrapped(
            "Moves every bandit to a random walkable navmesh point when the "
            "squad spawns, before deployment. Ignores authored EnemySpawn "
            "placement. Turret gunners and marines are left alone.");
        int seed = static_cast<int>(g_scatterEnemiesSeed);
        if (ImGui::InputInt("Seed (0 = random)", &seed))
            g_scatterEnemiesSeed = static_cast<unsigned int>((std::max)(0, seed));
        if (g_scatterEnemiesLastSeed != 0) {
            ImGui::Text("Last scatter seed: %u", g_scatterEnemiesLastSeed);
            ImGui::SameLine();
            // Pins a clock-seeded layout after the fact, so an interesting
            // random run can be replayed instead of being lost on restart.
            if (ImGui::SmallButton("Pin"))
                g_scatterEnemiesSeed = g_scatterEnemiesLastSeed;
        }
        // Re-scatters the squad already on the field, without a restart.
        if (ImGui::Button("Scatter now")) {
            const bool wasEnabled = g_scatterEnemiesOnNavmesh;
            g_scatterEnemiesOnNavmesh = true;
            ScatterEnemiesOnNavmesh();
            g_scatterEnemiesOnNavmesh = wasEnabled;
        }
    }
    if (ImGui::CollapsingHeader("Enemy Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderFloat("Flesh hit pitch min", &g_fleshHitPitchMin,
                               0.5f, 2.0f, "%.2f"))
            g_fleshHitPitchMin = (std::min)(g_fleshHitPitchMin, g_fleshHitPitchMax);
        if (ImGui::SliderFloat("Flesh hit pitch max", &g_fleshHitPitchMax,
                               0.5f, 2.0f, "%.2f"))
            g_fleshHitPitchMax = (std::max)(g_fleshHitPitchMax, g_fleshHitPitchMin);
    }
}
