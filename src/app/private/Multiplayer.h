#pragma once

// Private application implementation; included once by main.cpp in dependency
// order.
//
// The translation layer between NetSession and the game's globals. NetSession
// deliberately knows nothing about the camera, SkinnedEnemy or the AppState
// globals, so that conversion has to happen somewhere -- it happens here, in
// one file, rather than being spread through main.cpp.

#include <sstream>

// g_netSession lives in TerrainAndDamage.h, ahead of Menus.h, so the menu can
// start and stop a session.
//
// The local player's intent for this frame, published by ProcessInput. Held
// here rather than passed down through the frame so the multiplayer update can
// be a single call at one clear point in the loop.
static PlayerInput g_localPlayerInput;
// Reused across frames so the per-frame snapshot read does not allocate.
static std::vector<net::RemotePlayer> g_netRemoteScratch;
static std::vector<net::PlayerStateChange> g_netStateChanges;
static std::vector<net::WorldImpactRequest> g_netWorldImpactScratch;
static std::vector<net::WorldBreakEvent> g_netWorldBreakScratch;

static bool MultiplayerActive() { return g_netSession.Active(); }

// On-screen notices for players arriving and leaving. Driven off the session's
// own slots rather than off spawned bodies: a player who joins while this
// machine is still loading, or on a level whose marine model never came up, has
// no body to notice, and the arrival is exactly when it is worth saying so.
struct NetPlayerNotice {
    std::string text;
    float remaining = 0.0f;
    bool joined = true;
};
static std::vector<NetPlayerNotice> g_netPlayerNotices;
static bool g_netPlayerSeen[net::kMaxPlayers] = {};
static constexpr float kNetNoticeSeconds = 4.5f;

static void ResetNetPlayerNotices() {
    g_netPlayerNotices.clear();
    for (bool& seen : g_netPlayerSeen) seen = false;
}

// Diffs the session roster against the last frame and queues one notice per
// change. The local player is skipped -- "you connected" tells nobody anything,
// and the menu already names which player this machine is.
static void UpdateNetPlayerNotices(float deltaTime) {
    for (NetPlayerNotice& notice : g_netPlayerNotices)
        notice.remaining -= deltaTime;
    g_netPlayerNotices.erase(
        std::remove_if(g_netPlayerNotices.begin(), g_netPlayerNotices.end(),
                       [](const NetPlayerNotice& notice) {
                           return notice.remaining <= 0.0f;
                       }),
        g_netPlayerNotices.end());

    if (!MultiplayerActive()) {
        ResetNetPlayerNotices();
        return;
    }
    for (net::PlayerId id = 0; id < net::kMaxPlayers; ++id) {
        const bool active = g_netSession.PlayerActive(id);
        if (active == g_netPlayerSeen[id]) continue;
        g_netPlayerSeen[id] = active;
        if (id == g_netSession.LocalId()) continue;
        char text[64];
        std::snprintf(text, sizeof(text), "PLAYER-%d %s",
                      static_cast<int>(id) + 1,
                      active ? "CONNECTED" : "LEFT");
        g_netPlayerNotices.push_back({ text, kNetNoticeSeconds, active });
    }
}

// Drawn under the top edge, stacked downwards, newest last. Uses the foreground
// list so it survives whatever the HUD is doing beneath it.
static void DrawNetPlayerNotices() {
    if (g_netPlayerNotices.empty()) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    float y = display.y * 0.11f;
    for (const NetPlayerNotice& notice : g_netPlayerNotices) {
        // Fade the last second so a notice leaves rather than vanishing.
        const float alpha = (std::min)(1.0f, notice.remaining);
        const ImVec2 size = ImGui::CalcTextSize(notice.text.c_str());
        const float x = (display.x - size.x) * 0.5f;
        draw->AddRectFilled(
            ImVec2(x - 12.0f, y - 5.0f),
            ImVec2(x + size.x + 12.0f, y + size.y + 5.0f),
            IM_COL32(10, 14, 12, static_cast<int>(180 * alpha)), 4.0f);
        const ImU32 colour = notice.joined
            ? IM_COL32(150, 235, 170, static_cast<int>(250 * alpha))
            : IM_COL32(235, 175, 130, static_cast<int>(250 * alpha));
        draw->AddText(ImVec2(x, y), colour, notice.text.c_str());
        y += size.y + 14.0f;
    }
}

static DeferredReleaseQueue<std::unique_ptr<SkinnedEnemy>> g_retiredNetworkActors;

template <typename Predicate>
static void RetireNetworkActors(Predicate remove) {
    for (auto& actor : g_bandits) {
        if (!actor || !remove(*actor)) continue;
        if (g_heldBandit == actor.get()) g_heldBandit = nullptr;
        g_retiredNetworkActors.RetireAfterSubmission(std::move(actor));
    }
    g_bandits.erase(std::remove(g_bandits.begin(), g_bandits.end(), nullptr),
                   g_bandits.end());
}

// At the next frame boundary, update and ImGui removals both have a submitted
// direct fence covering their last draw. Never wait here.
static void CollectRetiredNetworkActors() {
    if (!g_dx12.fence) return;
    g_retiredNetworkActors.SealSubmission(g_dx12.lastDirectFenceValue);
    g_retiredNetworkActors.Collect(g_dx12.fence->GetCompletedValue());
}

// Levels used to be each player's own choice, which reads as a working session
// right up until two people shoot at terrain only one of them has. The host's
// map is now the session's map: it publishes what it is on, and a client loads
// to match -- on join, and again whenever the host moves.
static void PublishHostLevel() {
    if (!MultiplayerActive() ||
        g_netSession.CurrentRole() != net::Role::Host) return;
    // Called every frame rather than hooked into each level-start site: there
    // are six of those and they all end up writing g_activeLevelKind, so
    // watching the result is the version that cannot be forgotten. SetHostLevel
    // sends only on a change.
    g_netSession.SetHostLevel(g_activeLevelKind, g_activeLevelFile);
}

// Resolves a level file the way the rest of the game does. The three roots are
// not interchangeable guesses: the repo, a build/ run and a packaged install
// each keep Content/Levels somewhere different.
static std::filesystem::path FindNetworkLevelFile(const std::string& file) {
    const std::filesystem::path candidates[] = {
        std::filesystem::path("Content/Levels") / file,
        std::filesystem::path("levels") / file,
        std::filesystem::path("build/Content/Levels") / file,
    };
    std::error_code error;
    for (const std::filesystem::path& candidate : candidates)
        if (std::filesystem::exists(candidate, error)) return candidate;
    return {};
}

static void FollowHostLevel(HWND hwnd) {
    if (!MultiplayerActive() ||
        g_netSession.CurrentRole() != net::Role::Client) return;
    net::LevelKind kind = net::LevelKind::None;
    std::string file;
    if (!g_netSession.TakePendingLevel(kind, file)) return;
    if (kind == g_activeLevelKind && file == g_activeLevelFile) return;
    // A host sitting in its own menus is not a reason to tear this player's
    // level down. It will name one shortly, and being dropped to the menu in
    // between is worse than arriving a moment late.
    if (kind == net::LevelKind::None) return;
    // A load in progress owns the screen. Put the request back rather than
    // starting a second one on top of it.
    if (g_game.loading.Active()) {
        g_netSession.RequeuePendingLevel();
        return;
    }
    switch (kind) {
    case net::LevelKind::Level1:
        StartLevelOne(hwnd, true);
        break;
    case net::LevelKind::TestLevel:
        StartLevelOne(hwnd, true, false, true, nullptr, true);
        break;
    case net::LevelKind::LevelFile: {
        const std::filesystem::path path = FindNetworkLevelFile(file);
        if (path.empty()) {
            // Naming the file is the whole diagnostic: the host has a level
            // this machine does not, and no amount of retrying fixes that.
            SGE_LOG("LogNet", EngineLog::Level::Warning,
                "host is on '" + file + "', which is not in Content/Levels here");
            g_mainMenuLevelStatus = "Host is playing " + file +
                                    ", which is missing from Content/Levels.";
            return;
        }
        StartCustomLevel(hwnd, path);
        break;
    }
    default:
        return;
    }
    SGE_LOG("LogNet", EngineLog::Level::Display,
        "following host onto level " + std::to_string(int(kind)) +
        (file.empty() ? std::string() : " (" + file + ")"));
}

