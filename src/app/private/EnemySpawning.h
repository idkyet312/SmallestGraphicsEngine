#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static float RandomUnit() {
    return (float)std::rand() / (float)RAND_MAX;
}

// Squad composition. Most bandits keep the rifle; the specialists are rare
// because each one changes how a whole fight plays -- a shotgunner forces the
// player to back off, a sniper forces them into cover.
static constexpr float kBanditShotgunShare = 0.22f;
static constexpr float kBanditSniperShare  = 0.16f;

// Shotgun: a tight cone of pellets, lethal up close and nearly harmless past a
// few metres because each pellet is individually weak and the spread is wide.
static constexpr int   kBanditShotgunPellets = 7;
static constexpr float kBanditShotgunSpread = 0.075f;
// Combat ring for each class. The shotgunner has to close to be a threat; the
// sniper stays out past rifle range where its long telegraph is survivable.
static constexpr float kBanditShotgunOrbitRadius = 2.6f;
static constexpr float kBanditSniperOrbitRadius = 26.0f;

static BanditWeapon PickBanditWeapon() {
    const float roll = RandomUnit();
    if (roll < kBanditSniperShare) return BanditWeapon::Sniper;
    if (roll < kBanditSniperShare + kBanditShotgunShare)
        return BanditWeapon::Shotgun;
    return BanditWeapon::Rifle;
}

// Per-spawner loadout, read from the entity's `overrides` blob under
// "enemyWeapon". Absent or unrecognised means "random", which is the behaviour
// every level had before this was authorable -- so old levels are unchanged and
// a hand-edited typo degrades to a random pick rather than refusing to spawn.
static std::optional<BanditWeapon> AuthoredSpawnWeapon(const LevelEntity& entity) {
    const auto found = entity.overrides.find("enemyWeapon");
    if (found == entity.overrides.end() || !found->is_string())
        return std::nullopt;
    BanditWeapon weapon = BanditWeapon::Rifle;
    if (!ParseBanditWeapon(found->get<std::string>(), weapon))
        return std::nullopt;
    return weapon;
}

// Applies the loadout's movement profile. Called after the spawn code has set
// its own orbit radius so the class choice wins.
// `forced` pins the class (an authored spawner asked for it); std::nullopt keeps
// the original random roll, which is what every unauthored spawn still uses.
static void ApplyBanditLoadout(SkinnedEnemy& bandit,
                               std::optional<BanditWeapon> forced = std::nullopt) {
    bandit.weapon = forced ? *forced : PickBanditWeapon();
    if (bandit.weapon == BanditWeapon::Shotgun) {
        bandit.orbitRadius = kBanditShotgunOrbitRadius;
        // Aggressive closer: it only ever gets to shoot if it reaches the player.
        bandit.moveSpeed *= 1.25f;
        bandit.health = 130.0f;
    } else if (bandit.weapon == BanditWeapon::Sniper) {
        bandit.orbitRadius = kBanditSniperOrbitRadius;
        // Holds the line and repositions slowly between shots.
        bandit.moveSpeed *= 0.7f;
        bandit.health = 80.0f;
    }
}

// Distance at which an enemy voice fades out. Matches the 38 m the old manual
// falloff used, so voices carry exactly as far as they always did -- only the
// direction is new.
static constexpr float kBanditVoiceRange = 38.0f;

static float BanditVoiceVolume(const XMFLOAT3& position, float peak = 0.78f) {
    const float dx = position.x - scene.camera.Position.x;
    const float dy = position.y - scene.camera.Position.y;
    const float dz = position.z - scene.camera.Position.z;
    const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
    return (std::max)(0.06f, peak * (1.0f - distance / 38.0f));
}

