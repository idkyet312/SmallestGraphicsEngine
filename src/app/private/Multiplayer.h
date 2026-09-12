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

static void DamageBulletPrefabEntity(uint64_t entityId, float damage,
                                      const XMFLOAT3& hit, bool remoteCharge,
                                      bool playerOwned) {
    if (MultiplayerActive() && !remoteCharge)
        g_netSession.ReportWorldImpact(entityId, damage, hit.x, hit.y, hit.z,
                                       /*kind=*/1, 0.0f, 0.0f, 0, 0.0f, 0.0f,
                                       0.0f, playerOwned);
    else
        DamagePrefabEntity(entityId, damage, hit, remoteCharge, playerOwned);
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
                                    bool spawnImpactFx, bool playerOwned) {
    const XMFLOAT3 normal{ -dirX, -dirY, -dirZ };
    if (kind == 1) {
        // spawnImpactFx doubles as "this was not my round": the shot's own
        // machine already marked and sparked at the trigger pull.
        DamagePrefabEntity(entityId, damage, hit, false, playerOwned,
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
                                    impact.playerOwned);
            g_netSession.PublishWorldBreak(
                impact.kind, impact.entityId,
                impact.damage, impact.radius, impact.impulse,
                impact.hitX, impact.hitY, impact.hitZ,
                impact.dirX, impact.dirY, impact.dirZ, impact.shooter,
                impact.playerOwned);
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
                                    impact.playerOwned);
        }
    }
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
        if (hit.headshot) {
            // The client's hit test found a head. Reproducing that here as a
            // guaranteed-lethal hit keeps the one-headshot rule identical on
            // both machines rather than re-testing geometry the client already
            // resolved against the position it could see.
            enemy->KillFromNetworkHeadshot(direction, impact);
        } else {
            enemy->ApplyNetworkBodyDamage(hit.damage, direction, impact);
        }
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
            if (!body->Dead())
                body->KillFromNetwork({ 0.0f, 0.0f, 0.0f }, body->position);
            continue;
        }
        body->UpdateNetworkedPose(frameDelta, remote.moving, false);
    }

    // Drop bodies the host has stopped sending. An enemy that fell out of the
    // nearest-N window is gone from this client's view until it comes back,
    // which is what keeps a distant firefight off the wire.
    g_bandits.erase(
        std::remove_if(g_bandits.begin(), g_bandits.end(),
                       [&enemies](const std::unique_ptr<SkinnedEnemy>& actor) {
                           if (!actor || actor->networkControlled) return false;
                           if (actor->netEnemyId == net::kInvalidEnemyId)
                               return false;
                           for (const net::RemoteEnemy& remote : enemies)
                               if (remote.id == actor->netEnemyId) return false;
                           return true;
                       }),
        g_bandits.end());
}

// Grenades are simulated by the host. A client may keep its locally thrown
// copy moving for responsiveness, but it cannot commit the blast until the
// host's detonation edge arrives.
static std::vector<net::GrenadeSpawnEvent> g_netGrenadeSpawns;
static std::vector<net::GrenadeDetonationEvent> g_netGrenadeDetonations;
static uint32_t g_nextGrenadeClientToken = 1;

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
        p.netGrenadeId = spawn.grenadeId;
        p.netClientToken = spawn.clientToken;
        p.netAuthoritative = g_netSession.CurrentRole() == net::Role::Host;
        scene.projectiles.push_back(p);
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
    g_bandits.erase(
        std::remove_if(g_bandits.begin(), g_bandits.end(),
                       [](const std::unique_ptr<SkinnedEnemy>& actor) {
                           return actor && actor->networkControlled;
                       }),
        g_bandits.end());
    g_netSession.Shutdown();
}

// Parses -host [port] / -join <address> [port] out of the command line and
// starts the session. A failure is reported and then ignored: not being able
// to host is a reason to stay single-player, not a reason to refuse to launch.
static void StartMultiplayerFromCommandLine(const std::string& commandLine) {
    std::vector<std::string> arguments;
    std::istringstream stream(commandLine);
    for (std::string token; stream >> token;) arguments.push_back(token);

    constexpr uint16_t kDefaultPort = 27015;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        std::string error;
        if (argument == "-host") {
            uint16_t port = kDefaultPort;
            if (i + 1 < arguments.size() && arguments[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(arguments[++i].c_str()));
            if (g_netSession.StartHost(port, &error))
                std::cout << "Hosting on port " << port << "\n";
            else
                std::cerr << "Failed to host: " << error << "\n";
            return;
        }
        if (argument == "-join") {
            if (i + 1 >= arguments.size()) {
                std::cerr << "-join needs an address\n";
                return;
            }
            const std::string address = arguments[++i];
            uint16_t port = kDefaultPort;
            if (i + 1 < arguments.size() && arguments[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(arguments[++i].c_str()));
            if (g_netSession.StartClient(address, port, &error))
                std::cout << "Joining " << address << ":" << port << "\n";
            else
                std::cerr << "Failed to join: " << error << "\n";
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
    local.x = scene.camera.Position.x;
    // Report the feet rather than the eye: a remote body is placed by its feet,
    // and sending the eye would sink every other player waist-deep in terrain.
    local.y = scene.camera.Position.y - scene.camera.PlayerHeight;
    local.z = scene.camera.Position.z;
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
        // Mirror the authoritative life state onto the body. One direction
        // only: the host decides, and a local write here would be a desync
        // nobody else can see.
        body->netDowned = remote.downed;
        body->netHealth = remote.health;
        body->UpdateNetworkedPose(frameDelta, remote.moving, remote.sprinting);
    }

    // Drop bodies for players that are no longer in the session. Erases in one
    // pass rather than erasing one id at a time, so the vector is never
    // mutated while something else holds an iterator into it.
    g_bandits.erase(
        std::remove_if(g_bandits.begin(), g_bandits.end(),
                       [](const std::unique_ptr<SkinnedEnemy>& actor) {
                           if (!actor || !actor->networkControlled) return false;
                           for (const net::RemotePlayer& remote :
                                    g_netRemoteScratch)
                               if (remote.id == actor->netPlayerId) return false;
                           return true;
                       }),
        g_bandits.end());

    if (g_netSession.CurrentRole() == net::Role::Host) {
        // Apply what clients reported hitting, then publish the result. Both
        // after the player bodies above, so a hit reported this frame lands on
        // the positions the snapshot is about to carry.
        ApplyReportedEnemyHits();
        PublishHostEnemies();
    } else {
        UpdateClientEnemies(frameDelta);
    }
}