static void DamageBulletPrefabEntity(uint64_t entityId, float damage,
                                      const XMFLOAT3& hit, bool remoteCharge,
                                      bool playerOwned) {
    // Charges used to be excluded here and applied locally instead. That was
    // not a policy, it was a missing field: the wire had nowhere to say "this
    // was a charge", so a replicated demolition arrived looking like rifle fire
    // and CommTowerDamageAllowed threw it away. The tower then came down for
    // whoever planted the C4 and stayed standing for everyone else. The flag
    // travels now, so this takes the same route every other impact takes.
    if (MultiplayerActive())
        g_netSession.ReportWorldImpact(entityId, damage, hit.x, hit.y, hit.z,
                                       /*kind=*/1, 0.0f, 0.0f, 0, 0.0f, 0.0f,
                                       0.0f, playerOwned, remoteCharge);
    else
        DamagePrefabEntity(entityId, damage, hit, remoteCharge, playerOwned);
}

// A round from this machine struck an enemy gunship. In a session the host owns
// the airframe's health, so the hit is reported and the outcome comes back in
// the vehicle state; offline it is applied where it happened.
static void DamageNetworkedHelicopter(uint8_t airframe, float damage,
                                      const XMFLOAT3& hit) {
    if (MultiplayerActive()) {
        g_netSession.ReportWorldImpact(airframe, damage, hit.x, hit.y, hit.z,
                                       /*kind=*/3);
        return;
    }
    if (airframe == 0) DamageHelicopter(damage, hit);
    else DamageSecondaryHelicopter(damage, hit);
}

// Blast damage aimed at one objective by entity id -- the rigged comm tower and
// the objective aircraft, both of which are found by volume rather than by the
// radius sweep and so are damaged through a direct call. Same rule as the
// bullet path: in a session the host owns the outcome, so report it and let the
// committed edge come back, rather than collapsing the mast locally.
static void DamageObjectivePrefabEntity(uint64_t entityId, float damage,
                                        const XMFLOAT3& center,
                                        bool remoteCharge, bool playerOwned) {
    if (MultiplayerActive())
        g_netSession.ReportWorldImpact(entityId, damage, center.x, center.y,
                                       center.z, /*kind=*/1, 0.0f, 0.0f, 0,
                                       0.0f, 0.0f, 0.0f, playerOwned,
                                       remoteCharge);
    else
        DamagePrefabEntity(entityId, damage, center, remoteCharge, playerOwned);
}

// Applies a single authoritative world impact. The host and every client use
// the same damage and impulse inputs; the session only transports the event and
// never touches game state.
//
// `spawnImpactFx` is what makes another player's fire visible at all. The
// shooter drew its sparks, decal and dust the instant it fired; every other
// machine has drawn nothing, and without this a remote player's rounds would
// silently eat a wall with no sign of where they landed.
static void ApplyNetworkWorldImpact(uint8_t kind, uint64_t entityId,
                                    float damage, float radius, float impulse,
                                    float dirX, float dirY, float dirZ,
                                    const XMFLOAT3& hit,
                                    bool spawnImpactFx, bool playerOwned,
                                    bool remoteCharge) {
    const XMFLOAT3 normal{ -dirX, -dirY, -dirZ };
    if (kind == 1) {
        // spawnImpactFx doubles as "this was not my round": the shot's own
        // machine already marked and sparked at the trigger pull.
        DamagePrefabEntity(entityId, damage, hit, remoteCharge, playerOwned,
                           /*localShot=*/!spawnImpactFx);
        if (spawnImpactFx) {
            scene.SpawnBulletImpact(hit, normal);
            scene.SpawnSmokeBurst(hit, 0.3f, 0.1f);
        }
        return;
    }
    XMFLOAT3 direction{ dirX, dirY, dirZ };
    if (XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&direction))) < 1e-6f)
        direction = { 0.0f, 1.0f, 0.0f };
    if (kind == 0) {
        if (!scene.useDestruction || !g_destruction.IsInitialized()) return;
        const float hitRadius = radius > 0.0f
            ? radius : scene.destructionDamageRadius;
        // Sampled before the damage, which can tear the sheet loose and leave
        // the query looking at whatever is behind it -- the same order the
        // local hit path uses.
        const bool metalSheetHit =
            spawnImpactFx && g_destruction.IsMetalSheetAt(hit);
        g_destruction.ApplyRadialDamage(hit, hitRadius, damage);
        if (impulse > 0.0f)
            g_destruction.ApplyImpulse(hit, direction, impulse, hitRadius);
        if (spawnImpactFx) {
            if (metalSheetHit) PlayMetalHitAudio(hit, 0.9f);
            scene.SpawnBulletImpact(hit, normal);
            scene.SpawnSmokeBurst(hit, 0.3f, 0.1f);
        }
        return;
    }
    if (kind == 3) {
        // Only the host acts on this. A client is told the result through the
        // vehicle state instead: applying the damage here as well would take
        // the same health off twice on the machine that fired.
        if (g_netSession.CurrentRole() == net::Role::Host) {
            if (entityId == 0) DamageHelicopter(damage, hit);
            else DamageSecondaryHelicopter(damage, hit);
        }
        if (spawnImpactFx) {
            PlayMetalHitAudio(hit, 0.85f);
            scene.SpawnBulletImpact(hit, normal);
        }
        return;
    }
    if (kind == 2 && !g_emptyLevelMode && g_trees.IsInitialized()) {
        // PalmTrees::Shoot owns the segment selection and damage bookkeeping.
        // Reconstruct a short segment around the authoritative hit so the
        // host/client choose the same nearby trunk without adding a second tree
        // replication format.
        const XMVECTOR point = XMLoadFloat3(&hit);
        const XMVECTOR travel = XMLoadFloat3(&direction) * 0.5f;
        XMFLOAT3 start, end, ignored;
        XMStoreFloat3(&start, point - travel);
        XMStoreFloat3(&end, point + travel);
        if (g_trees.Shoot(start, end, direction,
                          radius > 0.0f ? radius : 0.08f, damage, ignored) &&
            spawnImpactFx) {
            scene.SpawnBulletImpact(hit, normal);
            scene.SpawnSmokeBurst(hit, 0.25f, 0.1f);
        }
    }
}

// Called from the gameplay update after NetSession::Update and level state are
// ready. Host requests are one frame delayed by design; clients apply only the
// committed server edge, so an originating bullet never mutates shared world
// state twice and no event can bounce back into the network.
static void UpdateNetworkWorldImpacts() {
    if (!MultiplayerActive()) return;
    if (g_netSession.CurrentRole() == net::Role::Host) {
        g_netSession.DrainWorldImpacts(g_netWorldImpactScratch);
        for (const net::WorldImpactRequest& impact : g_netWorldImpactScratch) {
            const XMFLOAT3 hit{ impact.hitX, impact.hitY, impact.hitZ };
            ApplyNetworkWorldImpact(impact.kind, impact.entityId,
                                    impact.damage, impact.radius,
                                    impact.impulse, impact.dirX,
                                    impact.dirY, impact.dirZ, hit,
                                    impact.shooter != g_netSession.LocalId(),
                                    impact.playerOwned, impact.remoteCharge);
            g_netSession.PublishWorldBreak(
                impact.kind, impact.entityId,
                impact.damage, impact.radius, impact.impulse,
                impact.hitX, impact.hitY, impact.hitZ,
                impact.dirX, impact.dirY, impact.dirZ, impact.shooter,
                impact.playerOwned, impact.remoteCharge);
        }
    } else {
        g_netSession.DrainWorldBreaks(g_netWorldBreakScratch);
        for (const net::WorldBreakEvent& impact : g_netWorldBreakScratch) {
            const XMFLOAT3 hit{ impact.hitX, impact.hitY, impact.hitZ };
            ApplyNetworkWorldImpact(impact.kind, impact.entityId,
                                    impact.damage, impact.radius,
                                    impact.impulse, impact.dirX,
                                    impact.dirY, impact.dirZ, hit,
                                    impact.shooter != g_netSession.LocalId(),
                                    impact.playerOwned, impact.remoteCharge);
        }
    }
}

static std::vector<net::RemoteShot> g_netRemoteShots;
static std::vector<net::RemoteChargeStuck> g_netChargeSticks;
static std::vector<net::RemoteChargeDetonate> g_netChargeDetonations;
// Last shot count this machine has already put on the wire. Compared rather
// than hooked, so Scene stays unaware that a session exists.
static uint32_t g_netLastReportedShot = 0;

// Announce whatever the local player has fired since the previous frame. The
// counter can move by more than one on a frame that dropped below the fire
// rate, and only the latest origin is kept, so a burst is reported as a single
// shot from where the last round left rather than as a stack of stale muzzles.
static void ReportLocalShots() {
    if (!MultiplayerActive()) {
        g_netLastReportedShot = scene.localShotCounter;
        return;
    }
    if (scene.localShotCounter == g_netLastReportedShot) return;
    g_netLastReportedShot = scene.localShotCounter;
    g_netSession.ReportShotFired(
        scene.localShotOrigin.x, scene.localShotOrigin.y,
        scene.localShotOrigin.z, scene.localShotDirection.x,
        scene.localShotDirection.y, scene.localShotDirection.z);
}