static void PlayBanditDeathEvents() {
    for (auto& bandit : g_bandits) {
        if (!bandit || !bandit->ConsumeDeathEvent()) continue;
        // Payout rides on the same one-shot death event the audio uses, so a
        // body cannot be paid for twice however it was killed -- rifle, rotor,
        // debris and fire all funnel through here.
        if (g_game.session.TimerRunning()) {
            g_game.money.Award(bandit->faction == Faction::Marine
                                   ? MoneyEvent::FriendlyLost
                                   : MoneyEvent::EnemyKilled);
        }
        const float pitch = 0.94f + ((float)std::rand() / RAND_MAX) * 0.10f;
        g_banditDeathAudio.PlayAt(bandit->position.x, bandit->position.y,
                                  bandit->position.z, 0.9f, pitch,
                                  kBanditVoiceRange);
        // Training range: the guard is the whole exercise, so his death is the
        // cue for the next order. Guarded so a map that later gains more enemies
        // does not stack the callout once per kill.
        if (g_trainingRangeMode && !g_plantC4Played) {
            g_plantC4TowerDelay = kPlantC4TowerDelay;
            g_plantC4Played = true;
        }
    }
}

static bool SpawnBandit() {
    if (!g_banditModel.valid) return false;
    const size_t activeSlots = ActiveBanditSlotCount();
    size_t slot = activeSlots;
    for (size_t candidate = 0; candidate < activeSlots; ++candidate) {
        bool occupied = false;
        for (const auto& existing : g_bandits) {
            if (existing && !existing->Dead() &&
                existing->spawnSlot == static_cast<int>(candidate)) {
                occupied = true;
                break;
            }
        }
        if (!occupied) { slot = candidate; break; }
    }
    if (slot == activeSlots) return false;

    auto bandit = std::make_unique<SkinnedEnemy>();
    if (!bandit->Init(g_banditModel)) return false;
    // Stagger the first grenade across the whole cooldown window so a squad
    // spawning together does not throw its opening volley in lockstep.
    bandit->grenadeCooldown = kBanditGrenadeCooldownMin +
        RandomUnit() * (kBanditGrenadeCooldownMax - kBanditGrenadeCooldownMin);

    if (g_customLevelMode) {
        size_t spawnIndex = 0;
        for (const LevelEntity& entity : g_game.world.Level().entities) {
            if (!entity.enabled || entity.type != LevelEntityType::EnemySpawn) continue;
            if (spawnIndex++ != slot) continue;
            bandit->position = { entity.transform.position[0],
                                 entity.transform.position[1],
                                 entity.transform.position[2] };
            // Settle an elevated spawn onto the prop beneath it, so a bandit
            // authored above a watchtower deck stands on the deck rather than
            // being snapped to terrain by its first update.
            float spawnSurfaceY = 0.0f;
            if (PrefabSurfaceSupports(bandit->position, bandit->position.y,
                                      kBanditPrefabSpawnDrop, spawnSurfaceY) &&
                spawnSurfaceY > GroundHeightAt(bandit->position.x,
                                               bandit->position.z))
                bandit->position.y = spawnSurfaceY;
            bandit->yaw = XMConvertToRadians(entity.transform.rotation[1]);
            bandit->modelScale *= entity.transform.scale[0];
            bandit->spawnSlot = static_cast<int>(slot);
            bandit->leftArmReach = g_banditLeftArmReach;
            bandit->orbitRadius = 4.4f + static_cast<float>(slot % 4) * 0.45f;
            bandit->orbitDirection = (slot & 1) ? -1.0f : 1.0f;
            bandit->fireCooldown = 0.7f +
                ((float)std::rand() / (float)RAND_MAX) * 2.8f;
            ApplyBanditLoadout(*bandit, AuthoredSpawnWeapon(entity));
            bandit->PlayClip("Walk");
            g_bandits.push_back(std::move(bandit));
            return true;
        }
        for (const PrefabSpawnPoint& spawn : g_prefabSpawnPoints) {
            if (spawn.enemyType != "bandit") continue;
            for (uint32_t member = 0; member < spawn.count; ++member, ++spawnIndex) {
                if (spawnIndex != slot) continue;
                const float angle = spawn.yawRadians + member * 2.3999632f;
                const float radius = member == 0 ? 0.0f : 0.8f * std::sqrt(
                    static_cast<float>(member));
                bandit->position = { spawn.position.x + std::cos(angle) * radius,
                    spawn.position.y, spawn.position.z + std::sin(angle) * radius };
                bandit->yaw = spawn.yawRadians;
                bandit->spawnSlot = static_cast<int>(slot);
                bandit->leftArmReach = g_banditLeftArmReach;
                bandit->orbitRadius = 4.4f + static_cast<float>(slot % 4) * 0.45f;
                bandit->orbitDirection = (slot & 1) ? -1.0f : 1.0f;
                bandit->fireCooldown = 0.7f +
                    ((float)std::rand() / (float)RAND_MAX) * 2.8f;
                ApplyBanditLoadout(*bandit);
                bandit->PlayClip("Walk");
                g_bandits.push_back(std::move(bandit));
                return true;
            }
        }
        return false;
    }

    const size_t spawner = slot / kEnemiesPerSpawner;
    const size_t member = slot % kEnemiesPerSpawner;
    const CompoundCenter& compound =
        kStressCompoundCenters[spawner / kSpawnersPerCompound];
    static constexpr float outwardX[kSpawnersPerCompound] = {
         0.0f, 1.0f, 0.0f, -1.0f };
    static constexpr float outwardZ[kSpawnersPerCompound] = {
         1.0f, 0.0f, -1.0f, 0.0f };
    const size_t sideIndex = spawner % kSpawnersPerCompound;
    const XMFLOAT3 spawn{
        compound.x + outwardX[sideIndex] * 17.5f,
        0.0f,
        compound.z + outwardZ[sideIndex] * 17.5f };
    const float side = member == 0 ? -0.85f : 0.85f;
    bandit->position = {
        spawn.x - outwardZ[sideIndex] * side,
        spawn.y,
        spawn.z + outwardX[sideIndex] * side
    };
    bandit->spawnSlot = static_cast<int>(slot);
    bandit->leftArmReach = g_banditLeftArmReach;
    bandit->orbitRadius = 4.4f + static_cast<float>(slot % 4) * 0.45f;
    bandit->orbitDirection = (slot & 1) ? -1.0f : 1.0f;
    bandit->fireCooldown =
        0.7f + ((float)std::rand() / (float)RAND_MAX) * 2.8f;
    ApplyBanditLoadout(*bandit);
    bandit->PlayClip("Walk");
    bandit->anim.Advance(0.19f * static_cast<float>(g_banditSpawnSerial++ % 8));
    g_bandits.push_back(std::move(bandit));
    return true;
}