// How far a tracer's line travels before the world stops it, measured along
// the shot's own ray out to the furthest the streak could reach anyway.
//
// The streak is presentation, but it is presentation the player reads as
// information -- it says "fire is coming from over there" -- so drawing it
// through a hillside or a hangar wall points them at cover the round never
// crossed. The blockers are the ones BanditHasLineOfSight already uses, minus
// the trees: terrain, solid props and destructibles. Chain-link stays
// transparent for the same reason it does there, since a round crosses it and
// a streak dying on the panel would read as cover that is not there.
//
// Palms are left out because g_trees.BlocksSegment answers yes/no and hands
// back no hit point, so stopping on one would mean bisecting the segment for a
// distance it does not report -- a second, disagreeing notion of where a trunk
// was struck. The visible cost is a streak grazing a palm; a trunk is narrow
// and the streak is moving, which is nothing like a ridge line.
//
// Actors are deliberately not tested. A round that hits a body was settled on
// the shooter's machine and its streak should still reach the victim, and a
// teammate crossing the line mid-flight must not chop the streak short.
static float ResolveRemoteTracerRange(const XMFLOAT3& origin,
                                      const XMFLOAT3& direction) {
    XMVECTOR forward = XMLoadFloat3(&direction);
    if (XMVectorGetX(XMVector3LengthSq(forward)) < 1e-6f) return FLT_MAX;
    forward = XMVector3Normalize(forward);
    // The whole distance the streak could cover in its lifetime. Casting
    // further would pay for geometry the tracer expires before reaching.
    const float reach = RemoteTracerFX::kSpeed * RemoteTracerFX::kMaxLife;
    const XMVECTOR originV = XMLoadFloat3(&origin);
    XMFLOAT3 end;
    XMStoreFloat3(&end, originV + forward * reach);

    // Matches the ray radius the bandit sight check uses, so a streak and the
    // shot it stands for agree about what counts as clipping a surface.
    constexpr float kRayRadius = 0.04f;
    float best = FLT_MAX;
    auto consider = [&](const XMFLOAT3& hit) {
        const XMVECTOR delta = XMLoadFloat3(&hit) - originV;
        // Projected onto the ray rather than taken as a straight length: the
        // streak advances along `direction`, so that is the axis the stop
        // distance has to be measured on.
        const float along = XMVectorGetX(XMVector3Dot(delta, forward));
        if (along > 0.0f && along < best) best = along;
    };

    XMFLOAT3 hit;
    if (HitPrefabColliderSegment(origin, end, kRayRadius, hit, nullptr, nullptr,
                                 /*fencePanelsTransparent=*/true))
        consider(hit);
    if (HitTerrainSegment(origin, end, kRayRadius, hit)) consider(hit);
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegmentForVision(origin, end, kRayRadius, hit))
        consider(hit);
    return best;
}

// Present the rounds other players fired. Purely presentation: the muzzle
// flash, the smoke and the report, at the place the shot actually came from.
// Nothing here damages anything -- what a round hits is settled by the
// hit-report path, and spawning a live projectile would apply it twice.
//
// The flash is a small world explosion rather than TriggerMuzzleFlash, which
// drives the local viewmodel and has no world position: the same reason the AA
// turret presents its own fire this way.
static void PresentRemoteShots(float deltaTime) {
    // Ahead of the early-out and ahead of the drain: tracers already in flight
    // have to keep flying while this machine sits on a menu or loads a level,
    // and one spawned below should start its life at the muzzle rather than a
    // frame downrange.
    scene.UpdateRemoteTracers(deltaTime);
    if (!MultiplayerActive()) return;
    g_netSession.DrainRemoteShots(g_netRemoteShots);
    for (const net::RemoteShot& shot : g_netRemoteShots) {
        const XMFLOAT3 muzzle{ shot.x, shot.y, shot.z };
        const XMFLOAT3 direction{ shot.dirX, shot.dirY, shot.dirZ };
        // The tracer is what says "someone is shooting, and that way". A rifle
        // firing is not an explosion: SpawnExplosionFX was standing in for a
        // muzzle flash here and brought its whole payload with it -- 54 sparks
        // and dust per round, and an ApplyExplosionImpulse that shook the
        // camera of anyone watching. One puff of smoke is the whole effect,
        // matching how the helicopter door gunner presents its own fire.
        // Same world stop as enemy fire. A remote player's shot is reported
        // from their machine and drawn here with no projectile behind it, so
        // without this an ally firing from inside a building put a streak out
        // through its wall.
        scene.SpawnRemoteTracer(muzzle, direction, /*hostile=*/false,
                                ResolveRemoteTracerRange(muzzle, direction));
        scene.SpawnWeaponSmoke(muzzle, direction, 0.55f);
        g_gunAudio.PlayAt(muzzle.x, muzzle.y, muzzle.z, 0.8f,
                          0.98f + ((float)std::rand() / RAND_MAX) * 0.05f);
    }
}

// Demolition charges, both halves. A charge used to be planted and fired
// entirely locally: nobody else could see one stuck to a wall, and pressing the
// detonator brought the target down on the presser's screen alone.
//
// The blast itself stays host-authoritative, like every other explosion here.
// Only the host queues the blast projectile -- that is what damages the world
// and reports the damage -- and the result reaches everyone through the
// world-break path already carrying craters and broken props. Clients drop the
// charges and show the burst so the explosion is not silent and invisible where
// it happens.
static void UpdateNetworkCharges() {
    if (!MultiplayerActive()) return;
    g_netSession.DrainChargeSticks(g_netChargeSticks);
    for (const net::RemoteChargeStuck& stuck : g_netChargeSticks) {
        // The planter placed its own the moment the charge landed; this is
        // everyone else catching up.
        if (stuck.owner == g_netSession.LocalId()) continue;
        scene.StickRemoteCharge({ stuck.x, stuck.y, stuck.z },
                                { stuck.nx, stuck.ny, stuck.nz },
                                static_cast<uint8_t>(stuck.owner));
    }

    const bool authoritative =
        g_netSession.CurrentRole() == net::Role::Host;
    g_netSession.DrainChargeDetonations(g_netChargeDetonations);
    for (const net::RemoteChargeDetonate& fired : g_netChargeDetonations) {
        // Rigged towers are recorded before the charges are cleared, for the
        // same reason the local detonator does it: the blast is not resolved
        // until the projectile pass later this frame, by which point the charge
        // that authorised the demolition is gone.
        //
        // Every machine marks, not just the authoritative one. The mark is a
        // permission, not damage: CommTowerDamageAllowed refuses all tower
        // damage unless the tower is in this machine's own rigged set, and the
        // committed demolition arrives later as an ordinary world break that
        // has to pass that same gate here. Marking only on the host left a
        // client rejecting the host's own answer -- charges went off at the
        // foot of the tower and it stayed standing, on the screen of the player
        // who had just blown it.
        MarkCommTowersRiggedForDemolition();
        scene.DetonateRemoteChargesFor(static_cast<uint8_t>(fired.owner),
                                       authoritative);
    }
}

// The two enemy gunships are the host's aircraft. Each machine used to fly its
// own copy from its own AI, which held together only while nothing touched
// them: the moment a client shot one down it crashed on that client alone and
// went on flying, firing and dropping troops for everyone else.
//
// Sent as plain state rather than as damage events. The craft is in continuous
// motion and its health only ever falls, so the last word from the host is
// always the right answer and a dropped unreliable packet costs one tick of
// staleness instead of a permanent disagreement.
static void PublishHostVehicles() {
    if (!MultiplayerActive() ||
        g_netSession.CurrentRole() != net::Role::Host) return;
    const VehicleSystem& vehicles = g_game.vehicles;
    net::EnemyHelicopterState state[net::kEnemyHelicopterCount];

    net::EnemyHelicopterState& primary = state[0];
    primary.present = g_helicopterModel != nullptr && scene.showHelicopter &&
                      !g_emptyLevelMode;
    primary.dead = vehicles.helicopterDead;
    primary.crashed = vehicles.helicopterCrashed;
    primary.x = vehicles.helicopterPosition.x;
    primary.y = vehicles.helicopterPosition.y;
    primary.z = vehicles.helicopterPosition.z;
    primary.yaw = vehicles.helicopterYaw;
    primary.health = vehicles.helicopterHealth;

    net::EnemyHelicopterState& secondary = state[1];
    secondary.present = SecondaryHelicopterPresent() && !g_emptyLevelMode;
    secondary.dead = vehicles.secondaryHelicopterDead;
    secondary.crashed = vehicles.secondaryHelicopterCrashed;
    secondary.x = vehicles.secondaryHelicopterPosition.x;
    secondary.y = vehicles.secondaryHelicopterPosition.y;
    secondary.z = vehicles.secondaryHelicopterPosition.z;
    secondary.yaw = vehicles.secondaryHelicopterYaw;
    secondary.pitch = vehicles.secondaryHelicopterPitch;
    secondary.roll = vehicles.secondaryHelicopterRoll;
    secondary.health = vehicles.secondaryHelicopterHealth;

    net::EscapeBoatSnapshot boat;
    boat.active = vehicles.escapeBoatActive ? 1 : 0;
    boat.x = vehicles.escapeBoatPosition.x;
    boat.y = vehicles.escapeBoatPosition.y;
    boat.z = vehicles.escapeBoatPosition.z;
    boat.yaw = vehicles.escapeBoatYaw;
    boat.bobTime = vehicles.escapeBoatBobTime;
    g_netSession.PublishVehicles(state, boat);
}

static void ApplyNetworkEscapeBoat() {
    if (g_netSession.CurrentRole() != net::Role::Client) return;
    VehicleSystem& vehicles = g_game.vehicles;
    const auto* boat = g_netSession.RemoteEscapeBoat();
    // Clients wait for the host's choice, including when joining mid-mission.
    vehicles.escapeBoatActive = boat && boat->active != 0;
    if (!boat) return;
    vehicles.escapeBoatPosition = { boat->x, boat->y, boat->z };
    vehicles.escapeBoatYaw = boat->yaw;
    vehicles.escapeBoatBobTime = boat->bobTime;
}

// Client-side. Runs after the local vehicle update rather than instead of it:
// the rotor spin, the damage smoke and the audio are all presentation driven
// off these same fields, so letting them run and then overwriting what the host
// owns keeps the craft animated without letting the client decide where it is,
// how hurt it is, or whether it is still flying.
static void ApplyNetworkEnemyHelicopters() {
    if (!MultiplayerActive() ||
        g_netSession.CurrentRole() != net::Role::Client) return;
    const net::EnemyHelicopterState* state = g_netSession.RemoteVehicles();
    if (!state) return;   // nothing received yet; leave the local craft alone
    VehicleSystem& vehicles = g_game.vehicles;

    const net::EnemyHelicopterState& primary = state[0];
    vehicles.helicopterPosition = { primary.x, primary.y, primary.z };
    vehicles.helicopterYaw = primary.yaw;
    vehicles.helicopterHealth = primary.health;
    vehicles.helicopterDead = primary.dead;
    vehicles.helicopterCrashed = primary.crashed;

    const net::EnemyHelicopterState& secondary = state[1];
    vehicles.secondaryHelicopterPosition = {
        secondary.x, secondary.y, secondary.z };
    vehicles.secondaryHelicopterYaw = secondary.yaw;
    vehicles.secondaryHelicopterPitch = secondary.pitch;
    vehicles.secondaryHelicopterRoll = secondary.roll;
    vehicles.secondaryHelicopterHealth = secondary.health;
    vehicles.secondaryHelicopterDead = secondary.dead;
    vehicles.secondaryHelicopterCrashed = secondary.crashed;
}

// Remote bodies live in g_bandits alongside the AI actors, so they are drawn,
// lit and shadowed by the paths that already exist. They are found by scanning
// for the player id rather than through a side map of pointers: g_bandits is
// cleared wholesale on a level reset (ResetLevelRuntime), which would leave any
// such map holding dangling pointers to freed actors. The list is a handful of
// entries and this runs once per remote player per frame.
static SkinnedEnemy* FindNetworkPlayerBody(net::PlayerId id) {
    for (const auto& actor : g_bandits) {
        if (!actor || !actor->networkControlled) continue;
        if (actor->netPlayerId == id) return actor.get();
    }
    return nullptr;
}

// Spawns the body for a player that just appeared in a snapshot. Mirrors
// SpawnMarine: same model, same faction, so the remote player is rendered by
// the ally path that already exists rather than by new rendering code.
static SkinnedEnemy* SpawnNetworkPlayerBody(net::PlayerId id) {
    if (!g_marineModel.valid) return nullptr;
    auto body = std::make_unique<SkinnedEnemy>();
    if (!body->Init(g_marineModel)) return nullptr;
    body->faction = Faction::Marine;
    body->networkControlled = true;
    char callsign[32];
    std::snprintf(callsign, sizeof(callsign), "Player-%d",
                  static_cast<int>(id) + 1);
    body->callsign = callsign;
    body->leftArmReach = g_banditLeftArmReach;
    body->health = 100.0f;
    // Still zero now that PvP exists, for a narrower reason than before:
    // player-vs-player damage no longer goes anywhere near this -- it runs the
    // geometry test only and lets the host apply the arithmetic. What this
    // still blocks is every OTHER local damage source (explosions, fire,
    // debris, vehicles), each of which would otherwise mutate a remote body's
    // health on one machine alone. Replicating those is a later milestone; until
    // then they must do nothing rather than desync.
    body->damageTakenScale = 0.0f;
    body->netPlayerId = id;
    SkinnedEnemy* raw = body.get();
    g_bandits.push_back(std::move(body));
    return raw;
}

// The local hit test said a round connected with another player's body. Tell
// the host, which owns the arithmetic and hands the result back in the next
// snapshot. Nothing is applied locally -- see the comment at the call site in
// main.cpp for why the mutation deliberately does not happen there.
static void ReportNetworkPlayerHit(net::PlayerId target, bool headshot,
                                   const XMFLOAT3& impact, float bodyDamage) {
    if (!MultiplayerActive() || target == net::kInvalidPlayerId) return;
    g_netSession.ReportHit(target, bodyDamage, headshot,
                           impact.x, impact.y, impact.z);
}

// The downed teammate the local player is standing close enough to pick up, or
// null. Nearest wins, the same rule NearbyWeaponPickup uses and for the same
// reason: the prompt and the key handler read this one function, so they can
// never disagree about who is being revived.
static SkinnedEnemy* NearbyDownedPlayer(net::PlayerId* outId = nullptr) {
    if (!MultiplayerActive() || g_drivingHumvee) return nullptr;
    // You cannot pick anyone up while you are on the floor yourself.
    if (scene.player.downed) return nullptr;
    if (g_game.session.Screen() != GameScreen::Level1 && !IsEditorPlaying())
        return nullptr;

    const XMFLOAT3& camera = scene.camera.Position;
    SkinnedEnemy* best = nullptr;
    float bestDistanceSquared = net::kReviveRadius * net::kReviveRadius;
    for (const auto& actor : g_bandits) {
        if (!actor || !actor->networkControlled || !actor->netDowned) continue;
        const float dx = camera.x - actor->position.x;
        const float dz = camera.z - actor->position.z;
        // Compared in the plane: the camera is at eye height and the body is on
        // the ground, so including Y would push every revive out of range.
        const float distanceSquared = dx * dx + dz * dz;
        if (distanceSquared >= bestDistanceSquared) continue;
        bestDistanceSquared = distanceSquared;
        best = actor.get();
    }
    if (best && outId) *outId = best->netPlayerId;
    return best;
}

// "[E] REVIVE PLAYER-N" over a downed teammate, with a progress bar once the
// hold starts. Same projection and drawing shape as DrawWeaponPickupPrompt.
static void DrawRevivePrompt(DirectX::CXMMATRIX view,
                             DirectX::CXMMATRIX projection) {
    const SkinnedEnemy* body = NearbyDownedPlayer();
    if (!body) return;

    // Low: the body is lying down, so an anchor at standing height would float
    // the prompt above a player who is not there.
    const XMFLOAT3 anchor{ body->position.x, body->position.y + 0.6f,
                           body->position.z };
    const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&anchor),
                                             view * projection);
    const float w = XMVectorGetW(clip);
    if (w <= 0.01f) return;   // behind the camera

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 screen{
        (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
        (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };

    char label[64];
    std::snprintf(label, sizeof(label), "[E] REVIVE PLAYER-%d",
                  static_cast<int>(body->netPlayerId) + 1);
    const ImVec2 size = ImGui::CalcTextSize(label);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(
        ImVec2(screen.x - size.x * 0.5f - 6.0f, screen.y - 4.0f),
        ImVec2(screen.x + size.x * 0.5f + 6.0f, screen.y + 4.0f + size.y),
        IM_COL32(14, 12, 6, 185), 3.0f);
    draw->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y),
                  IM_COL32(255, 255, 255, 245), label);

    // Progress comes off the session rather than being timed locally, so what
    // is drawn is what the host has actually credited.
    net::LocalPlayerStatus status;
    if (!g_netSession.PlayerStatus(body->netPlayerId, status)) return;
    if (status.reviveProgress <= 0.0f) return;
    constexpr float kBarWidth = 120.0f;
    const float barX = screen.x - kBarWidth * 0.5f;
    const float barY = screen.y + size.y + 8.0f;
    draw->AddRectFilled(ImVec2(barX, barY),
                        ImVec2(barX + kBarWidth, barY + 5.0f),
                        IM_COL32(18, 22, 20, 200), 2.0f);
    draw->AddRectFilled(
        ImVec2(barX, barY),
        ImVec2(barX + kBarWidth * status.reviveProgress, barY + 5.0f),
        IM_COL32(120, 230, 150, 240), 2.0f);
}