// Drops one marine at the given position/yaw. No slot rotation, no respawn --
// one-shot per AllySpawn level entity, called once at level load.
static bool SpawnMarine(const XMFLOAT3& position, float yaw) {
    if (!g_marineModel.valid) return false;
    auto marine = std::make_unique<SkinnedEnemy>();
    if (!marine->Init(g_marineModel)) return false;
    marine->faction = Faction::Marine;
    // Fireteam callsign, numbered in spawn order: Bravo-1, Bravo-2, ...
    // Counted from the marines already alive rather than a static counter, so a
    // level restart starts again at Bravo-1 instead of climbing every reload.
    {
        int squadNumber = 1;
        for (const auto& existing : g_bandits)
            if (existing && existing->faction == Faction::Marine) ++squadNumber;
        char callsign[32];
        std::snprintf(callsign, sizeof(callsign), "Bravo-%d", squadNumber);
        marine->callsign = callsign;
    }
    marine->position = position;
    marine->yaw = yaw;
    marine->leftArmReach = g_banditLeftArmReach;
    // 100 health like a rifle bandit, but taking a quarter damage -- so an ally
    // soaks four bars' worth of fire while every rule tuned around a 100 max
    // still reads correctly (the cover-hold term below, and anything comparing
    // against peakHealth). This replaces a flat 400 health, which broke exactly
    // those comparisons.
    //
    // A headshot still kills outright: the reduction is meant to keep allies
    // alive under sustained fire, not to make them bulletproof.
    marine->health = 100.0f;
    marine->damageTakenScale = 0.25f;
    marine->fireCooldown = 0.7f + ((float)std::rand() / (float)RAND_MAX) * 2.8f;
    // Stagger the first grenade across the whole window so a squad spawning
    // together does not throw its opening volley in lockstep.
    marine->grenadeCooldown = kMarineGrenadeCooldownMin +
        RandomUnit() * (kMarineGrenadeCooldownMax - kMarineGrenadeCooldownMin);
    // No ApplyBanditLoadout(): marines stay on the default Rifle loadout for v1.
    marine->PlayClip("Walk");
    g_bandits.push_back(std::move(marine));
    g_game.mission.RecordFriendlyDeployed();
    return true;
}

// Drops one reinforcement bandit out of the dropship at `position`. Unlike
// SpawnBandit() this takes no spawn slot: reinforcements are not part of the
// level's authored EnemySpawn rotation, so they neither consume a slot nor
// respawn when killed. spawnSlot = -1 keeps them out of the slot search, the
// same convention the turret gunners use.
static bool SpawnDropshipBandit(const XMFLOAT3& position) {
    if (!g_banditModel.valid) return false;
    auto bandit = std::make_unique<SkinnedEnemy>();
    if (!bandit->Init(g_banditModel)) return false;
    bandit->position = position;
    bandit->spawnSlot = -1;
    bandit->leftArmReach = g_banditLeftArmReach;
    // Stagger the first grenade across the whole cooldown window so a squad
    // roping down together does not throw its opening volley in lockstep.
    bandit->grenadeCooldown = kBanditGrenadeCooldownMin +
        RandomUnit() * (kBanditGrenadeCooldownMax - kBanditGrenadeCooldownMin);
    bandit->orbitRadius = 4.4f + RandomUnit() * 1.8f;
    bandit->orbitDirection = (g_banditSpawnSerial & 1) ? -1.0f : 1.0f;
    // Longer than a slot spawn's opening delay. A squad that lands already
    // firing gives the player no window to react to the drop itself.
    bandit->fireCooldown = 1.6f + RandomUnit() * 1.6f;
    ApplyBanditLoadout(*bandit);
    // Reinforcements are called in because the player was seen, so they arrive
    // already hunting rather than walking a patrol they never had. The real
    // line-of-sight test still gates every shot -- this only skips the vision
    // cone that a squad briefed on the player's position would not need.
    bandit->ForceCombatTarget(scene.camera.Position);
    // Onto the rope at the craft's altitude. UpdateRappel walks them down to the
    // terrain and hands them to the AI on touchdown -- until then they cannot
    // shoot, take cover, or throw.
    const float facing = std::atan2(scene.camera.Position.x - position.x,
                                    scene.camera.Position.z - position.z);
    bandit->BeginRappel(position, facing);
    bandit->PlayClip("Idle");
    bandit->anim.Advance(0.19f * static_cast<float>(g_banditSpawnSerial++ % 8));
    g_bandits.push_back(std::move(bandit));
    return true;
}

// Data-driven only: spawns one marine per enabled AllySpawn entity in the
// current level. Levels with none (including the non-custom default level)
// get zero marines -- mirrors how EnemySpawn entities drive SpawnBandit()'s
// custom-level branch.
static void SpawnMarinesFromLevel() {
    if (!g_customLevelMode) return;
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (!entity.enabled || entity.type != LevelEntityType::AllySpawn) continue;
        const XMFLOAT3 position{ entity.transform.position[0],
                                 entity.transform.position[1],
                                 entity.transform.position[2] };
        SpawnMarine(position, XMConvertToRadians(entity.transform.rotation[1]));
    }
}