// Next id to hand out. Monotonic and never reused within a session: an id that
// came back after its body was gone would let a late hit report land on a
// different enemy entirely.
static uint16_t g_nextNetEnemyId = 0;
// Reused across frames so the per-frame publish does not allocate.
static std::vector<net::HostEnemyState> g_hostEnemyScratch;
static std::vector<net::EnemyHitRequest> g_enemyHitScratch;

// Host-side: give every AI actor an id and tell the session where they are.
//
// Ids are assigned here rather than at each of the seven spawn sites, so a new
// kind of spawn cannot be added without one -- an actor with no id would simply
// never replicate, and it would take a two-machine test to notice.
static void PublishHostEnemies() {
    if (g_netSession.CurrentRole() != net::Role::Host) return;
    g_hostEnemyScratch.clear();
    g_hostEnemyScratch.reserve(g_bandits.size());
    for (const auto& actor : g_bandits) {
        // Player bodies replicate through the player snapshot; replicating them
        // twice would have each client fighting its own teammate's shadow.
        if (!actor || actor->networkControlled) continue;
        if (actor->netEnemyId == net::kInvalidEnemyId)
            actor->netEnemyId = g_nextNetEnemyId++;
        net::HostEnemyState state;
        state.id = actor->netEnemyId;
        state.x = actor->position.x;
        state.y = actor->position.y;
        state.z = actor->position.z;
        state.yaw = actor->yaw;
        state.aimYaw = actor->aimYaw;
        state.aimPitch = actor->aimPitch;
        state.health = actor->health;
        state.dead = actor->Dead();
        state.moving = actor->NetworkMoving();
        // SkinnedEnemy marks its own player kills with a placeholder rather than
        // a real id, because it has no business knowing about net players. This
        // is where it becomes this host's id; a client's reported kill has
        // already been stamped with the true shooter by ApplyReportedEnemyHits.
        state.killer = actor->netKiller == SkinnedEnemy::kLocalPlayerKiller
            ? g_netSession.LocalId()
            : actor->netKiller;
        g_hostEnemyScratch.push_back(state);
    }
    g_netSession.PublishEnemies(g_hostEnemyScratch);
}

// Finds an AI actor by its replication id. Linear for the same reason
// FindNetworkPlayerBody is: g_bandits owns these and is cleared wholesale on a
// level reset, so a side map would strand dangling pointers.
static SkinnedEnemy* FindEnemyByNetId(net::EnemyId id) {
    if (id == net::kInvalidEnemyId) return nullptr;
    for (const auto& actor : g_bandits) {
        if (!actor || actor->networkControlled) continue;
        if (actor->netEnemyId == id) return actor.get();
    }
    return nullptr;
}

// Host-side: apply the hits clients reported. This is where a client's round
// actually kills something -- the client only ran the geometry test.
//
// Routed through Shoot rather than by subtracting health directly, so a
// client's kill produces exactly what a host's kill does: the same damage
// scaling, the same headshot rule, the same ragdoll, the same one-shot death
// event that pays out and plays the audio.
static void ApplyReportedEnemyHits() {
    g_netSession.DrainEnemyHits(g_enemyHitScratch);
    for (const net::EnemyHitRequest& hit : g_enemyHitScratch) {
        SkinnedEnemy* enemy = FindEnemyByNetId(hit.target);
        // Gone already: the actor died to something else between the client
        // firing and the report arriving. Dropping it is correct -- the kill
        // simply went to whoever got there first.
        if (!enemy || enemy->Dead()) continue;
        const XMFLOAT3 impact{ hit.hitX, hit.hitY, hit.hitZ };
        const XMFLOAT3 direction{ hit.dirX, hit.dirY, hit.dirZ };
        // The host pays itself only for its own reported rounds -- its own shots
        // come back through this same queue with its own id on them, so the
        // comparison covers both machines with one rule.
        const bool localKill = hit.shooter == g_netSession.LocalId();
        if (hit.headshot) {
            // The client's hit test found a head. Reproducing that here as a
            // guaranteed-lethal hit keeps the one-headshot rule identical on
            // both machines rather than re-testing geometry the client already
            // resolved against the position it could see.
            enemy->KillFromNetworkHeadshot(direction, impact, localKill);
        } else {
            enemy->ApplyNetworkBodyDamage(hit.damage, direction, impact,
                                          localKill);
        }
        // Stamped after the damage so it only names a shooter who actually
        // finished the body, and published from here rather than from the kill
        // itself because only the session knows the reporter's id.
        if (enemy->Dead()) enemy->netKiller = hit.shooter;
    }
}

// Client-side: enemies are whatever the host last said. No AI runs here at all.
static void UpdateClientEnemies(float frameDelta) {
    const std::vector<net::RemoteEnemy>& enemies = g_netSession.RemoteEnemies();
    for (const net::RemoteEnemy& remote : enemies) {
        SkinnedEnemy* body = FindEnemyByNetId(remote.id);
        if (!body) {
            if (remote.dead) continue;   // do not spawn a corpse
            if (!g_banditModel.valid) continue;
            auto spawned = std::make_unique<SkinnedEnemy>();
            if (!spawned->Init(g_banditModel)) continue;
            spawned->faction = Faction::Bandit;
            spawned->netEnemyId = remote.id;
            spawned->leftArmReach = g_banditLeftArmReach;
            // The host owns this actor's life. Local damage must not touch it
            // or the two machines disagree about who is still standing; the
            // client reports its hits and waits to be told.
            spawned->damageTakenScale = 0.0f;
            spawned->position = { remote.x, remote.y, remote.z };
            body = spawned.get();
            g_bandits.push_back(std::move(spawned));
        }
        body->position = { remote.x, remote.y, remote.z };
        body->yaw = remote.yaw;
        body->aimYaw = remote.aimYaw;
        body->aimPitch = remote.aimPitch;
        body->health = remote.health;
        if (remote.dead) {
            // Kill locally so the ragdoll, the death audio and the payout all
            // run through the paths that already exist, rather than a second
            // notion of "dead" the rest of the game does not know about.
            // Only the player the host named banks it. Everyone else gets the
            // ragdoll and the audio and nothing on the ledger.
            if (!body->Dead())
                body->KillFromNetwork(
                    { 0.0f, 0.0f, 0.0f }, body->position,
                    remote.killer != net::kInvalidPlayerId &&
                        remote.killer == g_netSession.LocalId());
            continue;
        }
        body->UpdateNetworkedPose(frameDelta, remote.moving, false);
    }

    // Drop bodies the host has stopped sending. An enemy that fell out of the
    // nearest-N window is gone from this client's view until it comes back,
    // which is what keeps a distant firefight off the wire.
    size_t localStrays = 0;
    RetireNetworkActors(
                       [&enemies, &localStrays](const SkinnedEnemy& actor) {
                           if (actor.networkControlled) return false;
                           // No host id means this client spawned it itself:
                           // the level load and restart paths still run the
                           // squad, turret-gunner and marine spawns here. Ids
                           // are only handed out by the host, and the client
                           // never runs AI on its own actors, so a body like
                           // that never got a clip and stood in its bind pose
                           // -- the T-posed marines only player 2 could see,
                           // beside the host's real ones arriving by snapshot.
                           // One rule here covers every spawn site.
                           if (actor.netEnemyId == net::kInvalidEnemyId) {
                               ++localStrays;
                               return true;
                           }
                           for (const net::RemoteEnemy& remote : enemies)
                               if (remote.id == actor.netEnemyId) return false;
                           return true;
                       });
    // Once per spawn wave (load, restart), never per frame: after this there
    // are none left to count.
    if (localStrays > 0)
        SGE_LOG("LogNet", EngineLog::Level::Display,
                "client: removed " + std::to_string(localStrays) +
                " locally spawned actors; the host's squad arrives by snapshot");
}