// Puts the marines the player loaded onto the transport on the ground, once
// that transport has actually delivered them. Driven off the one-frame flag the
// release code raises, so a crashed helicopter or a sunk boat simply never
// reaches here and the whole squad is lost with it -- which is the entire point
// of choosing a number on the deploy screen.
//
// Placed on a ring around the drop point rather than at it: the player is
// standing there, and stacking eight actors on the same metre leaves them
// shoving each other apart for the first few seconds of the run.
static void DropDeploymentMarines() {
    if (!g_marineDropPending) return;
    g_marineDropPending = false;
    const int requested = (std::min)(g_deploymentMarineCount,
                                     kMaxDeploymentMarines);
    if (requested <= 0) return;
    if (!g_marineModel.valid) {
        SGE_LOG("LogGameplay", EngineLog::Level::Warning,
            "Marine drop skipped: ally model unavailable");
        return;
    }

    // Wide enough that the ring clears the transport's own hull, tight enough
    // that the squad still reads as having come off it together.
    constexpr float kRingRadius = 4.5f;
    int dropped = 0;
    for (int index = 0; index < requested; ++index) {
        const float angle = XM_2PI * static_cast<float>(index) /
                            static_cast<float>(requested);
        const float x = g_marineDropOrigin.x + std::sin(angle) * kRingRadius;
        const float z = g_marineDropOrigin.z + std::cos(angle) * kRingRadius;
        // Sampled per marine rather than reusing the drop-off height: the ring
        // can straddle a slope or a shoreline, where one shared Y buries half
        // the squad and leaves the rest hanging.
        const XMFLOAT3 stand{ x, GroundHeightAt(x, z), z };
        // Facing outward, away from the player and into whatever the landing
        // zone is surrounded by.
        if (SpawnMarine(stand, angle)) ++dropped;
    }
    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Deployment marines landed: " + std::to_string(dropped) + " of " +
            std::to_string(requested) + " requested");
}

// Test mode: relocates every live bandit to a random walkable point on the
// navmesh. Called once the squad exists and before the player is deployed, so
// the run opens against a layout the level did not author.
//
// Navmesh-sampled rather than terrain-sampled on purpose: a random terrain
// height is trivially easy to produce but routinely lands actors inside a
// house, on a cliff face, or out at sea, where they cannot path and the test
// tells you nothing. findRandomPoint only ever returns somewhere Detour agrees
// is walkable.
//
// Marines are left alone. They are authored to support the player's insertion
// and scattering them across the island defeats that; this mode is about
// varying where the *opposition* is.
static void ScatterEnemiesOnNavmesh() {
    if (!g_scatterEnemiesOnNavmesh) return;
    if (!g_navigation.Ready()) {
        SGE_LOG("LogGameplay", EngineLog::Level::Warning,
            "Enemy scatter skipped: navmesh not ready");
        return;
    }

    const unsigned int seed = g_scatterEnemiesSeed != 0
        ? g_scatterEnemiesSeed
        : static_cast<unsigned int>(std::time(nullptr));
    g_scatterEnemiesLastSeed = seed;

    // Own generator rather than std::rand, so a scatter is reproducible from
    // its seed regardless of whatever else has consumed the global sequence.
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const std::function<float()> random01 = [&]() { return unit(generator); };

    // Keep the scatter on dry land. The island terrain continues out under the
    // sea and the seabed is flat enough that Recast marks it walkable, so the
    // raw navmesh covers a large area of open water -- unfiltered, most of a
    // scatter lands offshore.
    //
    // Tested against the real terrain height rather than the navmesh Y: the
    // navmesh is a simplified surface and its Y can sit a little off the ground
    // the actor will actually stand on, which matters right at the waterline.
    // The ocean surface is y = 0 (see the g_ocean.Initialize call: centre
    // -kSeaDepth/2, height kSeaDepth), so anything at or below 0 is submerged.
    constexpr float kMinLandHeight = 0.55f;
    const auto onLand = [kMinLandHeight](const XMFLOAT3& candidate) {
        return GroundHeightAt(candidate.x, candidate.z) >= kMinLandHeight;
    };

    uint32_t moved = 0;
    uint32_t failed = 0;
    uint32_t submerged = 0;   // TEMP verify
    for (const auto& bandit : g_bandits) {
        // Turret gunners are pinned to their vehicle mount; moving them would
        // leave the actor and its turret in different places.
        if (!bandit || bandit->Dead() || bandit->turretGunner) continue;
        if (bandit->faction != Faction::Bandit) continue;

        XMFLOAT3 point{};
        if (!g_navigation.FindRandomPoint(random01, point, onLand)) {
            ++failed;
            continue;
        }
        // Detour returns the point on the navmesh surface; drop it onto the
        // terrain proper so the actor stands on the ground the renderer draws
        // rather than the simplified walkable poly.
        point.y = GroundHeightAt(point.x, point.z);
        bandit->position = point;
        // The actor captured its spawn on the first Update; clear that so its
        // patrol and leash anchor to where it now stands rather than the
        // authored spawn it no longer occupies.
        bandit->ResetSpawnAnchor();
        ++moved;
        if (point.y <= 0.0f) ++submerged;   // TEMP verify
    }

    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        "Enemy scatter: SUBMERGED=" + std::to_string(submerged) +
        " moved " + std::to_string(moved) +
        " bandits (seed " + std::to_string(seed) +
        (failed ? ", " + std::to_string(failed) + " navmesh samples failed" : "") +
        ")");
}