// Grenades are simulated by the host. A client may keep its locally thrown
// copy moving for responsiveness, but it cannot commit the blast until the
// host's detonation edge arrives.
static std::vector<net::GrenadeSpawnEvent> g_netGrenadeSpawns;
static std::vector<net::GrenadeDetonationEvent> g_netGrenadeDetonations;
// Host only: where clients say their own grenades finished up.
static std::vector<net::GrenadeDetonationEvent> g_netGrenadeDetonationReports;
static uint32_t g_nextGrenadeClientToken = 1;

// Ground the host has cut, waiting to be applied here. Reused across frames so
// the drain does not allocate, like the other scratch vectors above.
static std::vector<net::TerrainDeformEvent> g_netTerrainDeforms;
static std::vector<net::TerrainDeform> g_netRequestedTerrainDeforms;

static TerrainSculptStamp StampFromNetworkDeform(
        const net::TerrainDeform& deform, bool& valid) {
    TerrainSculptStamp stamp;
    stamp.x = deform.x;
    stamp.z = deform.z;
    stamp.radius = deform.radius;
    stamp.value = deform.value;
    stamp.strength = deform.strength;
    stamp.edgeFalloff = deform.edgeFalloff;
    stamp.baseHeight = deform.baseHeight;
    // The wire carries the operation as a byte. Only the shapes a runtime cut
    // actually uses are accepted -- Heightmap is excluded because it names a
    // texture file, which is a std::string that never crosses and would leave
    // the stamp pointing at nothing. Anything else is dropped rather than cast
    // blind into the evaluator.
    const auto operation =
        static_cast<TerrainSculptOperation>(deform.operation);
    valid = operation == TerrainSculptOperation::Crater ||
            operation == TerrainSculptOperation::Add ||
            operation == TerrainSculptOperation::Flatten;
    stamp.operation = valid ? operation : TerrainSculptOperation::Add;
    return stamp;
}

// Moves cut ground between machines, in both directions.
//
// On a client: applies what the host has committed. A client digs nothing of
// its own, so this is the only thing that changes its terrain.
//
// On the host: applies what a client asked for, then publishes it as its own.
// That request is how a client's rocket, C4 or exploding barrel is heard about
// at all -- none of them replicate as projectiles, so before this the host
// never learned a client had blown anything up and the hole appeared on neither
// machine.
static void ApplyNetworkTerrainDeforms() {
    if (!MultiplayerActive()) return;
    if (g_netSession.CurrentRole() == net::Role::Host) {
        g_netSession.DrainRequestedTerrainDeforms(g_netRequestedTerrainDeforms);
        for (const net::TerrainDeform& deform : g_netRequestedTerrainDeforms) {
            bool valid = false;
            const TerrainSculptStamp stamp =
                StampFromNetworkDeform(deform, valid);
            if (!valid) continue;
            ApplyRuntimeTerrainStamp(stamp, deform.impactY);
            // Straight back out, including to the client that asked: it applied
            // nothing locally and is waiting for this. Publishing also puts the
            // cut in the join backlog, so a later arrival gets it too.
            g_netSession.PublishTerrainDeform(deform);
        }
        return;
    }
    if (g_netSession.CurrentRole() != net::Role::Client) return;
    g_netSession.DrainTerrainDeforms(g_netTerrainDeforms);
    for (const net::TerrainDeformEvent& event : g_netTerrainDeforms) {
        bool valid = false;
        const TerrainSculptStamp stamp =
            StampFromNetworkDeform(event.deform, valid);
        if (!valid) continue;
        ApplyRuntimeTerrainStamp(stamp, event.deform.impactY);
    }
}

static net::GrenadeKind NetworkGrenadeKind(const Projectile& p) {
    return p.molotov ? net::GrenadeKind::Molotov
         : p.vortex ? net::GrenadeKind::Vortex : net::GrenadeKind::Frag;
}

static Projectile* FindNetworkGrenade(uint32_t id) {
    for (Projectile& p : scene.projectiles)
        if (p.grenade && p.netGrenadeId == id) return &p;
    return nullptr;
}

static void UpdateNetworkGrenades() {
    if (!MultiplayerActive()) {
        // A level/session teardown can leave predicted projectiles in Scene;
        // never carry their network identity into the next session.
        for (Projectile& p : scene.projectiles) {
            p.netGrenadeId = 0;
            p.netClientToken = 0;
            p.netAuthoritative = false;
            p.netAwaitingDetonation = false;
        }
        return;
    }

    g_netSession.DrainGrenadeSpawns(g_netGrenadeSpawns);
    for (const net::GrenadeSpawnEvent& spawn : g_netGrenadeSpawns) {
        Projectile* existing = (g_netSession.CurrentRole() == net::Role::Client &&
                                spawn.owner == g_netSession.LocalId() &&
                                spawn.clientToken)
            ? [&]() -> Projectile* {
                for (Projectile& p : scene.projectiles)
                    if (p.grenade && p.netClientToken == spawn.clientToken)
                        return &p;
                return nullptr;
            }() : nullptr;
        if (existing) {
            existing->netGrenadeId = spawn.grenadeId;
            existing->netAuthoritative = false;
            continue;
        }
        Projectile p{};
        p.position = p.previousPosition = { spawn.x, spawn.y, spawn.z };
        p.velocity = { spawn.velocityX, spawn.velocityY, spawn.velocityZ };
        p.grenade = true;
        p.molotov = spawn.kind == net::GrenadeKind::Molotov;
        p.vortex = spawn.kind == net::GrenadeKind::Vortex;
        p.hostile = spawn.hostile;
        p.active = true;
        p.fuse = (std::max)(0.0f, spawn.fuse);
        // An AI throw starts inside its own thrower, and a hostile grenade
        // collides with the player body the moment its grace runs out. Without
        // the same grace the local throw used, a replicated bandit frag
        // detonates on the arm that threw it.
        if (spawn.owner == net::kInvalidPlayerId)
            p.grenadeCollisionGrace = 0.18f;
        p.netGrenadeId = spawn.grenadeId;
        p.netClientToken = spawn.clientToken;
        p.netAuthoritative = g_netSession.CurrentRole() == net::Role::Host;
        scene.projectiles.push_back(p);
    }

    // The thrower has told the host where its grenade ended up. Move the
    // authoritative projectile there and let it go off this frame rather than
    // burning the rest of a fuse the client has already finished: the host's
    // own copy of the throw has drifted by now, and the blast belongs where the
    // player who threw it watched the grenade come to rest.
    if (g_netSession.CurrentRole() == net::Role::Host) {
        g_netSession.DrainGrenadeDetonationReports(g_netGrenadeDetonationReports);
        for (const net::GrenadeDetonationEvent& report :
                 g_netGrenadeDetonationReports) {
            Projectile* p = FindNetworkGrenade(report.grenadeId);
            if (!p || !p->active) continue;
            ReleaseGrenadePhysicsBody(*p);
            p->position = p->previousPosition =
                { report.x, report.y, report.z };
            p->velocity = {};
            p->fuse = 0.0f;
            p->active = false;
            p->detonate = true;
        }
    }

    g_netSession.DrainGrenadeDetonations(g_netGrenadeDetonations);
    for (const net::GrenadeDetonationEvent& event : g_netGrenadeDetonations) {
        Projectile* p = FindNetworkGrenade(event.grenadeId);
        if (!p) {
            Projectile blast{};
            blast.grenade = true;
            blast.molotov = event.kind == net::GrenadeKind::Molotov;
            blast.vortex = event.kind == net::GrenadeKind::Vortex;
            blast.hostile = event.hostile;
            blast.position = blast.previousPosition =
                { event.x, event.y, event.z };
            blast.netGrenadeId = event.grenadeId;
            blast.netAuthoritative = true;
            blast.active = false;
            blast.detonate = true;
            scene.projectiles.push_back(blast);
            continue;
        }
        // The local physics pose must not overwrite the host's blast center
        // during the two physics synchronizations before the projectile pass.
        ReleaseGrenadePhysicsBody(*p);
        p->position = { event.x, event.y, event.z };
        p->active = false;
        p->detonate = true;
        p->netAwaitingDetonation = false;
        p->netAuthoritative = true;
    }

    for (Projectile& p : scene.projectiles) {
        if (!p.grenade || p.held || (!p.active && !p.detonate) ||
            p.netGrenadeId != 0 || p.netClientToken != 0 ||
            p.missile || p.remoteCharge) continue;
        if (g_netSession.CurrentRole() == net::Role::Host) {
            const uint32_t id = g_netSession.PublishGrenadeSpawn(
                0, g_netSession.LocalId(), NetworkGrenadeKind(p),
                p.position.x, p.position.y, p.position.z,
                p.velocity.x, p.velocity.y, p.velocity.z,
                (std::max)(0.0f, p.fuse),
                p.hostile);
            if (id != 0) {
                p.netGrenadeId = id;
                p.netAuthoritative = true;
            }
        } else {
            uint32_t token = g_nextGrenadeClientToken++;
            if (token == 0) token = g_nextGrenadeClientToken++;
            p.netClientToken = token;
            p.netAwaitingDetonation = true;
            g_netSession.ReportGrenadeThrow(
                token, NetworkGrenadeKind(p), p.position.x, p.position.y,
                p.position.z, p.velocity.x, p.velocity.y, p.velocity.z,
                (std::max)(0.0f, p.fuse));
        }
    }
}

static void ShutdownMultiplayer() {
    RetireNetworkActors([](const SkinnedEnemy& actor) {
        return actor.networkControlled;
    });
    g_netSession.Shutdown();
}

// Parses -host [port] / -join <address> [port] out of the command line and
// starts the session. A failure is reported and then ignored: not being able
// to host is a reason to stay single-player, not a reason to refuse to launch.
// "address:port", "address port" or just "address". Steam hands back whatever
// string the host put in its `connect` rich presence, so this parses the shape
// this game writes rather than assuming a well-formed pair.
static bool JoinFromConnectString(const std::string& connect) {
    constexpr uint16_t kDefaultPort = 27015;
    std::string address = connect;
    // Steam launches the game with the connect value as arguments, so the
    // leading "+connect" is still attached when it arrives that way.
    const size_t flag = address.find("+connect");
    if (flag != std::string::npos) address.erase(0, flag + 8);
    const size_t first = address.find_first_not_of(" \t\"");
    if (first == std::string::npos) return false;
    address.erase(0, first);
    const size_t last = address.find_last_not_of(" \t\"");
    address.erase(last + 1);

    uint16_t port = kDefaultPort;
    // "steam:<id64>" is one token with a colon in it, not an address and a
    // port: the relay has no port to dial, and splitting here would hand the
    // transport half a SteamID.
    const size_t separator = net::IsSteamAddress(address)
        ? std::string::npos : address.find_last_of(": ");
    if (separator != std::string::npos) {
        const int parsed = std::atoi(address.c_str() + separator + 1);
        if (parsed > 0 && parsed <= 65535) {
            port = static_cast<uint16_t>(parsed);
            address.erase(separator);
        }
    }
    if (address.empty()) return false;

    std::string error;
    if (!g_netSession.StartClient(address, port, &error)) {
        SGE_LOG("LogNet", EngineLog::Level::Warning,
            "join from Steam failed (" + address + ":" +
            std::to_string(port) + "): " + error);
        // The invite arrives while the player is looking at the menu, so the
        // menu is where the failure has to appear; without this an invite that
        // cannot be honoured is indistinguishable from one that never came.
        g_multiplayerStatusError = "Could not join: " + error;
        return false;
    }
    g_multiplayerStatusError.clear();
    SGE_LOG("LogNet", EngineLog::Level::Display,
        "joining " + address + ":" + std::to_string(port) + " from Steam");
    return true;
}