// One-button run randomizer for the deployment screen.
//
// Rolls the conditions that change how a run plays -- light, weather, fog and
// enemy layout -- from a single seed, so a roll is reproducible and can be
// shared or replayed by typing the seed back into the debug field. Returns the
// seed it used.
//
// Deliberately does not touch the loadout or insertion mode: those are the
// player's tactical answer to the conditions, and rolling them too would leave
// nothing to decide on this screen.
static unsigned int RandomizeDeployment(unsigned int seed = 0) {
    if (seed == 0) seed = static_cast<unsigned int>(std::time(nullptr));
    std::mt19937 generator(seed);

    // Time of day: all four presets are equally likely.
    std::uniform_int_distribution<int> pickTime(
        0, static_cast<int>(TimeOfDay::Night));
    g_selectedTimeOfDay = static_cast<TimeOfDay>(pickTime(generator));
    ApplyTimeOfDay(g_selectedTimeOfDay);

    // Weather: Clear..Storm only. Custom is the sentinel meaning "fog sliders
    // were hand-edited", not a preset, so rolling it would apply whatever fog
    // happened to be left over and read as the randomizer doing nothing.
    std::uniform_int_distribution<int> pickWeather(
        0, static_cast<int>(WeatherState::Storm));
    const auto weather = static_cast<WeatherState>(pickWeather(generator));
    ApplyLiveWeatherState(weather);

    // Fog density, jittered around whatever the rolled weather set. Scaling the
    // preset rather than picking an absolute keeps a Clear roll thin and a Fog
    // roll thick, so the weather choice still means something.
    {
        VolumetricFogSettings& fog = VolumetricFogFor(g_selectedTimeOfDay);
        if (fog.enabled) {
            std::uniform_real_distribution<float> jitter(0.65f, 1.55f);
            fog.density = std::clamp(fog.density * jitter(generator),
                                     0.0001f, 0.05f);
            ApplyVolumetricFogSettings(fog);
        }
        // Enemy sight keys off light and fog, so it has to be recomputed after
        // the density lands rather than left on ApplyLiveWeatherState's value.
        UpdateEnemyVisionForCurrentConditions();
    }

    // Enemy layout. Derived from the same generator so the whole roll travels
    // under one seed; a zero would mean "reseed from the clock" to the scatter
    // and break that, hence the 1-based range.
    std::uniform_int_distribution<unsigned int> pickScatter(
        1u, 0xfffffffeu);
    g_scatterEnemiesSeed = pickScatter(generator);
    // Force the scatter for this roll regardless of the persistent test toggle,
    // which is off by default so a fresh map opens on its authored layout.
    // Pressing randomize is an explicit request for a new layout, so it has to
    // move enemies even though entering the map does not.
    {
        const bool wasEnabled = g_scatterEnemiesOnNavmesh;
        g_scatterEnemiesOnNavmesh = true;
        ScatterEnemiesOnNavmesh();
        g_scatterEnemiesOnNavmesh = wasEnabled;
    }

    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        std::string("Deployment randomized (seed ") + std::to_string(seed) +
        "): " + TimeOfDayName(g_selectedTimeOfDay) + ", " +
        WeatherStateName(weather));
    return seed;
}

static bool SpawnHumveeTurretGunner(int vehicleIndex) {
    // No Humvee on the training range means no turret to man; without this the
    // gunner still spawns and stands in mid-air where the vehicle would be.
    // The base has no Humvee either, and no hostiles at all.
    if (g_trainingRangeMode || g_baseMode) return false;
    if (!g_banditModel.valid || !g_humveeModel) return false;
    const bool authoredVehicle = vehicleIndex >= 0 &&
        static_cast<size_t>(vehicleIndex) < LevelHumveeCount();
    const bool stressVehicle = vehicleIndex == kStressHumveeGunnerMount &&
        g_stressTestMode;
    if (!authoredVehicle && !stressVehicle) return false;
    for (const auto& existing : g_bandits)
        if (existing && !existing->Dead() && existing->turretGunner &&
            existing->mountedVehicleIndex == vehicleIndex)
            return true;
    auto bandit = std::make_unique<SkinnedEnemy>();
    if (!bandit->Init(g_banditModel)) return false;
    // Stagger the first grenade across the whole cooldown window so a squad
    // spawning together does not throw its opening volley in lockstep.
    bandit->grenadeCooldown = kBanditGrenadeCooldownMin +
        RandomUnit() * (kBanditGrenadeCooldownMax - kBanditGrenadeCooldownMin);
    bandit->position = authoredVehicle
        ? HumveeTurretMountWorld(static_cast<size_t>(vehicleIndex))
        : XMFLOAT3{ g_secondaryHumveePosition.x + g_humveeTurretLocal.x,
                    g_humveeTurretLocal.y + 3.45f,
                    g_secondaryHumveePosition.z + g_humveeTurretLocal.z };
    bandit->turretGunner = true;
    bandit->mountedVehicleIndex = vehicleIndex;
    bandit->spawnSlot = -1;
    bandit->leftArmReach = g_banditLeftArmReach;
    bandit->fireCooldown = 1.4f;
    bandit->PlayClip("Idle");
    g_bandits.push_back(std::move(bandit));
    return true;
}

static bool SpawnLevelHumveeTurretGunners() {
    bool allReady = true;
    for (size_t index = 0; index < LevelHumveeCount(); ++index)
        allReady = SpawnHumveeTurretGunner(static_cast<int>(index)) && allReady;
    if (g_stressTestMode)
        allReady = SpawnHumveeTurretGunner(kStressHumveeGunnerMount) && allReady;
    return allReady;
}

// The patrol boat uses a reserved negative mount ID, leaving every non-negative
// value available for an authored Humvee index.
static bool SpawnBoatTurretGunner() {
    // No patrol boat on the training range, so no gunner to ride it -- the boat
    // model is hidden there, and without this he is left firing from open water.
    // Same for the base, whose level file switches the patrol boat off.
    if (g_trainingRangeMode || g_baseMode) return false;
    if (!g_banditModel.valid || !g_boatModel) return false;
    for (const auto& existing : g_bandits)
        if (existing && !existing->Dead() && existing->turretGunner &&
            existing->mountedVehicleIndex == kBoatGunnerMount)
            return true;
    auto bandit = std::make_unique<SkinnedEnemy>();
    if (!bandit->Init(g_banditModel)) return false;
    bandit->grenadeCooldown = kBanditGrenadeCooldownMin +
        RandomUnit() * (kBanditGrenadeCooldownMax - kBanditGrenadeCooldownMin);
    bandit->position = BoatTurretMountWorld();
    bandit->turretGunner = true;
    bandit->mountedVehicleIndex = kBoatGunnerMount;
    bandit->spawnSlot = -1;
    bandit->leftArmReach = g_banditLeftArmReach;
    bandit->fireCooldown = 1.4f;
    bandit->PlayClip("Idle");
    g_bandits.push_back(std::move(bandit));
    return true;
}