static void StartMultiplayerFromCommandLine(const std::string& commandLine) {
    std::vector<std::string> arguments;
    std::istringstream stream(commandLine);
    for (std::string token; stream >> token;) arguments.push_back(token);

    constexpr uint16_t kDefaultPort = 27015;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        std::string error;
        // How a friend's "Join Game" arrives when the game was not already
        // running: Steam launches it with the host's connect string appended.
        if (argument == "+connect") {
            if (i + 1 >= arguments.size()) {
                std::cerr << "+connect needs an address\n";
                return;
            }
            JoinFromConnectString(arguments[i + 1]);
            return;
        }
        // -host is the direct UDP form, which is what the LAN test script and
        // a hand-typed address need. -host-steam puts the session on Steam's
        // relay instead, where a friend joins by SteamID and neither end needs
        // a reachable address.
        if (argument == "-host" || argument == "-host-steam") {
            const bool overSteam = argument == "-host-steam";
            uint16_t port = kDefaultPort;
            if (i + 1 < arguments.size() && arguments[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(arguments[++i].c_str()));
            if (g_netSession.StartHost(port, &error, overSteam)) {
                // Logged rather than only printed: which transport a session
                // actually got is the first thing worth knowing when a friend
                // cannot reach it, and -host-steam falls back to IP silently
                // when Steam is not running.
                const std::string where = g_netSession.OverSteam()
                    ? "Steam as " + g_netSession.ConnectAddress()
                    : "port " + std::to_string(port);
                std::cout << "Hosting on " << where << "\n";
                SGE_LOG("LogNet", EngineLog::Level::Display,
                    "hosting on " + where);
            } else {
                std::cerr << "Failed to host: " << error << "\n";
                SGE_LOG("LogNet", EngineLog::Level::Warning,
                    "failed to host: " + error);
            }
            return;
        }
        if (argument == "-join") {
            if (i + 1 >= arguments.size()) {
                std::cerr << "-join needs an address\n";
                SGE_LOG("LogNet", EngineLog::Level::Warning,
                    "-join needs an address");
                return;
            }
            std::string address = arguments[++i];
            // Quotes and stray whitespace survive a copy-paste out of a chat
            // window, and ParseSteamId rejects any non-digit outright -- so a
            // pasted id with a trailing quote failed for a reason that looked
            // nothing like the cause. Trimmed here the way the Steam invite
            // path already trims it.
            const size_t first = address.find_first_not_of(" \t\"");
            if (first != std::string::npos) {
                address.erase(0, first);
                address.erase(address.find_last_not_of(" \t\"") + 1);
            }
            uint16_t port = kDefaultPort;
            if (i + 1 < arguments.size() && arguments[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(arguments[++i].c_str()));
            // Logged, not just printed: the console this writes to belongs to
            // AllocConsole and is gone with the process, which left a failed
            // command-line join as the one path in this file with no trace.
            if (g_netSession.StartClient(address, port, &error)) {
                std::cout << "Joining " << address << ":" << port << "\n";
                SGE_LOG("LogNet", EngineLog::Level::Display,
                    "joining " + address + ":" + std::to_string(port));
            } else {
                std::cerr << "Failed to join: " << error << "\n";
                SGE_LOG("LogNet", EngineLog::Level::Warning,
                    "failed to join " + address + ":" + std::to_string(port) +
                    ": " + error);
                g_multiplayerStatusError = "Could not join: " + error;
            }
            return;
        }
    }
}

// Drives the session itself: polling, the handshake, the net tick and sending.
//
// Called every frame from any screen, deliberately NOT gated on gameplay.
// NetSession::Update is the only caller of transport_->Poll(), so gating this
// on IsGameplayScreen meant no packet was ever read while a player sat in the
// menu -- which made the handshake impossible to complete and, with it, joining
// from the menu at all.
static void UpdateMultiplayerSession(float frameDelta,
                                     const PlayerInput& localInput) {
    // A session can end without anyone here asking it to -- the host quit, or
    // the connection never came up at all. The reason is taken once and shown
    // in the menu; the actors it leaves behind are the same ones an explicit
    // disconnect has to clear, so the same cleanup runs.
    std::string ended = g_netSession.TakeLastError();
    if (!ended.empty()) {
        g_multiplayerStatusError = std::move(ended);
        ShutdownMultiplayer();
    }
    if (!MultiplayerActive()) {
        // Drop the sink as soon as there is no session, so a disconnected
        // player's damage stops being reported into nothing and single-player
        // is left exactly as it was.
        if (scene.playerDamageNetworkSink) scene.playerDamageNetworkSink = {};
        // Leaving a session while down must not strand the player on the floor
        // with no one left who could ever pick them up. Nothing outside a
        // session can clear this, so it is cleared here.
        if (scene.player.downed) {
            scene.player.downed = false;
            scene.player.reviveProgress = 0.0f;
            if (scene.player.health <= 0.0f)
                scene.player.health = net::kReviveHealth;
        }
        return;
    }
    // Installed here rather than at StartHost/StartClient because there are
    // several ways to open a session (menu, two command-line flags) and only
    // one place that runs every frame one is live.
    if (!scene.playerDamageNetworkSink) {
        scene.playerDamageNetworkSink = [](float damage) {
            g_netSession.ReportLocalDamage(damage);
        };
    }

    net::LocalPlayerState local;
    local.input = localInput;
    // Only the host's is used: it becomes the session's god mode.
    local.godMode = scene.player.godMode;
    local.x = scene.camera.Position.x;
    // Report the feet rather than the eye: a remote body is placed by its feet,
    // and sending the eye would sink every other player waist-deep in terrain.
    local.y = scene.camera.Position.y - scene.camera.PlayerHeight;
    local.z = scene.camera.Position.z;
    if (IsGameplayScreen() && !g_game.loading.Active() &&
        !g_insertionChoicePending && !g_baseMode && BlackHawkVisible()) {
        auto& helicopter = local.helicopter;
        helicopter.visible = 1;
        helicopter.airframe = static_cast<uint8_t>(g_insertionAirframe);
        helicopter.x = g_blackHawkPosition.x;
        helicopter.y = g_blackHawkPosition.y;
        helicopter.z = g_blackHawkPosition.z;
        helicopter.yaw = XMConvertToDegrees(g_blackHawkYaw);
        helicopter.pitch = XMConvertToDegrees(g_game.vehicles.blackHawkPitch);
        helicopter.roll = XMConvertToDegrees(g_game.vehicles.blackHawkRoll);
        helicopter.centerX = g_blackHawkModelCenter.x;
        helicopter.minY = g_blackHawkModelMinY;
        helicopter.centerZ = g_blackHawkModelCenter.z;
        helicopter.scale = g_blackHawkModelScale;
    }
    if (g_insertionChoicePending) {
        local.x = g_deploymentNetworkPosition.x;
        local.y = g_deploymentNetworkPosition.y;
        local.z = g_deploymentNetworkPosition.z;
        local.input = {};
        local.input.sequence = localInput.sequence;
        local.input.deltaTime = localInput.deltaTime;
    }
    g_netSession.Update(frameDelta, local);

    // The host owns our health in a session, so pull it back rather than
    // letting the local copy drift. Everything that reads health -- the bar,
    // the low-health vignette, the death gate -- then works from one number.
    //
    // Local damage is still applied locally first for the flash and the chip
    // bar; this is the correction that arrives a tick later, which is why it
    // overwrites rather than subtracts.
    net::LocalPlayerStatus status;
    if (g_netSession.LocalStatus(status)) {
        scene.player.health = status.health;
        scene.player.downed = status.downed;
        scene.player.reviveProgress = status.reviveProgress;
    }

    // A client follows the host's god mode. The host already skips all player
    // damage while it is on; mirroring it here is what makes the rest of the
    // client agree -- the death gate, unlimited ammo, the reward cap and the
    // GOD MODE label all read scene.player.godMode.
    if (g_netSession.CurrentRole() == net::Role::Client) {
        bool known = false;
        const bool hostGodMode = g_netSession.SessionGodMode(known);
        if (known && scene.player.godMode != hostGodMode) {
            scene.player.godMode = hostGodMode;
            // Same restock the menu does when god mode is switched off: the
            // unenforced path leaves magazines in whatever state it left them.
            if (!hostGodMode) scene.player.RestoreAmmo();
        }
    }

    // Life-state edges. Drained every frame whether or not anything is
    // listening, so the queue cannot grow unbounded in a long session.
    g_netSession.DrainStateChanges(g_netStateChanges);
    for (const net::PlayerStateChange& change : g_netStateChanges) {
        const int playerNumber = static_cast<int>(change.id) + 1;
        if (change.event == net::PlayerStateEvent::Downed) {
            SGE_LOG("LogNet", EngineLog::Level::Display,
                    "player " + std::to_string(playerNumber) + " is down");
        } else if (change.event == net::PlayerStateEvent::Revived) {
            SGE_LOG("LogNet", EngineLog::Level::Display,
                    "player " + std::to_string(playerNumber) + " is back up");
        }
    }
}

// Moves and animates the remote player bodies. Gameplay-only: these need a
// live level, a terrain height to stand on and a loaded marine model, none of
// which exist at the menu.
static void UpdateMultiplayerBodies(float frameDelta) {
    if (!MultiplayerActive()) return;

    // Apply the session's interpolated view to the bodies. On the host this
    // only moves other players; on a client it moves everyone but the local
    // player, whose own position stays locally predicted.
    g_netSession.GetRemotePlayers(g_netRemoteScratch);
    // One-shot trace of the first frame that has remote players to place, and
    // of every spawn. Placed here because "connected but invisible" cannot be
    // told apart from "no remote players in the snapshot" from the outside.
    static bool tracedRemotes = false;
    if (!tracedRemotes && !g_netRemoteScratch.empty()) {
        tracedRemotes = true;
        SGE_LOG("LogNet", EngineLog::Level::Display,
                "bodies: remotes=" + std::to_string(g_netRemoteScratch.size()) +
                " marineModel=" + std::to_string(g_marineModel.valid ? 1 : 0) +
                " bandits=" + std::to_string(g_bandits.size()) +
                " firstPos=" + std::to_string(g_netRemoteScratch[0].x) + "," +
                std::to_string(g_netRemoteScratch[0].y) + "," +
                std::to_string(g_netRemoteScratch[0].z) +
                " camera=" + std::to_string(scene.camera.Position.x) + "," +
                std::to_string(scene.camera.Position.y) + "," +
                std::to_string(scene.camera.Position.z));
    }
    for (const net::RemotePlayer& remote : g_netRemoteScratch) {
        SkinnedEnemy* body = FindNetworkPlayerBody(remote.id);
        if (!body) {
            body = SpawnNetworkPlayerBody(remote.id);
            if (!body) {
                SGE_LOG("LogNet", EngineLog::Level::Warning,
                        "spawn FAILED for player " +
                        std::to_string(static_cast<int>(remote.id)) +
                        " (marineModel valid=" +
                        std::to_string(g_marineModel.valid ? 1 : 0) + ")");
                continue;
            }
            SGE_LOG("LogNet", EngineLog::Level::Display,
                    "spawned body for player " +
                    std::to_string(static_cast<int>(remote.id)) + " at " +
                    std::to_string(remote.x) + "," + std::to_string(remote.y) +
                    "," + std::to_string(remote.z));
            // Start exactly where the snapshot says instead of interpolating
            // in from the origin, which would drag the new body across the map.
            body->position = { remote.x, remote.y, remote.z };
        }
        body->position = { remote.x, remote.y, remote.z };
        // Camera yaw faces (cos(yaw), sin(yaw)) in XZ; the mesh's +Z forward
        // rotates to (sin(yaw), cos(yaw)), so units alone are not enough.
        const float yawRadians = DirectX::XM_PIDIV2 -
                                 DirectX::XMConvertToRadians(remote.yaw);
        body->yaw = yawRadians;
        body->aimYaw = yawRadians;
        // Look angle, so the body points its weapon where its owner is aiming
        // instead of levelling it at the horizon whatever the player does. Only
        // yaw was applied before, which read fine on flat ground and wrongly
        // everywhere else -- someone shooting down from a roof or up at a
        // gunship still held the rifle flat.
        //
        // Camera pitch is degrees and positive up; aimPitch is radians and
        // positive up, so the sign carries straight across. Clamped to the same
        // +/-0.55 rad the AI aim uses: that is what the rig's spine and gun IK
        // are built for, and a player can pitch nearly +/-90, which would tear
        // the pose apart.
        body->aimPitch = (std::max)(-0.55f, (std::min)(0.55f,
            DirectX::XMConvertToRadians(remote.pitch)));
        // Mirror the authoritative life state onto the body. One direction
        // only: the host decides, and a local write here would be a desync
        // nobody else can see.
        body->netDowned = remote.downed;
        body->netHealth = remote.health;
        body->UpdateNetworkedPose(frameDelta, remote.moving, remote.sprinting,
                                  remote.aiming);
    }

    // Drop bodies for players that are no longer in the session. Erases in one
    // pass rather than erasing one id at a time, so the vector is never
    // mutated while something else holds an iterator into it.
    RetireNetworkActors(
                       [](const SkinnedEnemy& actor) {
                           if (!actor.networkControlled) return false;
                           for (const net::RemotePlayer& remote :
                                    g_netRemoteScratch)
                               if (remote.id == actor.netPlayerId) return false;
                           return true;
                       });

    if (g_netSession.CurrentRole() == net::Role::Host) {
        // Apply what clients reported hitting, then publish the result. Both
        // after the player bodies above, so a hit reported this frame lands on
        // the positions the snapshot is about to carry.
        ApplyReportedEnemyHits();
        PublishHostEnemies();
        // After the hits: a gunship brought down by a client's report this
        // frame goes out already dead rather than flying for one more tick.
        PublishHostVehicles();
    } else {
        UpdateClientEnemies(frameDelta);
    }
}
