#ifndef NET_SESSION_H
#define NET_SESSION_H

#include "EngineLogger.h"
#include "FixedStepClock.h"
#include "NetProtocol.h"
#include "NetTransport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Owns "are we in a multiplayer game, and who is in it".
//
// Deliberately knows nothing about rendering, the camera, or the AppState
// globals: it takes the local player's input and position in, and hands back
// where every remote player is. main.cpp does the translation. That boundary is
// what keeps the ~234 globals in AppState.h out of the network layer, and it is
// why this can be unit tested against the null transport with no device.
namespace net {

enum class Role : uint8_t { Offline, Host, Client };

// PvP tuning. All of these are starting values meant to be played with rather
// than defended -- they live here so changing one is a single edit, not a hunt
// through gameplay code.
inline constexpr float kMaxPlayerHealth = 100.0f;
// Matches SkinnedEnemy::Shoot's balance comment: one headshot, five body hits.
inline constexpr float kBodyShotDamage = 20.0f;
// How long a reviver has to stand there holding the key.
inline constexpr float kReviveSeconds = 3.0f;
// How close they have to be. Re-checked by the host against its own copy of
// both positions, so a client cannot claim a revive from across the map.
inline constexpr float kReviveRadius = 2.2f;
// What a revived player stands up with. Deliberately well under full: getting
// picked up should leave you vulnerable, not reset the fight.
inline constexpr float kReviveHealth = 40.0f;
// Out-of-combat regeneration, mirroring PlayerState's single-player numbers
// (regenDelay 5s, then maxHealth over regenDuration 2s). The host has to run
// this itself: it owns health, and it overwrites every client's local copy
// each frame, so a client healing on its own would be undone a tick later.
inline constexpr float kRegenDelaySeconds = 5.0f;
inline constexpr float kRegenPerSecond = kMaxPlayerHealth / 2.0f;

// Where a remote player is, already interpolated and ready to drive a body.
struct RemotePlayer {
    InsertionHelicopterState helicopter;
    // Newest snapshot's, not interpolated: the seat is a state, and the pose it
    // carries is only read by the host, which gets it straight off the input.
    DrivenVehicleState vehicle;
    PlayerId id = kInvalidPlayerId;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    bool moving = false;
    bool crouching = false;
    bool sprinting = false;
    bool aiming = false;
    bool active = false;
    float health = kMaxPlayerHealth;
    bool downed = false;
    PlayerId reviver = kInvalidPlayerId;
    float reviveProgress = 0.0f;   // 0..1
};

// One AI actor as the game should render it. The client's view of an enemy is
// entirely this: it runs no enemy AI of its own.
struct RemoteEnemy {
    EnemyId id = kInvalidEnemyId;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, aimYaw = 0.0f, aimPitch = 0.0f;
    float health = 0.0f;
    bool moving = false;
    bool dead = false;
    // Who the host says killed it, or kInvalidPlayerId for an AI or hazard
    // death. The only thing a client can key a payout off.
    PlayerId killer = kInvalidPlayerId;
    bool marine = false;
};

// A demolition charge another player planted, and the order to fire one
// player's charges. Both are plain events: the charge is placed once and then
// sits, and the detonator fires whatever its owner has out.
struct RemoteChargeStuck {
    PlayerId owner = kInvalidPlayerId;
    uint32_t chargeId = 0;
    ChargeStickData charge;
};

struct ChargeStickRequest {
    PlayerId owner = kInvalidPlayerId;
    ChargeStickData charge;
};

struct RemoteChargeDetonate {
    PlayerId owner = kInvalidPlayerId;
};

// A round another player fired, for presentation only. Never carries damage:
// what a shot hits is settled by the hit-report path, and a receiver that acted
// on this would apply the same round twice.
struct RemoteShot {
    PlayerId shooter = kInvalidPlayerId;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
};

// A round a host tank or AA gun fired, for the client to spawn as a live
// hostile projectile. Unlike RemoteShot this one does damage -- but only to
// the receiving machine's own player, through the same local hostile-hit path
// a bandit round takes, which the host already trusts a client to report.
// The host's DEPLOY SQUAD order, as a client receives it.
struct SquadDeployOrder {
    uint8_t insertionMode = 0;
    uint8_t airframe = 0;
    bool hostLeftSeat = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct RemoteEnemyFire {
    EnemyFireKind kind = EnemyFireKind::TankShell;
    InfantryWeapon weapon = InfantryWeapon::Rifle;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    float speed = 0.0f;
    float lifetime = 0.0f;
    float damageScale = 1.0f;
};

struct BarrelEventRecord {
    uint16_t barrel = 0;
    BarrelEvent event = BarrelEvent::Detonate;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct BlastRecord {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float radius = 0.0f;
    float damage = 0.0f;
    float push = 0.0f;
    float ragdollImpulse = 0.0f;
};

// One enemy gunship as the gameplay layer sees it. Used in both directions: the
// host fills it from its vehicle state, a client reads it back to overwrite its
// own. Mirrors EnemyHelicopterSnapshot without the wire packing.
struct EnemyHelicopterState {
    bool present = false;
    bool dead = false;
    bool crashed = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    float health = 0.0f;
};

// What the host publishes about one of its enemies each tick. The host fills a
// vector of these from g_bandits; the session does the culling and the sending.
struct HostEnemyState {
    EnemyId id = kInvalidEnemyId;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, aimYaw = 0.0f, aimPitch = 0.0f;
    float health = 0.0f;
    bool moving = false;
    bool dead = false;
    PlayerId killer = kInvalidPlayerId;
    bool marine = false;
};

// A client's squad, landed by the host where that client's transport set down.
struct MarineDropRequest {
    PlayerId requester = kInvalidPlayerId;
    uint8_t count = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// A hit a client reported on one of the host's enemies, drained by the host and
// applied to the real actor. The session cannot apply it itself: it knows
// nothing about SkinnedEnemy, which is the boundary that keeps it testable.
struct EnemyHitRequest {
    EnemyId target = kInvalidEnemyId;
    PlayerId shooter = kInvalidPlayerId;
    float damage = 0.0f;
    bool headshot = false;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
};

// The local player's own authoritative life state, read back every frame so the
// HUD, the downed gate and the camera all work from one number.
struct LocalPlayerStatus {
    float health = kMaxPlayerHealth;
    bool downed = false;
    // 0..1, how far along someone is at picking us up.
    float reviveProgress = 0.0f;
    PlayerId reviver = kInvalidPlayerId;
};

// One chat line as the game should display it. Carries the speaker's id rather
// than a formatted name: the session knows nothing about how the HUD wants to
// label a player, and the id is what every machine already agrees on.
struct ChatLine {
    PlayerId speaker = kInvalidPlayerId;
    // std::string rather than the wire's char buffer: by this point the text
    // has been terminated and validated, and the consumer is ordinary C++.
    std::string text;
    // True for the local player's own line, so the HUD can tint it. Resolved
    // here because the session is what knows which id is local.
    bool fromLocalPlayer = false;
};

// A life-state transition that just happened, drained once per frame by the
// game so it can fire one-shot effects. Populated from the reliable event on a
// client and directly by the host, so both ends run the same code.
struct PlayerStateChange {
    PlayerId id = kInvalidPlayerId;
    PlayerStateEvent event = PlayerStateEvent::Downed;
    PlayerId instigator = kInvalidPlayerId;
    float health = 0.0f;
    float impulseX = 0.0f, impulseY = 0.0f, impulseZ = 0.0f;
    float impactX = 0.0f, impactY = 0.0f, impactZ = 0.0f;
};

struct WorldImpactRequest {
    uint32_t impactId = 0;
    uint8_t kind = 0;
    PlayerId shooter = kInvalidPlayerId;
    bool playerOwned = true;
    // Set for a demolition charge. Carried end to end because the objectives
    // only a charge may destroy check it before taking any damage at all.
    bool remoteCharge = false;
    uint64_t entityId = 0;
    float damage = 0.0f;
    float radius = 0.0f;
    float impulse = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
};

struct WorldBreakEvent {
    uint32_t impactId = 0;
    uint8_t kind = 0;
    // The player whose round caused it, so a receiver can skip the impact FX
    // for its own shot, which it drew when it fired.
    PlayerId shooter = kInvalidPlayerId;
    bool playerOwned = true;
    bool remoteCharge = false;
    uint64_t entityId = 0;
    float damage = 0.0f;
    float radius = 0.0f;
    float impulse = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
};

struct GrenadeSpawnEvent {
    uint32_t grenadeId = 0;
    uint32_t clientToken = 0;
    PlayerId owner = kInvalidPlayerId;
    GrenadeKind kind = GrenadeKind::Frag;
    bool hostile = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float velocityX = 0.0f, velocityY = 0.0f, velocityZ = 0.0f;
    float fuse = 0.0f;
};

struct GrenadeDetonationEvent {
    uint32_t grenadeId = 0;
    GrenadeKind kind = GrenadeKind::Frag;
    bool hostile = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// A cut in the ground the host has committed. The client applies it verbatim;
// it does no crater arithmetic of its own.
struct TerrainDeformEvent {
    uint32_t deformId = 0;
    TerrainDeform deform;
};

// What the local player is doing this frame, handed to the session.
struct LocalPlayerState {
    InsertionHelicopterState helicopter;
    DrivenVehicleState vehicle;
    PlayerInput input;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // This machine's god mode toggle. Only the host's is read: it becomes the
    // session's, and a client's is replaced by whatever the host says.
    bool godMode = false;
};

class NetSession {
public:
    // 30 Hz. Independent of both the render frame and the 60 Hz destruction
    // step: snapshots do not need to be as frequent as physics, and tying them
    // to the render rate would make bandwidth depend on framerate.
    static constexpr float kNetTickSeconds = 1.0f / 30.0f;
    // Remote players are rendered this far in the past so there are always two
    // snapshots to interpolate between. One tick plus a little slack -- enough
    // to hide ordinary jitter without the delay becoming visible as lag.
    static constexpr float kInterpolationDelay = kNetTickSeconds * 2.0f;

    NetSession() : clock_(kNetTickSeconds, 4) {}

    // `overSteam` hosts on Steam's relay network instead of a UDP port, which
    // is the only form a friend outside this network can reach: they connect to
    // a SteamID, and Steam routes it. It falls back to the IP transport when
    // Steam is not running, so hosting never fails for want of a client.
    bool StartHost(uint16_t port, std::string* error, bool overSteam = false) {
        Shutdown();
        transport_ = MakeTransport(overSteam);
        if (!transport_->Listen(port, error)) { transport_.reset(); return false; }
        overSteam_ = overSteam && SteamTransportAvailable();
        role_ = Role::Host;
        // Kept so the session can be described to someone outside it -- the
        // Steam "Join Game" button needs an address and a port to hand a friend.
        port_ = port;
        serverAddress_.clear();
        // The host is always player 0, assigned without a handshake: it never
        // connects to itself.
        localId_ = 0;
        players_[0].active = true;
        players_[0].id = 0;
        return true;
    }

    // A "steam:<id64>" host goes over the relay; anything else is an address
    // and a port. The caller does not choose the transport -- the shape of the
    // address does, so a connect string can travel through a command line or a
    // friend's invite without carrying a second field saying what it is.
    bool StartClient(const std::string& host, uint16_t port,
                     std::string* error) {
        Shutdown();
        const bool steam = IsSteamAddress(host);
        transport_ = MakeTransport(steam);
        if (steam && !SteamTransportAvailable()) {
            if (error) *error = "that invite needs Steam, which is not running";
            transport_.reset();
            return false;
        }
        if (!transport_->Connect(host, port, error)) {
            transport_.reset();
            return false;
        }
        overSteam_ = steam;
        role_ = Role::Client;
        // A client passes the same host on to its own friends: "join my game"
        // means the session, not this machine.
        serverAddress_ = host;
        port_ = port;
        // Stays invalid until the welcome arrives; nothing is sent before then.
        localId_ = kInvalidPlayerId;
        return true;
    }

    // Where a friend would have to connect to reach this session. Empty on an
    // IP host, which does not know its own address; the caller supplies that.
    const std::string& ServerAddress() const { return serverAddress_; }
    uint16_t Port() const { return port_; }
    bool OverSteam() const { return overSteam_; }

    // The address to hand a friend: a SteamID when this session runs on the
    // relay, empty when it does not and the caller has to find the machine's
    // own IP. A client passes on the host it is connected to -- "join my game"
    // means the session, and there is only one machine in it taking players.
    std::string ConnectAddress() const {
        if (!overSteam_) return serverAddress_;
        if (role_ == Role::Client) return serverAddress_;
        const uint64_t local = SteamTransportLocalId();
        return local ? "steam:" + std::to_string(local) : std::string();
    }

    void Shutdown() {
        if (transport_) transport_->Disconnect();
        transport_.reset();
        role_ = Role::Offline;
        localId_ = kInvalidPlayerId;
        players_ = {};
        peerToPlayer_.clear();
        stateChanges_.clear();
        chatLines_.clear();
        medicCalls_.clear();
        marineDrops_.clear();
        scoreboard_.clear();
        sessionGodMode_ = false;
        sessionGodModeKnown_ = false;
        hostEnemies_.clear();
        remoteEnemies_.clear();
        remoteShots_.clear();
        chargeSticks_.clear();
        chargeStickRequests_.clear();
        chargeDetonations_.clear();
        activeCharges_.clear();
        receivedChargeIds_.clear();
        nextChargeId_ = 1;
        for (uint8_t i = 0; i < kEnemyHelicopterCount; ++i) {
            hostHelicopters_[i] = EnemyHelicopterState{};
            remoteHelicopters_[i] = EnemyHelicopterState{};
        }
        hostEscapeBoat_ = {};
        remoteEscapeBoat_ = {};
        remoteVehicleTick_ = 0;
        hasHostHelicopters_ = false;
        hasRemoteHelicopters_ = false;
        hostArmor_ = {};
        remoteArmor_ = {};
        remoteArmorTick_ = 0;
        hasHostArmor_ = false;
        hasRemoteArmor_ = false;
        squadDeployPending_ = false;
        enemyFire_.clear();
        enemyHits_.clear();
        worldImpacts_.clear();
        worldBreaks_.clear();
        grenadeSpawns_.clear();
        grenadeDetonations_.clear();
        grenadeOwners_.clear();
        grenadeDetonationReports_.clear();
        reportedGrenadeDetonations_.clear();
        acceptedGrenadeTokens_.clear();
        receivedWorldImpacts_.clear();
        receivedGrenadeIds_.clear();
        receivedGrenadeDetonations_.clear();
        receivedWorldBreaks_.clear();
        terrainDeforms_.clear();
        terrainDeformEvents_.clear();
        requestedTerrainDeforms_.clear();
        receivedTerrainDeforms_.clear();
        barrelEvents_.clear();
        requestedBarrelEvents_.clear();
        requestedBlasts_.clear();
        nextGrenadeId_ = 1;
        nextImpactId_ = 1;
        nextDeformId_ = 1;
        levelKind_ = LevelKind::None;
        levelFile_.clear();
        hasPendingLevel_ = false;
        levelRestartPending_ = false;
        levelRestartSerial_ = 0;
        serverAddress_.clear();
        port_ = 0;
        overSteam_ = false;
        lastEnemyTick_ = 0;
        tick_ = 0;
        serverPeer_ = kInvalidPeer;
        disconnectPending_ = false;
        lastError_.clear();
        clock_.Reset();
    }

    // Why the session ended, when it ended on its own rather than by being
    // asked to. Taken rather than read: the caller shows it once, and a reason
    // left behind would resurface on the next panel that looks for one.
    std::string TakeLastError() {
        std::string reason;
        reason.swap(lastError_);
        return reason;
    }

    Role CurrentRole() const { return role_; }
    // Session god mode, which is the host's toggle. A client mirrors this onto
    // its own player; `known` stays false until the first snapshot lands.
    bool SessionGodMode(bool& known) const {
        known = role_ == Role::Host || sessionGodModeKnown_;
        return sessionGodMode_;
    }
    bool Active() const { return role_ != Role::Offline; }
    PlayerId LocalId() const { return localId_; }

    // Everyone in the session, the local player included. The menu needs this
    // before any body exists: a host sitting on the lobby screen has loaded no
    // level, so counting spawned actors would report nobody had joined.
    uint8_t PlayerCount() const {
        uint8_t count = 0;
        for (const PlayerSlot& slot : players_)
            if (slot.active) ++count;
        return count;
    }

    // True when `id` is a player the session is actually carrying. Lets the
    // menu list occupied slots without reaching into the snapshot path, which
    // interpolates and is meant for rendering bodies.
    bool PlayerActive(PlayerId id) const {
        return id < kMaxPlayers && players_[id].active;
    }

    // Called once per frame. Drains the transport every frame so connection
    // events are handled promptly, but only sends on a net tick.
    void Update(float deltaTime, const LocalPlayerState& local) {
        if (!Active() || !transport_) return;

        transport_->Poll(events_);
        for (Event& event : events_) HandleEvent(event);

        // Torn down here rather than inside HandleEvent: Shutdown releases the
        // transport the loop above is still walking the events of.
        if (disconnectPending_) {
            std::string reason;
            reason.swap(lastError_);
            Shutdown();
            lastError_.swap(reason);
            return;
        }

        // Remember the local player's own state so the host's snapshot
        // includes it and a client can be told where it thinks it is.
        if (localId_ != kInvalidPlayerId) {
            PlayerSlot& slot = players_[localId_];
            slot.active = true;
            slot.id = localId_;
            slot.current.x = local.x;
            slot.current.y = local.y;
            slot.current.z = local.z;
            slot.current.helicopter = local.helicopter;
            slot.current.vehicle = ValidDrivenVehicle(local.vehicle)
                ? local.vehicle : DrivenVehicleState{};
            slot.current.yaw = local.input.yaw;
            slot.current.pitch = local.input.pitch;
            slot.current.moving = local.input.Moving() ? 1 : 0;
            slot.current.crouching =
                local.input.Held(PlayerInput::Crouch) ? 1 : 0;
            slot.current.sprinting =
                local.input.Held(PlayerInput::Sprint) ? 1 : 0;
            slot.current.aiming =
                local.input.Held(PlayerInput::Aim) ? 1 : 0;
            // The host's own toggle is the session's god mode. A client's is
            // ignored here: it mirrors the host's from the snapshot instead.
            if (role_ == Role::Host) sessionGodMode_ = local.godMode;
            // Health and downed are NOT written from local state: the host owns
            // them for every player including itself, so they flow the other
            // way -- out through the snapshot, and back via LocalStatus.
            slot.current.health = slot.health;
            slot.current.downed = slot.downed ? 1 : 0;
            slot.current.reviver = slot.reviver;
        }

        clock_.Accumulate(deltaTime);
        float step = 0.0f;
        while (clock_.Consume(step)) {
            ++tick_;
            if (role_ == Role::Host) {
                // Both before the snapshot, so a revive or a heal completing
                // this tick is carried by the snapshot it completed on rather
                // than the next.
                StepRegen(step);
                StepRevives(step);
                SendSnapshot();
                SendEnemySnapshots();
                SendVehicleState();
                SendArmorState();
                SendScoreboard();
            } else {
                SendInput(local);
            }
        }
        interpolationTime_ += deltaTime;
    }

    // Every player except the local one, interpolated for rendering.
    void GetRemotePlayers(std::vector<RemotePlayer>& out) const {
        out.clear();
        for (uint8_t i = 0; i < kMaxPlayers; ++i) {
            if (!players_[i].active || i == localId_) continue;
            const PlayerSlot& slot = players_[i];
            RemotePlayer remote;
            remote.id = slot.id;
            // The render delay spans two ticks, so the last two packets alone
            // cannot bracket it. Sample retained history for both rider and
            // aircraft; otherwise the aircraft freezes between packet arrivals.
            const PlayerSnapshot* previous = &slot.previous;
            const PlayerSnapshot* current = &slot.current;
            float previousTime = slot.previousTime;
            float currentTime = slot.currentTime;
            for (size_t sample = 0; sample < slot.renderSampleCount; ++sample) {
                const auto& next = slot.renderSamples[sample];
                current = &next.state;
                currentTime = next.time;
                if (sample == 0) {
                    previous = current;
                    previousTime = currentTime;
                }
                if (currentTime >= RenderTime()) break;
                previous = current;
                previousTime = currentTime;
            }
            const float span = currentTime - previousTime;
            const float alpha = span > 1e-5f
                ? Clamp01((RenderTime() - previousTime) / span)
                : 1.0f;
            remote.x = Lerp(previous->x, current->x, alpha);
            remote.y = Lerp(previous->y, current->y, alpha);
            remote.z = Lerp(previous->z, current->z, alpha);
            remote.yaw = LerpAngle(previous->yaw, current->yaw, alpha);
            remote.pitch = Lerp(previous->pitch, current->pitch, alpha);
            remote.helicopter = slot.current.helicopter;
            remote.vehicle = slot.current.vehicle;
            const auto& previousHelicopter = previous->helicopter;
            const auto& currentHelicopter = current->helicopter;
            if (remote.helicopter.visible && previousHelicopter.visible &&
                currentHelicopter.visible &&
                remote.helicopter.airframe == previousHelicopter.airframe &&
                remote.helicopter.airframe == currentHelicopter.airframe) {
                remote.helicopter = currentHelicopter;
                auto& helicopter = remote.helicopter;
                helicopter.x = Lerp(previousHelicopter.x, helicopter.x, alpha);
                helicopter.y = Lerp(previousHelicopter.y, helicopter.y, alpha);
                helicopter.z = Lerp(previousHelicopter.z, helicopter.z, alpha);
                helicopter.yaw = LerpAngle(previousHelicopter.yaw, helicopter.yaw, alpha);
                helicopter.pitch = LerpAngle(previousHelicopter.pitch, helicopter.pitch, alpha);
                helicopter.roll = LerpAngle(previousHelicopter.roll, helicopter.roll, alpha);
            }
            remote.moving = slot.current.moving != 0;
            remote.crouching = slot.current.crouching != 0;
            remote.sprinting = slot.current.sprinting != 0;
            // Taken from the newest snapshot rather than interpolated: raising
            // a weapon is a state, not a position, and blending it would leave
            // a body half-sighted between packets.
            remote.aiming = slot.current.aiming != 0;
            remote.active = true;
            // Not interpolated: these are states, not positions, and a
            // half-downed player is not a thing.
            remote.health = slot.health;
            remote.downed = slot.downed;
            remote.reviver = slot.reviver;
            remote.reviveProgress = kReviveSeconds > 0.0f
                ? Clamp01(slot.reviveProgress / kReviveSeconds) : 0.0f;
            out.push_back(remote);
        }
    }

    // The input a remote player last sent. The host integrates movement from
    // this rather than trusting a position the client chose for itself, which
    // is the property that lets the same code become PvP-safe later.
    const PlayerInput* PendingInput(PlayerId id) const {
        if (id >= kMaxPlayers || !players_[id].active) return nullptr;
        return players_[id].hasInput ? &players_[id].lastInput : nullptr;
    }
    void ClearPendingInput(PlayerId id) {
        if (id < kMaxPlayers) players_[id].hasInput = false;
    }
    // Host-side: where the host decided a remote player ended up, fed back in
    // so the next snapshot carries it.
    void SetPlayerPosition(PlayerId id, float x, float y, float z) {
        if (id >= kMaxPlayers || !players_[id].active) return;
        PlayerSlot& slot = players_[id];
        slot.current.x = x;
        slot.current.y = y;
        slot.current.z = z;
    }

    uint32_t Tick() const { return tick_; }

    // ---- Level ---------------------------------------------------------
    //
    // The host's map is session state, not a per-player choice: two players on
    // different levels share coordinates, damage and enemies that mean nothing
    // to each other. The session carries the name; loading it is the game's job.
    void SetHostLevel(LevelKind kind, const std::string& file) {
        if (role_ != Role::Host) return;
        std::string trimmed = file.substr(0, kMaxLevelFileName - 1);
        if (kind == levelKind_ && trimmed == levelFile_) return;
        levelKind_ = kind;
        levelFile_ = std::move(trimmed);
        // The craters belonged to the map being left. Replaying them to the
        // next player who joins would cut the new level's ground at the old
        // one's coordinates.
        terrainDeforms_.clear();
        activeCharges_.clear();
        chargeSticks_.clear();
        chargeStickRequests_.clear();
        SendLevelTo(kInvalidPeer); // everyone
    }

    // Host-side: the squad restarts the level it is already on. Everyone is put
    // back on their feet at full health -- the slots are the authority, so a
    // local reset alone would be overwritten by the next status pull -- and
    // clients are told to reload even though the level name has not changed.
    void RestartHostLevel() {
        if (role_ != Role::Host) return;
        ++levelRestartSerial_;
        terrainDeforms_.clear();
        activeCharges_.clear();
        chargeSticks_.clear();
        chargeStickRequests_.clear();
        for (PlayerSlot& slot : players_) {
            if (!slot.active) continue;
            const bool wasDowned = slot.downed;
            slot.downed = false;
            slot.health = kMaxPlayerHealth;
            slot.reviver = kInvalidPlayerId;
            slot.reviveProgress = 0.0f;
            slot.downedTimer = 0.0f;
            if (!wasDowned) continue;
            PlayerStateChange change;
            change.id = slot.id;
            change.event = PlayerStateEvent::Revived;
            change.instigator = kInvalidPlayerId;
            change.health = kMaxPlayerHealth;
            PublishStateChange(change);
        }
        SendLevelTo(kInvalidPeer);
    }

    // Client-side: the pending level is a restart of the current one. Cleared
    // by the caller once the reload has actually started, so a request put
    // back behind a load in progress stays a restart.
    bool LevelRestartPending() const { return levelRestartPending_; }
    void ClearLevelRestart() { levelRestartPending_ = false; }

    LevelKind HostLevelKind() const { return levelKind_; }
    const std::string& HostLevelFile() const { return levelFile_; }

    // One-shot: returns true once per level the host names, so the caller can
    // run a load -- which takes seconds -- without being asked again while it
    // is still in progress.
    bool TakePendingLevel(LevelKind& kind, std::string& file) {
        if (!hasPendingLevel_) return false;
        hasPendingLevel_ = false;
        kind = levelKind_;
        file = levelFile_;
        return true;
    }

    // For a caller that took the level but could not act on it yet -- a load
    // already running, say. The level itself is still stored, so this only puts
    // the one-shot back.
    void RequeuePendingLevel() { hasPendingLevel_ = true; }

    // ---- World destruction and grenades -----------------------------
    //
    // These are event seams for the gameplay layer. NetSession never owns a
    // prefab, physics body, or Projectile; it only moves the authoritative
    // edges and leaves application of them to the caller.
    void ReportWorldImpact(uint64_t entityId, float damage,
                           float hitX, float hitY, float hitZ,
                           uint8_t kind = 1, float radius = 0.0f,
                           float impulse = 0.0f, uint32_t impactId = 0,
                           float dirX = 0.0f, float dirY = 0.0f,
                           float dirZ = 0.0f, bool playerOwned = true,
                           bool remoteCharge = false,
                           PlayerId shooter = kInvalidPlayerId) {
        const uint32_t assignedImpactId = impactId ? impactId : AllocateImpactId();
        if (!Active() || !ValidWorldTarget(entityId, kind, assignedImpactId) ||
            !(damage > 0.0f) ||
            !std::isfinite(damage) || !std::isfinite(hitX) ||
            !std::isfinite(hitY) || !std::isfinite(hitZ) ||
            !std::isfinite(radius) || !std::isfinite(impulse) ||
            !Finite3(dirX, dirY, dirZ) ||
            radius < 0.0f) return;
        if (role_ == Role::Host) {
            // `shooter` lets the host file an impact under another player:
            // it detonates everyone's charges, and the demolition is theirs.
            worldImpacts_.push_back({ assignedImpactId,
                                       kind,
                                       shooter != kInvalidPlayerId
                                           ? shooter : localId_,
                                       playerOwned,
                                       remoteCharge, entityId,
                                       damage,
                                       radius, impulse, dirX, dirY, dirZ,
                                       hitX, hitY, hitZ });
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientWorldImpactMessage message;
        message.impactId = assignedImpactId;
        message.kind = kind;
        message.playerOwned = playerOwned ? 1 : 0;
        message.remoteCharge = remoteCharge ? 1 : 0;
        message.entityId = entityId;
        message.damage = damage;
        message.radius = radius;
        message.impulse = impulse;
        message.dirX = dirX; message.dirY = dirY; message.dirZ = dirZ;
        message.hitX = hitX; message.hitY = hitY; message.hitZ = hitZ;
        message.shooterTick = tick_;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    void DrainWorldImpacts(std::vector<WorldImpactRequest>& out) {
        out.clear();
        out.swap(worldImpacts_);
    }

    // Host only, called after the gameplay layer has committed a break. The
    // host does not enqueue its own event: it already changed its world, and
    // replaying the edge locally would double-count score/effects.
    void PublishWorldBreak(uint64_t entityId, float hitX, float hitY,
                           float hitZ) {
        PublishWorldBreak(1, entityId, 0.0f, 0.0f, 0.0f,
                          hitX, hitY, hitZ, 0.0f, 0.0f, 0.0f,
                          kInvalidPlayerId);
    }

    // The id on the wire is minted here rather than carried over from the
    // request: every client allocates impact ids from its own counter, so two
    // peers routinely hand the host the same number, and forwarding it would
    // make one client's break dedupe away another's.
    void PublishWorldBreak(uint8_t kind,
                           uint64_t entityId, float damage, float radius,
                           float impulse, float hitX, float hitY,
                           float hitZ, float dirX = 0.0f,
                           float dirY = 0.0f, float dirZ = 0.0f,
                           PlayerId shooter = kInvalidPlayerId,
                           bool playerOwned = true,
                           bool remoteCharge = false) {
        const uint32_t canonicalImpactId = AllocateImpactId();
        if (role_ != Role::Host ||
            !ValidWorldTarget(entityId, kind, canonicalImpactId) ||
            !transport_) return;
        ServerWorldBreakMessage message;
        message.impactId = canonicalImpactId;
        message.kind = kind;
        message.shooter = shooter;
        message.playerOwned = playerOwned ? 1 : 0;
        message.remoteCharge = remoteCharge ? 1 : 0;
        message.entityId = entityId;
        message.damage = damage;
        message.radius = radius;
        message.impulse = impulse;
        message.dirX = dirX; message.dirY = dirY; message.dirZ = dirZ;
        message.hitX = hitX; message.hitY = hitY; message.hitZ = hitZ;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
    }

    void DrainWorldBreaks(std::vector<WorldBreakEvent>& out) {
        out.clear();
        out.swap(worldBreaks_);
    }

    // Returns a session-wide id for a host-created grenade. Zero is reserved
    // as "not networked" so existing offline projectiles remain unchanged.
    uint32_t AllocateGrenadeId() {
        if (nextGrenadeId_ == 0) ++nextGrenadeId_;
        return nextGrenadeId_++;
    }

    uint32_t AllocateImpactId() {
        if (nextImpactId_ == 0) ++nextImpactId_;
        return nextImpactId_++;
    }

    // A local player calls this immediately after creating its predicted throw.
    // The host receives it, allocates the canonical id, and broadcasts the
    // approved spawn. Host callers can use PublishGrenadeSpawn directly.
    void ReportGrenadeThrow(uint32_t clientToken, GrenadeKind kind,
                            float x, float y, float z,
                            float velocityX, float velocityY, float velocityZ,
                            float fuse) {
        if (!Active() || clientToken == 0 || !ValidGrenade(kind) ||
            !Finite3(x, y, z) || !Finite3(velocityX, velocityY, velocityZ) ||
            !std::isfinite(fuse) || fuse < 0.0f) return;
        if (role_ == Role::Host) {
            PublishGrenadeSpawn(clientToken, localId_, kind, x, y, z,
                                velocityX, velocityY, velocityZ, fuse);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientGrenadeThrowMessage message;
        message.clientToken = clientToken;
        message.kind = kind;
        message.x = x; message.y = y; message.z = z;
        message.velocityX = velocityX; message.velocityY = velocityY;
        message.velocityZ = velocityZ; message.fuse = fuse;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Host-only. Returns the assigned id so the caller can tag its own
    // authoritative Projectile and publish its later detonation by id.
    uint32_t PublishGrenadeSpawn(uint32_t clientToken, PlayerId owner,
                                 GrenadeKind kind, float x, float y, float z,
                                 float velocityX, float velocityY,
                                 float velocityZ, float fuse,
                                 bool hostile = false) {
        if (role_ != Role::Host || !transport_ || owner >= kMaxPlayers ||
            !ValidGrenade(kind) || !Finite3(x, y, z) ||
            !Finite3(velocityX, velocityY, velocityZ) ||
            !std::isfinite(fuse) || fuse < 0.0f) return 0;
        if (clientToken != 0) {
            const uint64_t tokenKey = (uint64_t(owner) << 32) | clientToken;
            if (!acceptedGrenadeTokens_.insert(tokenKey).second) return 0;
        }
        const uint32_t id = AllocateGrenadeId();
        GrenadeSpawnEvent event{ id, clientToken, owner, kind, hostile,
                                 x, y, z, velocityX, velocityY, velocityZ,
                                 fuse };
        // The host's game already owns the initiating projectile. Queue only
        // requests arriving from a client; host-created throws are represented
        // by their caller and must not spawn a duplicate.
        ServerGrenadeSpawnMessage message;
        message.grenadeId = id; message.clientToken = clientToken;
        message.owner = owner; message.kind = kind;
        message.hostile = hostile ? 1 : 0;
        message.x = x; message.y = y; message.z = z;
        message.velocityX = velocityX; message.velocityY = velocityY;
        message.velocityZ = velocityZ; message.fuse = fuse;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
        return id;
    }

    void QueueAuthoritativeGrenadeSpawn(const GrenadeSpawnEvent& event) {
        if (role_ != Role::Host ||
            (event.owner >= kMaxPlayers && event.owner != kInvalidPlayerId) ||
            event.grenadeId == 0 || !ValidGrenade(event.kind)) return;
        grenadeSpawns_.push_back(event);
    }

    // An AI thrower's grenade. It has no player behind it, so it carries
    // kInvalidPlayerId as its owner and is spawned on the host by the same
    // queue every machine reads, rather than by its caller: the enemy AI runs
    // on every machine, so a bandit that threw locally on each one produced a
    // different grenade per screen, landing in different places and wounding
    // different people. The host throws for everybody now.
    //
    // Returns the id, or 0 when there is no session to carry it.
    uint32_t SpawnAIGrenade(GrenadeKind kind, bool hostile,
                            float x, float y, float z,
                            float velocityX, float velocityY, float velocityZ,
                            float fuse) {
        if (role_ != Role::Host || !transport_ || !ValidGrenade(kind) ||
            !Finite3(x, y, z) ||
            !Finite3(velocityX, velocityY, velocityZ) ||
            !std::isfinite(fuse) || fuse < 0.0f) return 0;
        const uint32_t id = AllocateGrenadeId();
        GrenadeSpawnEvent event{ id, 0, kInvalidPlayerId, kind, hostile,
                                 x, y, z, velocityX, velocityY, velocityZ,
                                 fuse };
        grenadeSpawns_.push_back(event);
        ServerGrenadeSpawnMessage message;
        message.grenadeId = id;
        message.clientToken = 0;
        message.owner = kInvalidPlayerId;
        message.kind = kind;
        message.hostile = hostile ? 1 : 0;
        message.x = x; message.y = y; message.z = z;
        message.velocityX = velocityX; message.velocityY = velocityY;
        message.velocityZ = velocityZ; message.fuse = fuse;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
        return id;
    }

    void DrainGrenadeSpawns(std::vector<GrenadeSpawnEvent>& out) {
        out.clear(); out.swap(grenadeSpawns_);
    }

    // The thrower telling the host where its own grenade finished up. Sent once
    // per grenade, at the moment the client's copy would have gone off; the
    // client still waits for the host's detonation edge before anything
    // explodes on its screen.
    void ReportGrenadeDetonation(uint32_t grenadeId, float x, float y, float z) {
        if (role_ != Role::Client || !transport_ || grenadeId == 0 ||
            !Finite3(x, y, z)) return;
        ClientGrenadeDetonationMessage message;
        message.grenadeId = grenadeId;
        message.x = x; message.y = y; message.z = z;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Host side: where clients say their grenades ended up, for the game layer
    // to move the authoritative projectile to before it detonates it.
    void DrainGrenadeDetonationReports(
            std::vector<GrenadeDetonationEvent>& out) {
        out.clear(); out.swap(grenadeDetonationReports_);
    }

    void PublishGrenadeDetonation(uint32_t grenadeId, GrenadeKind kind,
                                  float x, float y, float z,
                                  bool hostile = false) {
        if (role_ != Role::Host || !transport_ || grenadeId == 0 ||
            !ValidGrenade(kind) || !Finite3(x, y, z)) return;
        ServerGrenadeDetonatedMessage message;
        message.grenadeId = grenadeId; message.kind = kind;
        message.hostile = hostile ? 1 : 0;
        message.x = x; message.y = y; message.z = z;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
    }

    void DrainGrenadeDetonations(std::vector<GrenadeDetonationEvent>& out) {
        out.clear(); out.swap(grenadeDetonations_);
    }

    // ---- Terrain ------------------------------------------------------
    //
    // The ground is host-owned outright. A client builds no craters of its own,
    // not even for its own grenade: two machines cutting from their own editor
    // tunables produced two different holes, which is the whole reason this
    // message exists. The cost is a round trip before your own blast digs in,
    // the same bargain the grenade lifecycle already makes.
    void PublishTerrainDeform(const TerrainDeform& deform) {
        if (role_ != Role::Host || !transport_) return;
        if (!ValidDeform(deform)) return;

        ServerTerrainDeformMessage message;
        message.deformId = nextDeformId_++;
        message.deform = deform;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);

        // Kept so a player joining later lands on the ground everyone else is
        // standing on. Trimmed from the front at the same capacity
        // RuntimeWorld::AddRuntimeTerrainStamp uses, so the backlog evicts in
        // step with the stamps it describes rather than replaying cuts the
        // host itself has already dropped.
        if (terrainDeforms_.size() >= kMaxReplicatedTerrainDeforms)
            terrainDeforms_.erase(terrainDeforms_.begin());
        terrainDeforms_.push_back({ message.deformId, deform });
    }

    void DrainTerrainDeforms(std::vector<TerrainDeformEvent>& out) {
        out.clear(); out.swap(terrainDeformEvents_);
    }

    // A client's own explosion. Its rocket, C4 and barrels never reach the host
    // as projectiles, so the cut they make is the only word the host gets that
    // anything happened to the ground. The client applies nothing here: what it
    // draws is the host's broadcast coming back.
    void RequestTerrainDeform(const TerrainDeform& deform) {
        if (role_ != Role::Client || !transport_) return;
        if (!ValidDeform(deform)) return;
        ClientTerrainDeformMessage message;
        message.deform = deform;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Cuts requested by clients, for the host's game layer to apply and then
    // publish. Kept out of PublishTerrainDeform so the session never decides on
    // its own to change the world.
    void DrainRequestedTerrainDeforms(std::vector<TerrainDeform>& out) {
        out.clear(); out.swap(requestedTerrainDeforms_);
    }

    // ---- Explosive barrels ------------------------------------------------
    //
    // Host-owned, like the ground. A client asks; the host decides and
    // broadcasts; every client plays the broadcast. See BarrelEventMessage.
    void ReportBarrelEvent(uint16_t barrel, BarrelEvent barrelEvent,
                           float x, float y, float z) {
        if (role_ != Role::Client || !transport_ ||
            serverPeer_ == kInvalidPeer || !Finite3(x, y, z)) return;
        BarrelEventMessage message;
        message.header.type = MessageType::ClientBarrelEvent;
        message.barrel = barrel;
        message.event = barrelEvent;
        message.x = x; message.y = y; message.z = z;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    void PublishBarrelEvent(uint16_t barrel, BarrelEvent barrelEvent,
                            float x, float y, float z) {
        if (role_ != Role::Host || !transport_ || !Finite3(x, y, z)) return;
        BarrelEventMessage message;
        message.header.type = MessageType::ServerBarrelEvent;
        message.barrel = barrel;
        message.event = barrelEvent;
        message.x = x; message.y = y; message.z = z;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
    }

    // Host-side: what clients asked for. Client-side: what the host decided.
    void DrainRequestedBarrelEvents(std::vector<BarrelEventRecord>& out) {
        out.clear(); out.swap(requestedBarrelEvents_);
    }
    void DrainBarrelEvents(std::vector<BarrelEventRecord>& out) {
        out.clear(); out.swap(barrelEvents_);
    }

    // Client-side: a rocket of this machine's went off. The host applies it to
    // its soldiers; nothing here touches anything locally.
    void ReportBlast(float x, float y, float z, float radius, float damage,
                     float push, float ragdollImpulse) {
        if (role_ != Role::Client || !transport_ ||
            serverPeer_ == kInvalidPeer || !Finite3(x, y, z)) return;
        ClientBlastMessage message;
        message.x = x; message.y = y; message.z = z;
        message.radius = radius;
        message.damage = damage;
        message.push = push;
        message.ragdollImpulse = ragdollImpulse;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    void DrainRequestedBlasts(std::vector<BlastRecord>& out) {
        out.clear(); out.swap(requestedBlasts_);
    }

    // ---- PvP ----------------------------------------------------------
    //
    // Hits are shooter-authoritative: whoever fired ran the geometry test
    // against the body it could see on its own screen and reports the result.
    // The host owns the arithmetic, so there is exactly one place damage is
    // applied -- this one -- whether the shot came from the host itself or
    // arrived in a packet.
    //
    // Deliberately unvalidated beyond sanity clamping. A modified client can
    // lie about damage; that is the accepted cost of a design that feels
    // responsive on a home connection with no lag compensation. The clamps
    // exist so a *bug* cannot destroy a session, not to stop a cheater.
    void ReportHit(PlayerId target, float damage, bool headshot,
                   float hitX, float hitY, float hitZ) {
        if (!Active()) return;
        if (role_ == Role::Host) {
            // No point packeting to ourselves.
            ApplyHitToPlayer(localId_, target, damage, headshot,
                             hitX, hitY, hitZ);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientHitReportMessage message;
        message.target = target;
        message.headshot = headshot ? 1 : 0;
        message.damage = damage;
        message.hitX = hitX;
        message.hitY = hitY;
        message.hitZ = hitZ;
        message.shooterTick = tick_;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Host-side damage application. Public so the host's own shots reach it
    // without a round trip, and so tests can drive it with no transport.
    void ApplyHitToPlayer(PlayerId shooter, PlayerId target, float damage,
                          bool headshot, float hitX, float hitY, float hitZ) {
        if (role_ != Role::Host) return;
        if (target >= kMaxPlayers || target == shooter) return;
        PlayerSlot& slot = players_[target];
        if (!slot.active || slot.downed) return;
        // Session god mode, checked here rather than at each call site because
        // this is the one door every source of player damage comes through --
        // a local shot, a client's hit report, a bandit, a fall. It is the
        // host's toggle, so while it is on nobody in the session takes damage.
        if (sessionGodMode_) return;
        // Clamp rather than trust. A negative would heal, and a NaN or a wild
        // value would put the slot somewhere no later arithmetic recovers from.
        if (!(damage >= 0.0f)) return;   // false for NaN, which is the point
        const float applied =
            headshot ? slot.health
                     : (damage < kMaxPlayerHealth ? damage : kMaxPlayerHealth);
        slot.health -= applied;
        if (slot.health < 0.0f) slot.health = 0.0f;
        // Re-armed on every hit, including the one that downs them, so being
        // picked up does not inherit a nearly expired timer and heal instantly.
        slot.regenTimer = kRegenDelaySeconds;
        if (slot.health > 0.0f) return;

        slot.downed = true;
        slot.downedTimer = 0.0f;
        slot.reviver = kInvalidPlayerId;
        slot.reviveProgress = 0.0f;
        slot.deaths++;
        slot.scoreboardDirty = true;
        PlayerStateChange change;
        change.id = target;
        change.event = PlayerStateEvent::Downed;
        change.instigator = shooter;
        change.health = 0.0f;
        change.impactX = hitX;
        change.impactY = hitY;
        change.impactZ = hitZ;
        PublishStateChange(change);
    }

    // Damage the local player took from something that is not another player --
    // a bandit, a fall, their own grenade. The client predicts it locally for
    // the flash and the chip bar; this is what makes the host agree.
    void ReportLocalDamage(float damage) {
        if (!Active() || localId_ == kInvalidPlayerId) return;
        if (!(damage > 0.0f)) return;
        if (role_ == Role::Host) {
            ApplyHitToPlayer(kInvalidPlayerId, localId_, damage, false,
                             0.0f, 0.0f, 0.0f);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientHitReportMessage message;
        // Target is ourselves: the host trusts a client about its own damage,
        // which it already does for position.
        message.target = localId_;
        message.damage = damage;
        message.shooterTick = tick_;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Who the local player is holding the revive key on this tick, or
    // kInvalidPlayerId for nobody. Sent every tick while held.
    void ReportReviveIntent(PlayerId target, bool holding) {
        if (!Active() || localId_ == kInvalidPlayerId) return;
        if (role_ == Role::Host) {
            players_[localId_].pendingReviveTarget = target;
            players_[localId_].pendingReviveHolding = holding;
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientReviveProgressMessage message;
        message.target = target;
        message.holding = holding ? 1 : 0;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Unreliable);
    }

    bool LocalStatus(LocalPlayerStatus& out) const {
        if (localId_ == kInvalidPlayerId || localId_ >= kMaxPlayers)
            return false;
        const PlayerSlot& slot = players_[localId_];
        if (!slot.active) return false;
        out.health = slot.health;
        out.downed = slot.downed;
        out.reviver = slot.reviver;
        out.reviveProgress = kReviveSeconds > 0.0f
            ? Clamp01(slot.reviveProgress / kReviveSeconds) : 0.0f;
        return true;
    }

    // Drained once per frame. Moves the queue out rather than copying so a
    // change cannot be handled twice.
    void DrainStateChanges(std::vector<PlayerStateChange>& out) {
        out.clear();
        out.swap(stateChanges_);
    }

    // Send a line the local player typed. The host stamps and broadcasts its
    // own immediately; a client hands it up and waits for it to come back, so
    // both ends see the same ordering rather than a local echo that jumps the
    // queue whenever the host is busy.
    void SendChat(const char* text) {
        if (!Active() || localId_ == kInvalidPlayerId || !text) return;
        // An empty or whitespace-only line is a stray Enter, not a message.
        bool printable = false;
        for (const char* c = text; *c; ++c)
            if (static_cast<unsigned char>(*c) > ' ') { printable = true; break; }
        if (!printable) return;

        if (role_ == Role::Host) {
            ServerChatMessage message;
            message.speaker = localId_;
            CopyChatText(message.text, text);
            transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
            // The host is not in its own broadcast, so it queues its own line
            // directly rather than waiting for a copy that never arrives.
            QueueChat(localId_, message.text);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientChatMessage message;
        CopyChatText(message.text, text);
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Drained once per frame like the state changes above, and for the same
    // reason: each line has to reach the log exactly once.
    void DrainChat(std::vector<ChatLine>& out) {
        out.clear();
        out.swap(chatLines_);
    }

    // Current standings, read every frame the scoreboard is open. Not drained:
    // the host sends only on change, so a drained copy would be gone the frame
    // after it arrived. The host builds it from its own slots.
    void GetScoreboard(std::vector<ScoreboardEntry>& out) const {
        out.clear();
        if (role_ != Role::Host) {
            out = scoreboard_;
            return;
        }
        for (const PlayerSlot& slot : players_) {
            if (!slot.active) continue;
            ScoreboardEntry entry;
            entry.id = slot.id;
            entry.kills = slot.kills;
            entry.deaths = slot.deaths;
            entry.revives = slot.revives;
            out.push_back(entry);
        }
    }

    void SendMedicCall() {
        if (!Active() || localId_ == kInvalidPlayerId) return;

        if (role_ == Role::Host) {
            ServerMedicCallMessage message;
            message.callerId = localId_;
            transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
            // Host queues its own call directly.
            medicCalls_.push_back(localId_);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientMedicCallMessage message;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    void DrainMedicCalls(std::vector<PlayerId>& out) {
        out.clear();
        out.swap(medicCalls_);
    }

    // Client-side: this player's transport set down with `count` marines
    // aboard. The host lands them; a client cannot run their AI.
    void SendMarineDrop(uint8_t count, float x, float y, float z) {
        if (role_ != Role::Client || !transport_ ||
            serverPeer_ == kInvalidPeer || count == 0 || !Finite3(x, y, z))
            return;
        ClientMarineDropMessage message;
        message.count = count;
        message.x = x; message.y = y; message.z = z;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Host-side: the client squads to land this frame, moved out so each is
    // landed exactly once.
    void DrainMarineDrops(std::vector<MarineDropRequest>& out) {
        out.clear();
        out.swap(marineDrops_);
    }

    // Host-only: track kills, deaths, and revives for scoreboard. Called when
    // stats change; marked dirty and broadcast on next SendScoreboard.
    void IncrementPlayerKill(PlayerId player) {
        if (role_ != Role::Host || player >= kMaxPlayers) return;
        PlayerSlot& slot = players_[player];
        if (!slot.active) return;
        slot.kills++;
        slot.scoreboardDirty = true;
    }

    void IncrementPlayerDeath(PlayerId player) {
        if (role_ != Role::Host || player >= kMaxPlayers) return;
        PlayerSlot& slot = players_[player];
        if (!slot.active) return;
        slot.deaths++;
        slot.scoreboardDirty = true;
    }

    void IncrementPlayerRevive(PlayerId player) {
        if (role_ != Role::Host || player >= kMaxPlayers) return;
        PlayerSlot& slot = players_[player];
        if (!slot.active) return;
        slot.revives++;
        slot.scoreboardDirty = true;
    }

    // Any player's life state, not just the local one. Used by the HUD to show
    // a teammate's health, and by the tests to assert on a player the harness
    // never has to impersonate over a socket.
    bool PlayerStatus(PlayerId id, LocalPlayerStatus& out) const {
        if (id >= kMaxPlayers || !players_[id].active) return false;
        const PlayerSlot& slot = players_[id];
        out.health = slot.health;
        out.downed = slot.downed;
        out.reviver = slot.reviver;
        out.reviveProgress = kReviveSeconds > 0.0f
            ? Clamp01(slot.reviveProgress / kReviveSeconds) : 0.0f;
        return true;
    }

    // ---- Enemies ------------------------------------------------------
    //
    // Enemies are host-authoritative outright: a client runs no enemy AI at
    // all. Two machines each simulating their own copy would diverge within
    // seconds -- the scatter seed alone is std::time(nullptr), so they would
    // not even start from the same layout -- and each player would be killing
    // a private set of bodies the other never saw fall.

    // Host-side, once per frame: hand the session the current state of every
    // AI actor. Stored rather than sent immediately, because sending happens on
    // the net tick and per-client culling needs all of them to choose from.
    void PublishEnemies(const std::vector<HostEnemyState>& enemies) {
        if (role_ != Role::Host) return;
        hostEnemies_ = enemies;
    }

    // Client-side: the enemies the host last told us about.
    const std::vector<RemoteEnemy>& RemoteEnemies() const {
        return remoteEnemies_;
    }

    // Host-side: the gunships as they stand this tick. Stored and sent with the
    // next snapshot rather than sent here, so the send stays on the net tick.
    void PublishVehicles(const EnemyHelicopterState* helicopters,
                         const EscapeBoatSnapshot& escapeBoat = {}) {
        if (role_ != Role::Host || !helicopters) return;
        for (uint8_t i = 0; i < kEnemyHelicopterCount; ++i)
            hostHelicopters_[i] = helicopters[i];
        hostEscapeBoat_ = escapeBoat;
        hasHostHelicopters_ = true;
    }

    // A round left this machine's muzzle. A client tells the host, the host
    // broadcasts; either way it reaches every other player and nobody else
    // re-derives it. The shooter never gets its own shot back -- it drew the
    // flash and played the sound when it pulled the trigger.
    void ReportShotFired(float x, float y, float z,
                         float dirX, float dirY, float dirZ) {
        if (!Active() || !transport_ || localId_ == kInvalidPlayerId ||
            !Finite3(x, y, z) || !Finite3(dirX, dirY, dirZ)) return;
        if (role_ == Role::Host) {
            ServerShotFiredMessage message;
            message.shooter = localId_;
            message.x = x; message.y = y; message.z = z;
            message.dirX = dirX; message.dirY = dirY; message.dirZ = dirZ;
            transport_->Broadcast(&message, sizeof(message),
                                  Channel::Unreliable);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientShotFiredMessage message;
        message.x = x; message.y = y; message.z = z;
        message.dirX = dirX; message.dirY = dirY; message.dirZ = dirZ;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Unreliable);
    }

    // The shots to present this frame, moved out so each is drawn once.
    void DrainRemoteShots(std::vector<RemoteShot>& out) {
        out.clear();
        out.swap(remoteShots_);
    }

    // A charge this machine's player just stuck to something. The host places
    // it and tells everyone; a client asks the host to. Either way the charge
    // appears on every machine from the one committed event, so nobody is
    // holding a charge the others cannot see.
    void ReportChargeStuck(const ChargeStickData& charge) {
        if (!Active() || !transport_ || localId_ == kInvalidPlayerId ||
            !ValidChargeStick(charge)) return;
        if (role_ == Role::Host) {
            CommitChargeStuck(localId_, charge);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientChargeStuckMessage message;
        message.charge = charge;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    void DrainChargeStickRequests(std::vector<ChargeStickRequest>& out) {
        out.clear();
        out.swap(chargeStickRequests_);
    }

    void CommitChargeStuck(PlayerId owner, const ChargeStickData& charge) {
        if (role_ != Role::Host || !transport_ || owner >= kMaxPlayers ||
            !players_[owner].active || !ValidChargeStick(charge)) return;
        ServerChargeStuckMessage message;
        message.owner = owner;
        message.chargeId = nextChargeId_++;
        if (message.chargeId == 0) message.chargeId = nextChargeId_++;
        message.charge = charge;
        if (activeCharges_.size() >= 12) activeCharges_.erase(activeCharges_.begin());
        activeCharges_.push_back(message);
        chargeSticks_.push_back({ owner, message.chargeId, charge });
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
    }

    // Keep the replay fallback at the host's last drawn pose. If the turret
    // disappears, late joiners receive a fixed charge at that pose.
    void UpdateActiveChargePose(uint32_t id, float x, float y, float z,
                                float nx, float ny, float nz,
                                float qx, float qy, float qz, float qw,
                                bool frozen) {
        if (role_ != Role::Host || id == 0) return;
        for (ServerChargeStuckMessage& stored : activeCharges_) {
            if (stored.chargeId != id) continue;
            stored.charge.x = x; stored.charge.y = y; stored.charge.z = z;
            stored.charge.nx = nx; stored.charge.ny = ny;
            stored.charge.nz = nz;
            stored.charge.qx = qx; stored.charge.qy = qy;
            stored.charge.qz = qz; stored.charge.qw = qw;
            if (frozen) stored.charge.frozen = 1;
            return;
        }
    }

    // This machine's player pressed the detonator.
    void ReportChargeDetonate() {
        if (!Active() || !transport_ || localId_ == kInvalidPlayerId) return;
        if (role_ == Role::Host) {
            RemoveActiveChargesFor(localId_);
            chargeDetonations_.push_back({ localId_ });
            ServerChargeDetonateMessage message;
            message.owner = localId_;
            transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientChargeDetonateMessage message;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    void DrainChargeSticks(std::vector<RemoteChargeStuck>& out) {
        out.clear();
        out.swap(chargeSticks_);
    }

    void DiscardPendingChargeSticksFor(PlayerId owner) {
        chargeSticks_.erase(std::remove_if(chargeSticks_.begin(),
            chargeSticks_.end(), [owner](const RemoteChargeStuck& charge) {
                return charge.owner == owner;
            }), chargeSticks_.end());
    }

    void DrainChargeDetonations(std::vector<RemoteChargeDetonate>& out) {
        out.clear();
        out.swap(chargeDetonations_);
    }

    // Client-side: the gunship state the host last sent, or nullptr before the
    // first one arrives -- which is the signal to leave the local craft alone
    // rather than snapping it to an all-zero pose at the origin.
    const EscapeBoatSnapshot* RemoteEscapeBoat() const {
        return hasRemoteHelicopters_ ? &remoteEscapeBoat_ : nullptr;
    }

    const EnemyHelicopterState* RemoteVehicles() const {
        return hasRemoteHelicopters_ ? remoteHelicopters_ : nullptr;
    }

    // Host-side: every tank and AA gun as it stands this tick. Stored and sent
    // on the net tick, like the gunships. Counts past the wire caps are cut.
    void PublishArmor(const EnemyTankSnapshot* tanks, size_t tankCount,
                      const AATurretSnapshot* turrets, size_t turretCount,
                      const EnemyHumveeSnapshot* humvees, size_t humveeCount,
                      const ObjectivePlaneSnapshot* planes = nullptr,
                      size_t planeCount = 0) {
        if (role_ != Role::Host) return;
        hostArmor_ = ServerArmorStateMessage{};
        hostArmor_.tankCount = static_cast<uint8_t>(
            tankCount < kMaxReplicatedTanks ? tankCount : kMaxReplicatedTanks);
        hostArmor_.turretCount = static_cast<uint8_t>(
            turretCount < kMaxReplicatedAATurrets ? turretCount
                                                  : kMaxReplicatedAATurrets);
        hostArmor_.humveeCount = static_cast<uint8_t>(
            humveeCount < kMaxReplicatedHumvees ? humveeCount
                                                : kMaxReplicatedHumvees);
        for (uint8_t i = 0; i < hostArmor_.tankCount; ++i)
            hostArmor_.tanks[i] = tanks[i];
        for (uint8_t i = 0; i < hostArmor_.turretCount; ++i)
            hostArmor_.turrets[i] = turrets[i];
        for (uint8_t i = 0; i < hostArmor_.humveeCount; ++i)
            hostArmor_.humvees[i] = humvees[i];
        hostArmor_.planeCount = static_cast<uint8_t>(
            planeCount < kMaxReplicatedPlanes ? planeCount
                                              : kMaxReplicatedPlanes);
        for (uint8_t i = 0; i < hostArmor_.planeCount; ++i)
            hostArmor_.planes[i] = planes[i];
        hasHostArmor_ = true;
    }

    // Client-side: the armor the host last described, or nullptr before the
    // first message -- the signal to leave the local tanks and guns alone.
    const ServerArmorStateMessage* RemoteArmor() const {
        return hasRemoteArmor_ ? &remoteArmor_ : nullptr;
    }

    // Host-side: a tank or AA gun fired. Sent straight away rather than on the
    // tick, so the round leaves every client's muzzle as close as possible to
    // when it left the host's. The host does not queue its own: it spawned
    // the projectile when it fired.
    void PublishEnemyFire(EnemyFireKind kind, float x, float y, float z,
                          float dirX, float dirY, float dirZ,
                          float speed = 0.0f, float lifetime = 0.0f,
                          float damageScale = 1.0f,
                          InfantryWeapon weapon = InfantryWeapon::Rifle) {
        if (role_ != Role::Host || !transport_ || !Finite3(x, y, z) ||
            !Finite3(dirX, dirY, dirZ) || !std::isfinite(speed) ||
            !std::isfinite(lifetime) || !std::isfinite(damageScale)) return;
        ServerEnemyFireMessage message;
        message.kind = kind;
        message.weapon = weapon;
        message.x = x; message.y = y; message.z = z;
        message.dirX = dirX; message.dirY = dirY; message.dirZ = dirZ;
        message.speed = speed;
        message.lifetime = lifetime;
        message.damageScale = damageScale;
        // A sniper round is rare and nearly lethal, like a tank shell; rifle
        // and shotgun fire is a stream where a resent round arrives late.
        const bool reliable = kind == EnemyFireKind::TankShell ||
            (kind == EnemyFireKind::InfantryShot &&
             weapon == InfantryWeapon::Sniper);
        transport_->Broadcast(&message, sizeof(message),
                              reliable ? Channel::Reliable : Channel::Unreliable);
    }

    // Host-side: DEPLOY SQUAD. Reliable -- a lost order leaves a player on the
    // planning map while the rest of the squad flies off without them.
    void PublishSquadDeploy(uint8_t insertionMode, uint8_t airframe,
                            bool hostLeftSeat, float x, float y, float z) {
        if (role_ != Role::Host || !transport_ || !Finite3(x, y, z)) return;
        ServerSquadDeployMessage message;
        message.insertionMode = insertionMode;
        message.airframe = airframe;
        message.hostLeftSeat = hostLeftSeat ? 1 : 0;
        message.x = x; message.y = y; message.z = z;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
    }

    // Client-side: the squad order, once. False when none is waiting.
    bool TakeSquadDeploy(SquadDeployOrder& out) {
        if (!squadDeployPending_) return false;
        squadDeployPending_ = false;
        out = squadDeploy_;
        return true;
    }

    // Client-side: the host's rounds to spawn this frame, moved out so each is
    // spawned exactly once.
    void DrainEnemyFire(std::vector<RemoteEnemyFire>& out) {
        out.clear();
        out.swap(enemyFire_);
    }

    // A round from this machine hit an enemy. On a client this reports to the
    // host; on the host it queues into the same list the reports land in, so
    // both take one identical path into the damage code.
    void ReportEnemyHit(EnemyId target, float damage, bool headshot,
                        float dirX, float dirY, float dirZ,
                        float hitX, float hitY, float hitZ) {
        if (!Active() || target == kInvalidEnemyId) return;
        if (role_ == Role::Host) {
            EnemyHitRequest request;
            request.target = target;
            request.shooter = localId_;
            request.damage = damage;
            request.headshot = headshot;
            request.dirX = dirX; request.dirY = dirY; request.dirZ = dirZ;
            request.hitX = hitX; request.hitY = hitY; request.hitZ = hitZ;
            enemyHits_.push_back(request);
            return;
        }
        if (serverPeer_ == kInvalidPeer) return;
        ClientEnemyHitReportMessage message;
        message.target = target;
        message.headshot = headshot ? 1 : 0;
        message.damage = damage;
        message.dirX = dirX; message.dirY = dirY; message.dirZ = dirZ;
        message.hitX = hitX; message.hitY = hitY; message.hitZ = hitZ;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Reliable);
    }

    // Host-side: the hits to apply this frame, moved out so each is applied
    // exactly once.
    void DrainEnemyHits(std::vector<EnemyHitRequest>& out) {
        out.clear();
        out.swap(enemyHits_);
    }

    // Seats a player in a slot without a handshake. The host uses this for
    // itself; tests use it to stand up a second player with no socket. Not a
    // way to join a real session -- a client still has to complete the
    // handshake to be given an id it can send under.
    bool ActivatePlayerSlot(PlayerId id) {
        if (id >= kMaxPlayers || players_[id].active) return false;
        players_[id] = PlayerSlot{};
        players_[id].id = id;
        players_[id].active = true;
        return true;
    }

private:
    static bool ValidChargeStick(const ChargeStickData& charge) {
        if (!Finite3(charge.x, charge.y, charge.z) ||
            !Finite3(charge.nx, charge.ny, charge.nz) ||
            !Finite3(charge.qx, charge.qy, charge.qz) ||
            !std::isfinite(charge.qw) || charge.frozen > 1 ||
            !SGE::ValidChargeAnchorPart(charge.part)) return false;
        if (charge.part == SGE::ChargeAnchorPart::World) return true;
        if (!Finite3(charge.turretX, charge.turretY, charge.turretZ) ||
            !Finite3(charge.localX, charge.localY, charge.localZ) ||
            !Finite3(charge.localNx, charge.localNy, charge.localNz)) return false;
        const float dx = charge.x - charge.turretX;
        const float dy = charge.y - charge.turretY;
        const float dz = charge.z - charge.turretZ;
        const float offsetSq = charge.localX * charge.localX +
                               charge.localY * charge.localY +
                               charge.localZ * charge.localZ;
        const float normalSq = charge.localNx * charge.localNx +
                               charge.localNy * charge.localNy +
                               charge.localNz * charge.localNz;
        return dx * dx + dy * dy + dz * dz <= 7.0f * 7.0f &&
               offsetSq <= 6.0f * 6.0f && normalSq > 0.25f &&
               normalSq < 2.25f;
    }

    void RemoveActiveChargesFor(PlayerId owner) {
        activeCharges_.erase(std::remove_if(activeCharges_.begin(),
            activeCharges_.end(), [owner](const ServerChargeStuckMessage& charge) {
                return charge.owner == owner;
            }), activeCharges_.end());
    }

    static bool ValidInsertionHelicopter(const InsertionHelicopterState& h) {
        return h.visible <= 1 && h.airframe < 2 &&
            Finite3(h.x, h.y, h.z) && Finite3(h.yaw, h.pitch, h.roll) &&
            Finite3(h.centerX, h.minY, h.centerZ) &&
            std::isfinite(h.scale) && h.scale > 0.0f && h.scale <= 1000.0f;
    }
    static bool Finite3(float x, float y, float z) {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    // The pose goes straight into a physics body on the host, so a NaN or a
    // zero quaternion here would not fail loudly -- it would fling the chassis.
    static bool ValidDrivenVehicle(const DrivenVehicleState& v) {
        if (v.kind == DrivenVehicleKind::None) return true;
        if (v.kind == DrivenVehicleKind::Humvee) {
            if (v.index >= kMaxReplicatedHumvees) return false;
        } else if (v.kind == DrivenVehicleKind::Tank) {
            if (v.index >= kMaxReplicatedTanks) return false;
        } else {
            return false;
        }
        if (!Finite3(v.x, v.y, v.z) || !Finite3(v.qx, v.qy, v.qz) ||
            !std::isfinite(v.qw) || !std::isfinite(v.turretYaw)) return false;
        const float lengthSq = v.qx * v.qx + v.qy * v.qy + v.qz * v.qz +
                               v.qw * v.qw;
        return lengthSq > 0.25f && lengthSq < 4.0f;
    }
    static bool ValidGrenade(GrenadeKind kind) {
        return kind == GrenadeKind::Frag || kind == GrenadeKind::Molotov ||
               kind == GrenadeKind::Vortex;
    }
    // These numbers drive a height field and a collision rebuild, so a NaN or a
    // wild radius does not fail loudly -- it corrupts the ground. The ceilings
    // are far above anything the game makes: the widest legitimate cut is
    // explosionBlastRadius 3.5 x craterScale 3 x missileBlastScale 2, about 21
    // metres. As with hit reporting, the clamps are here so a bug cannot wreck
    // a session, not to stop someone determined to cheat.
    static constexpr float kMaxDeformRadius = 64.0f;
    static constexpr float kMaxDeformDepth = 32.0f;
    static bool ValidDeform(const TerrainDeform& deform) {
        return Finite3(deform.x, deform.z, deform.radius) &&
               Finite3(deform.value, deform.strength, deform.edgeFalloff) &&
               std::isfinite(deform.baseHeight) &&
               std::isfinite(deform.impactY) &&
               deform.radius > 0.0f &&
               deform.radius <= kMaxDeformRadius &&
               std::fabs(deform.value) <= kMaxDeformDepth;
    }
    // 3 is an enemy gunship, with the airframe index in entityId. It rides the
    // world-impact channel rather than a message of its own: a round on a hull
    // is the same shooter-authoritative bargain as a round on a wall, and the
    // dedupe and validation here are exactly what it needs.
    static bool ValidWorldKind(uint8_t kind) { return kind <= 5; }
    static bool ValidWorldTarget(uint64_t entityId, uint8_t kind,
                                 uint32_t impactId) {
        // Destruction surfaces, trees and gunships have no prefab entity id.
        // Their per-impact id is the stable key used for reliable dedupe.
        // A gunship's entityId is an airframe index, so it must name one.
        if (kind == 3 && entityId >= kEnemyHelicopterCount) return false;
        // A tank is named by its level entity; there is no tank zero.
        if (kind == 4 && entityId == 0) return false;
        // An AA turret's entityId is the sender's index, only a hint -- the
        // host finds the gun by the reported base position.
        if (kind == 5 && entityId >= kMaxReplicatedAATurrets) return false;
        return ValidWorldKind(kind) &&
               (entityId != 0 || (kind != 1 && impactId != 0));
    }

    struct PlayerSlot {
        struct RenderSample {
            PlayerSnapshot state;
            float time = 0.0f;
        };
        std::array<RenderSample, 8> renderSamples{};
        size_t renderSampleCount = 0;
        PlayerId id = kInvalidPlayerId;
        bool active = false;
        PeerId peer = kInvalidPeer;
        PlayerSnapshot current;
        PlayerSnapshot previous;
        float currentTime = 0.0f;
        float previousTime = 0.0f;
        PlayerInput lastInput;
        bool hasInput = false;
        // Life state. The host owns every one of these; a client writes them
        // only from an inbound snapshot, never from its own gameplay.
        float health = kMaxPlayerHealth;
        bool downed = false;
        // How long they have been down. Unused today -- carried so a bleedout
        // timer is a tuning change rather than a protocol change.
        float downedTimer = 0.0f;
        PlayerId reviver = kInvalidPlayerId;
        float reviveProgress = 0.0f;
        // Set while a client is reporting a held revive key this tick. Cleared
        // every tick by the host, which re-derives progress from scratch rather
        // than trusting the flag to stop arriving -- a client that disconnects
        // mid-hold must not revive anyone.
        PlayerId pendingReviveTarget = kInvalidPlayerId;
        bool pendingReviveHolding = false;
        // Counts down after the last hit; regen starts when it reaches zero.
        float regenTimer = 0.0f;
        // Throttles the "revive denied" diagnostic to roughly one line a second
        // so a 30 Hz rejection does not bury the log it is meant to explain.
        float diagnosticTimer = 0.0f;
        // Scoreboard stats. Host-only and replicated on change.
        uint32_t kills = 0;
        uint32_t deaths = 0;  // times downed
        uint32_t revives = 0;
        bool scoreboardDirty = false;  // triggers broadcast on next SendScoreboard
    };

    static float Clamp01(float value) {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }
    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    // Yaw wraps, so interpolating 359 -> 1 the short way matters or the body
    // spins most of a turn backwards on every wrap.
    static float LerpAngle(float a, float b, float t) {
        float delta = b - a;
        while (delta > 180.0f) delta -= 360.0f;
        while (delta < -180.0f) delta += 360.0f;
        return a + delta * t;
    }
    float RenderTime() const {
        return interpolationTime_ - kInterpolationDelay;
    }

    // Queues a transition locally and, on the host, tells everyone else. The
    // queue is what the game drains; the message is what makes a client's queue
    // fill too.
    void PublishStateChange(const PlayerStateChange& change) {
        stateChanges_.push_back(change);
        if (role_ != Role::Host || !transport_) return;
        ServerPlayerStateChangedMessage message;
        message.id = change.id;
        message.event = change.event;
        message.instigator = change.instigator;
        message.health = change.health;
        message.impulseX = change.impulseX;
        message.impulseY = change.impulseY;
        message.impulseZ = change.impulseZ;
        message.impactX = change.impactX;
        message.impactY = change.impactY;
        message.impactZ = change.impactZ;
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
        SGE_LOG("LogNet", EngineLog::Level::Display,
            std::string("player ") + std::to_string(change.id) +
            (change.event == PlayerStateEvent::Downed ? " downed by "
                                                      : " revived by ") +
            std::to_string(change.instigator));
    }

    // Host-only, once per net tick. Out-of-combat regeneration for every
    // player, run here rather than on each client: the host's copy is
    // authoritative and overwrites theirs every frame, so a client healing
    // itself would be undone a tick later and read as regen being broken.
    //
    // A downed player does not regenerate -- that is what being downed means,
    // and healing them back up would make the revive pointless.
    void StepRegen(float step) {
        for (PlayerSlot& slot : players_) {
            if (!slot.active || slot.downed) continue;
            if (slot.health <= 0.0f || slot.health >= kMaxPlayerHealth) continue;
            slot.regenTimer -= step;
            if (slot.regenTimer > 0.0f) continue;
            slot.regenTimer = 0.0f;
            slot.health += kRegenPerSecond * step;
            if (slot.health > kMaxPlayerHealth) slot.health = kMaxPlayerHealth;
        }
    }

    // Host-only, once per net tick. Recomputes every downed player's revive
    // progress from scratch rather than trusting the holding flags to stop
    // arriving: a client that drops mid-hold leaves its last flag set forever,
    // and distance is re-checked here against the host's own positions.
    void StepRevives(float step) {
        for (uint8_t target = 0; target < kMaxPlayers; ++target) {
            PlayerSlot& slot = players_[target];
            if (!slot.active) continue;
            if (!slot.downed) {
                slot.reviver = kInvalidPlayerId;
                slot.reviveProgress = 0.0f;
                continue;
            }
            slot.downedTimer += step;

            // Nearest valid reviver wins, so two players crowding one body
            // cannot both be credited and the progress cannot double-rate.
            PlayerId best = kInvalidPlayerId;
            float bestDistanceSquared = kReviveRadius * kReviveRadius;
            for (uint8_t candidate = 0; candidate < kMaxPlayers; ++candidate) {
                if (candidate == target) continue;   // no self-revive
                const PlayerSlot& other = players_[candidate];
                if (!other.active || other.downed) continue;
                if (!other.pendingReviveHolding) continue;
                if (other.pendingReviveTarget != target) continue;
                // Compared in the plane, which is the same rule the client's
                // prompt uses (NearbyDownedPlayer). Including Y here instead
                // made the two disagree on sloped terrain: the prompt appeared
                // and the intent was sent, but this test rejected it and the
                // bar never filled. Over kReviveRadius of ground a slope can
                // easily separate two sets of feet by more than the radius
                // itself, and "standing next to them" is an XZ question.
                const float dx = other.current.x - slot.current.x;
                const float dz = other.current.z - slot.current.z;
                const float distanceSquared = dx * dx + dz * dz;
                if (distanceSquared > bestDistanceSquared) continue;
                bestDistanceSquared = distanceSquared;
                best = candidate;
            }

            if (best == kInvalidPlayerId) {
                // Why nobody was credited, about once a second while someone is
                // on the floor. A revive that silently does nothing has three
                // very different causes -- the intent never reached the host,
                // it named someone else, or it arrived and lost the range test
                // -- and they are indistinguishable from inside the game. The
                // numbers separate them; guessing between them does not.
                slot.diagnosticTimer += step;
                if (slot.diagnosticTimer >= 1.0f) {
                    slot.diagnosticTimer = 0.0f;
                    std::string line = "revive denied for player " +
                        std::to_string(static_cast<int>(target)) + " at (" +
                        std::to_string(slot.current.x) + "," +
                        std::to_string(slot.current.z) + ")";
                    bool anyHolder = false;
                    for (uint8_t c = 0; c < kMaxPlayers; ++c) {
                        if (c == target) continue;
                        const PlayerSlot& other = players_[c];
                        if (!other.active) continue;
                        if (!other.pendingReviveHolding) continue;
                        anyHolder = true;
                        const float dx = other.current.x - slot.current.x;
                        const float dz = other.current.z - slot.current.z;
                        line += "; player " + std::to_string(static_cast<int>(c)) +
                            " holding on " +
                            std::to_string(static_cast<int>(
                                other.pendingReviveTarget)) +
                            " at (" + std::to_string(other.current.x) + "," +
                            std::to_string(other.current.z) + ") distance " +
                            std::to_string(std::sqrt(dx * dx + dz * dz)) +
                            " vs radius " + std::to_string(kReviveRadius) +
                            (other.downed ? " [holder is downed]" : "");
                    }
                    if (!anyHolder) line += "; no revive intent reached the host";
                    SGE_LOG("LogNet", EngineLog::Level::Display, line);
                }
                // Decay rather than reset: stepping out of range for one tick
                // of jitter should not throw away three seconds of work.
                slot.reviver = kInvalidPlayerId;
                slot.reviveProgress -= step;
                if (slot.reviveProgress < 0.0f) slot.reviveProgress = 0.0f;
                continue;
            }
            slot.diagnosticTimer = 0.0f;

            slot.reviver = best;
            slot.reviveProgress += step;
            if (slot.reviveProgress < kReviveSeconds) continue;

            slot.downed = false;
            slot.health = kReviveHealth;
            slot.reviveProgress = 0.0f;
            slot.downedTimer = 0.0f;
            const PlayerId reviver = slot.reviver;
            slot.reviver = kInvalidPlayerId;
            if (reviver < kMaxPlayers && players_[reviver].active) {
                players_[reviver].revives++;
                players_[reviver].scoreboardDirty = true;
            }
            PlayerStateChange change;
            change.id = target;
            change.event = PlayerStateEvent::Revived;
            change.instigator = reviver;
            change.health = kReviveHealth;
            PublishStateChange(change);
        }
    }

    void HandleEvent(Event& event) {
        switch (event.type) {
        case EventType::Connected:
            SGE_LOG("LogNet", EngineLog::Level::Display,
                "peer " + std::to_string(event.peer) + " connected");
            // The host waits for a hello before assigning a player id; a
            // client sends one as soon as the connection is up.
            if (role_ == Role::Client) {
                ClientHello hello;
                transport_->Send(event.peer, &hello, sizeof(hello),
                                 Channel::Reliable);
                serverPeer_ = event.peer;
                SGE_LOG("LogNet", EngineLog::Level::Display, "sent hello");
            }
            break;
        case EventType::Disconnected:
            SGE_LOG("LogNet", EngineLog::Level::Display,
                "peer " + std::to_string(event.peer) + " disconnected");
            // A client has exactly one peer worth losing, and ReleasePeer
            // below is host-side bookkeeping: peerToPlayer_ is only ever
            // filled by AllocatePlayer, so on a client it does nothing. That
            // left a refused or dropped connection sitting in Role::Client
            // with no id -- which the menu renders as "Connecting..." forever.
            //
            // serverPeer_ is only set once the handshake starts, so a failure
            // during the connect itself has nothing to compare against.
            if (role_ == Role::Client &&
                (serverPeer_ == kInvalidPeer || event.peer == serverPeer_)) {
                lastError_ = localId_ == kInvalidPlayerId
                                 ? "Could not reach the host."
                                 : "Lost the connection to the host.";
                disconnectPending_ = true;
            }
            ReleasePeer(event.peer);
            break;
        case EventType::Message:
            HandleMessage(event);
            break;
        }
    }

    void HandleMessage(Event& event) {
        if (event.payload.size() < sizeof(MessageHeader)) return;
        MessageHeader header{};
        std::memcpy(&header, event.payload.data(), sizeof(header));
        switch (header.type) {
        case MessageType::ClientHello:
            if (role_ == Role::Host) HandleHello(event);
            break;
        case MessageType::ServerWelcome:
            if (role_ == Role::Client) HandleWelcome(event);
            break;
        case MessageType::ClientInput:
            if (role_ == Role::Host) HandleInput(event);
            break;
        case MessageType::ServerSnapshot:
            if (role_ == Role::Client) HandleSnapshot(event);
            break;
        case MessageType::ClientHitReport:
            if (role_ == Role::Host) HandleHitReport(event);
            break;
        case MessageType::ClientReviveProgress:
            if (role_ == Role::Host) HandleReviveProgress(event);
            break;
        case MessageType::ServerPlayerStateChanged:
            if (role_ == Role::Client) HandlePlayerStateChanged(event);
            break;
        case MessageType::ServerEnemySnapshot:
            if (role_ == Role::Client) HandleEnemySnapshot(event);
            break;
        case MessageType::ServerVehicleState:
            if (role_ == Role::Client) HandleVehicleState(event);
            break;
        case MessageType::ServerArmorState:
            if (role_ == Role::Client) HandleArmorState(event);
            break;
        case MessageType::ServerEnemyFire:
            if (role_ == Role::Client) HandleEnemyFire(event);
            break;
        case MessageType::ServerSquadDeploy:
            if (role_ == Role::Client) HandleSquadDeploy(event);
            break;
        case MessageType::ClientChargeStuck:
            if (role_ == Role::Host) HandleClientChargeStuck(event);
            break;
        case MessageType::ServerChargeStuck:
            if (role_ == Role::Client) HandleServerChargeStuck(event);
            break;
        case MessageType::ClientChargeDetonate:
            if (role_ == Role::Host) HandleClientChargeDetonate(event);
            break;
        case MessageType::ServerChargeDetonate:
            if (role_ == Role::Client) HandleServerChargeDetonate(event);
            break;
        case MessageType::ClientChatMessage:
            if (role_ == Role::Host) HandleClientChat(event);
            break;
        case MessageType::ServerChatMessage:
            if (role_ == Role::Client) HandleServerChat(event);
            break;
        case MessageType::ClientShotFired:
            if (role_ == Role::Host) HandleClientShotFired(event);
            break;
        case MessageType::ServerShotFired:
            if (role_ == Role::Client) HandleServerShotFired(event);
            break;
        case MessageType::ClientEnemyHitReport:
            if (role_ == Role::Host) HandleEnemyHitReport(event);
            break;
        case MessageType::ClientWorldImpact:
            if (role_ == Role::Host) HandleWorldImpact(event);
            break;
        case MessageType::ServerWorldBreak:
            if (role_ == Role::Client) HandleWorldBreak(event);
            break;
        case MessageType::ClientGrenadeThrow:
            if (role_ == Role::Host) HandleGrenadeThrow(event);
            break;
        case MessageType::ServerGrenadeSpawn:
            if (role_ == Role::Client) HandleGrenadeSpawn(event);
            break;
        case MessageType::ServerGrenadeDetonated:
            if (role_ == Role::Client) HandleGrenadeDetonated(event);
            break;
        case MessageType::ServerLevel:
            if (role_ == Role::Client) HandleLevel(event);
            break;
        case MessageType::ServerTerrainDeform:
            if (role_ == Role::Client) HandleTerrainDeform(event);
            break;
        case MessageType::ClientTerrainDeform:
            if (role_ == Role::Host) HandleTerrainDeformRequest(event);
            break;
        case MessageType::ClientGrenadeDetonation:
            if (role_ == Role::Host) HandleGrenadeDetonationReport(event);
            break;
        case MessageType::ClientBarrelEvent:
            if (role_ == Role::Host) HandleBarrelEvent(event, /*fromServer=*/false);
            break;
        case MessageType::ServerBarrelEvent:
            if (role_ == Role::Client) HandleBarrelEvent(event, /*fromServer=*/true);
            break;
        case MessageType::ClientBlast:
            if (role_ == Role::Host) HandleClientBlast(event);
            break;
        case MessageType::ClientMedicCall:
            if (role_ == Role::Host) HandleClientMedicCall(event);
            break;
        case MessageType::ServerMedicCall:
            if (role_ == Role::Client) HandleServerMedicCall(event);
            break;
        case MessageType::ClientMarineDrop:
            if (role_ == Role::Host) HandleClientMarineDrop(event);
            break;
        case MessageType::ServerScoreboard:
            if (role_ == Role::Client) HandleServerScoreboard(event);
            break;
        default:
            break;
        }
    }

    void HandleBarrelEvent(Event& event, bool fromServer) {
        if (event.payload.size() < sizeof(BarrelEventMessage)) return;
        if (fromServer) {
            if (event.peer != serverPeer_) return;
        } else if (peerToPlayer_.find(event.peer) == peerToPlayer_.end()) {
            return;
        }
        BarrelEventMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.event != BarrelEvent::Ignite &&
            message.event != BarrelEvent::Detonate) return;
        if (!Finite3(message.x, message.y, message.z)) return;
        // Bounded like the enemy-fire queue: a machine sitting on a loading
        // screen is not draining, and a chain of a whole yard is a few dozen.
        std::vector<BarrelEventRecord>& queue =
            fromServer ? barrelEvents_ : requestedBarrelEvents_;
        if (queue.size() >= 256) return;
        queue.push_back({ message.barrel, message.event,
                          message.x, message.y, message.z });
    }

    void HandleClientBlast(Event& event) {
        if (event.payload.size() < sizeof(ClientBlastMessage)) return;
        if (peerToPlayer_.find(event.peer) == peerToPlayer_.end()) return;
        ClientBlastMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (!Finite3(message.x, message.y, message.z) ||
            !Finite3(message.radius, message.damage, message.push) ||
            !std::isfinite(message.ragdollImpulse)) return;
        // Clamped rather than trusted, for the same reason player damage is:
        // so a bug cannot wipe the map, not to stop a cheater. The ceilings sit
        // well above a missile-scaled rocket.
        BlastRecord blast;
        blast.x = message.x; blast.y = message.y; blast.z = message.z;
        blast.radius = (std::max)(0.0f, (std::min)(message.radius, 40.0f));
        blast.damage = (std::max)(0.0f, (std::min)(message.damage, 2000.0f));
        blast.push = (std::max)(0.0f, (std::min)(message.push, 60.0f));
        blast.ragdollImpulse =
            (std::max)(0.0f, (std::min)(message.ragdollImpulse, 400.0f));
        if (blast.radius <= 0.0f || requestedBlasts_.size() >= 64) return;
        requestedBlasts_.push_back(blast);
    }

    void HandleHitReport(Event& event) {
        if (event.payload.size() < sizeof(ClientHitReportMessage)) return;
        ClientHitReportMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        const PlayerId shooter = it->second;
        // A client reporting damage against itself is the environment-damage
        // path; anything else is a shot at another player.
        const PlayerId attributed =
            message.target == shooter ? kInvalidPlayerId : shooter;
        ApplyHitToPlayer(attributed, message.target, message.damage,
                         message.headshot != 0, message.hitX, message.hitY,
                         message.hitZ);
    }

    void HandleReviveProgress(Event& event) {
        if (event.payload.size() < sizeof(ClientReviveProgressMessage)) return;
        ClientReviveProgressMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        PlayerSlot& slot = players_[it->second];
        slot.pendingReviveTarget = message.target;
        slot.pendingReviveHolding = message.holding != 0;
    }

    void HandlePlayerStateChanged(Event& event) {
        if (event.payload.size() < sizeof(ServerPlayerStateChangedMessage))
            return;
        ServerPlayerStateChangedMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.id >= kMaxPlayers) return;
        // Queue the edge for its one-shot effect only. The state itself is not
        // written here: the snapshot owns it, and letting a reliable event
        // write state is exactly how the two end up disagreeing.
        PlayerStateChange change;
        change.id = message.id;
        change.event = message.event;
        change.instigator = message.instigator;
        change.health = message.health;
        change.impulseX = message.impulseX;
        change.impulseY = message.impulseY;
        change.impulseZ = message.impulseZ;
        change.impactX = message.impactX;
        change.impactY = message.impactY;
        change.impactZ = message.impactZ;
        stateChanges_.push_back(change);
    }

    void HandleHello(Event& event) {
        if (event.payload.size() < sizeof(ClientHello)) return;
        ClientHello hello{};
        std::memcpy(&hello, event.payload.data(), sizeof(hello));
        // Refuse anything that is not this build before reading further. A
        // mismatched peer misreading our bytes is far worse to debug than
        // being told the versions differ.
        if (hello.magic != kProtocolMagic ||
            hello.version != kProtocolVersion) {
            SGE_LOG("LogNet", EngineLog::Level::Warning,
                "rejecting peer " + std::to_string(event.peer) +
                ": version/magic mismatch (theirs " +
                std::to_string(hello.version) + ", ours " +
                std::to_string(kProtocolVersion) + ")");
            ServerReject reject;
            reject.reason = RejectReason::VersionMismatch;
            transport_->Send(event.peer, &reject, sizeof(reject),
                             Channel::Reliable);
            return;
        }
        const PlayerId assigned = AllocatePlayer(event.peer);
        if (assigned == kInvalidPlayerId) {
            SGE_LOG("LogNet", EngineLog::Level::Warning,
                "rejecting peer " + std::to_string(event.peer) +
                ": session full");
            ServerReject reject;
            reject.reason = RejectReason::ServerFull;
            transport_->Send(event.peer, &reject, sizeof(reject),
                             Channel::Reliable);
            return;
        }
        SGE_LOG("LogNet", EngineLog::Level::Display,
            "hello accepted from peer " + std::to_string(event.peer) +
            "; assigned player " + std::to_string(assigned));
        ServerWelcome welcome;
        welcome.assignedId = assigned;
        transport_->Send(event.peer, &welcome, sizeof(welcome),
                         Channel::Reliable);

        PlayerJoinedMessage joined;
        joined.id = assigned;
        transport_->Broadcast(&joined, sizeof(joined), Channel::Reliable,
                              event.peer);

        // Straight after the welcome, so a player joining a session already in
        // progress loads the host's map instead of sitting in the menu waiting
        // for a level change that may never come -- the host is already there.
        SendLevelTo(event.peer);
        // And the ground as it stands. Without this a player joining a session
        // in progress walks onto pristine terrain while everyone else is taking
        // cover in craters that, to them, are not there.
        SendTerrainDeformsTo(event.peer);
        SendActiveChargesTo(event.peer);
    }

    void SendActiveChargesTo(PeerId peer) {
        if (role_ != Role::Host || !transport_ || peer == kInvalidPeer) return;
        for (const ServerChargeStuckMessage& charge : activeCharges_)
            transport_->Send(peer, &charge, sizeof(charge), Channel::Reliable);
    }

    // Replays the cuts made so far to one joining peer. One message per stamp
    // rather than a batch: it reuses the live handler and its dedup exactly, and
    // the backlog is bounded, reliable, and sent behind a level load the client
    // is already waiting on.
    void SendTerrainDeformsTo(PeerId peer) {
        if (role_ != Role::Host || !transport_ || peer == kInvalidPeer) return;
        for (const TerrainDeformEvent& stored : terrainDeforms_) {
            ServerTerrainDeformMessage message;
            message.deformId = stored.deformId;
            message.deform = stored.deform;
            transport_->Send(peer, &message, sizeof(message), Channel::Reliable);
        }
    }

    // peer == kInvalidPeer broadcasts; otherwise it answers one joining player.
    void SendLevelTo(PeerId peer) {
        if (role_ != Role::Host || !transport_) return;
        ServerLevelMessage message;
        message.kind = levelKind_;
        message.restartSerial = levelRestartSerial_;
        // Bounded copy into a fixed field: the name came from a filesystem path
        // and nothing upstream promises it is short.
        const size_t length =
            (std::min)(levelFile_.size(), size_t(kMaxLevelFileName - 1));
        std::memcpy(message.file, levelFile_.data(), length);
        message.file[length] = '\0';
        if (peer == kInvalidPeer)
            transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
        else
            transport_->Send(peer, &message, sizeof(message), Channel::Reliable);
    }

    void HandleLevel(Event& event) {
        if (event.payload.size() < sizeof(ServerLevelMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerLevelMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.kind != LevelKind::None &&
            message.kind != LevelKind::Level1 &&
            message.kind != LevelKind::TestLevel &&
            message.kind != LevelKind::LevelFile) return;
        // The wire field is a char array from another process: treat it as
        // bytes, not as a string that is promised to be terminated.
        message.file[kMaxLevelFileName - 1] = '\0';
        const std::string file(message.file);
        // A file name is all that may cross: anything with a separator or a
        // parent reference in it is a path, and a path from the network is a
        // way to point this machine at a file the host chose.
        if (message.kind == LevelKind::LevelFile &&
            (file.empty() || file.find('/') != std::string::npos ||
             file.find('\\') != std::string::npos ||
             file.find("..") != std::string::npos)) {
            SGE_LOG("LogNet", EngineLog::Level::Warning,
                "ignoring level '" + file + "': not a bare file name");
            return;
        }
        const bool sameLevel = message.kind == levelKind_ && file == levelFile_;
        if (sameLevel && message.restartSerial == levelRestartSerial_) return;
        // Only a restart when this machine already knew the level: a joining
        // player's first message carries whatever serial the host is on.
        if (sameLevel) levelRestartPending_ = true;
        levelRestartSerial_ = message.restartSerial;
        levelKind_ = message.kind;
        levelFile_ = file;
        chargeSticks_.clear();
        chargeDetonations_.clear();
        receivedChargeIds_.clear();
        hasPendingLevel_ = true;
        SGE_LOG("LogNet", EngineLog::Level::Display,
            "host is on level " + std::to_string(int(levelKind_)) +
            (file.empty() ? std::string() : " (" + file + ")"));
    }

    void HandleWelcome(Event& event) {
        if (event.payload.size() < sizeof(ServerWelcome)) return;
        ServerWelcome welcome{};
        std::memcpy(&welcome, event.payload.data(), sizeof(welcome));
        if (welcome.assignedId >= kMaxPlayers) return;
        localId_ = welcome.assignedId;
        // The welcome is sufficient to establish the server peer even in a
        // transport/test harness that delivers the handshake without a
        // separate Connected event.
        serverPeer_ = event.peer;
        players_[localId_].active = true;
        players_[localId_].id = localId_;
        SGE_LOG("LogNet", EngineLog::Level::Display,
            "welcome received; we are player " + std::to_string(localId_));
    }

    void HandleInput(Event& event) {
        if (event.payload.size() < sizeof(ClientInputMessage)) return;
        ClientInputMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        PlayerSlot& slot = players_[it->second];
        // Drop inputs that arrive out of order: UDP reorders, and applying an
        // older input after a newer one would rubber-band the player.
        if (!std::isfinite(message.x) || !std::isfinite(message.y) ||
            !std::isfinite(message.z)) return;
        if (slot.hasInput && message.input.sequence <= slot.lastInput.sequence)
            return;
        // Match the position used by the client revive prompt. Integrating
        // one render frame of input per net tick loses movement.
        if (!slot.downed) {
            slot.current.x = message.x;
            slot.current.y = message.y;
            slot.current.z = message.z;
        }
        slot.lastInput = message.input;
        slot.current.helicopter = ValidInsertionHelicopter(message.helicopter)
            ? message.helicopter : InsertionHelicopterState{};
        slot.current.vehicle = ValidDrivenVehicle(message.vehicle)
            ? message.vehicle : DrivenVehicleState{};
        slot.hasInput = true;
        slot.current.yaw = message.input.yaw;
        slot.current.pitch = message.input.pitch;
        slot.current.moving = message.input.Moving() ? 1 : 0;
        slot.current.crouching =
            message.input.Held(PlayerInput::Crouch) ? 1 : 0;
        slot.current.sprinting =
            message.input.Held(PlayerInput::Sprint) ? 1 : 0;
        slot.current.aiming =
            message.input.Held(PlayerInput::Aim) ? 1 : 0;
    }

    void HandleSnapshot(Event& event) {
        if (event.payload.size() < sizeof(ServerSnapshotMessage)) return;
        ServerSnapshotMessage snapshot{};
        std::memcpy(&snapshot, event.payload.data(), sizeof(snapshot));
        // Older than what we already have: discard rather than rewind. This is
        // what makes out-of-order UDP delivery harmless.
        if (snapshot.tick <= lastSnapshotTick_ && lastSnapshotTick_ != 0) return;
        // Only the first: a line every tick at 30 Hz would bury the log.
        if (lastSnapshotTick_ == 0) {
            SGE_LOG("LogNet", EngineLog::Level::Display,
                "first snapshot: tick " + std::to_string(snapshot.tick) +
                ", " + std::to_string(snapshot.playerCount) + " players");
        }
        lastSnapshotTick_ = snapshot.tick;

        const uint8_t count =
            snapshot.playerCount < kMaxPlayers ? snapshot.playerCount
                                               : kMaxPlayers;
        // The host's god mode. Every entry carries it, so the first will do;
        // a snapshot with no players in it says nothing and changes nothing.
        if (count > 0) {
            sessionGodMode_ = snapshot.players[0].godMode != 0;
            sessionGodModeKnown_ = true;
        }
        for (uint8_t i = 0; i < count; ++i) {
            const PlayerSnapshot& incoming = snapshot.players[i];
            if (incoming.id >= kMaxPlayers) continue;
            PlayerSlot& slot = players_[incoming.id];
            slot.previous = slot.current;
            slot.previousTime = slot.currentTime;
            slot.current = incoming;
            if (!ValidInsertionHelicopter(slot.current.helicopter))
                slot.current.helicopter = {};
            if (!ValidDrivenVehicle(slot.current.vehicle))
                slot.current.vehicle = {};
            slot.currentTime = interpolationTime_;
            // Visibility/airframe changes mark a new insertion, not a path
            // through the old flight. Equal-time packets replace the newest
            // sample rather than evicting useful history when UDP batches.
            if (slot.previous.helicopter.visible != slot.current.helicopter.visible ||
                slot.previous.helicopter.airframe != slot.current.helicopter.airframe)
                slot.renderSampleCount = 0;
            if (slot.renderSampleCount > 0 &&
                slot.renderSamples[slot.renderSampleCount - 1].time == interpolationTime_)
                --slot.renderSampleCount;
            if (slot.renderSampleCount == slot.renderSamples.size()) {
                for (size_t sample = 1; sample < slot.renderSampleCount; ++sample)
                    slot.renderSamples[sample - 1] = slot.renderSamples[sample];
                --slot.renderSampleCount;
            }
            slot.renderSamples[slot.renderSampleCount++] =
                { slot.current, interpolationTime_ };
            slot.active = true;
            slot.id = incoming.id;
            // The snapshot is the authority on life state. Mirrored onto the
            // slot so LocalStatus and the revive logic read one place, and
            // taken unconditionally: the stale-tick discard above already
            // guarantees this snapshot is newer than what we had, which is what
            // stops an out-of-order packet from resurrecting a downed player.
            slot.health = incoming.health;
            slot.downed = incoming.downed != 0;
            slot.reviver = incoming.reviver;
            // Rides the same stale-tick discard as the rest of the life state,
            // so an out-of-order packet cannot rewind a bar that has moved on.
            slot.reviveProgress = incoming.reviveProgress;
        }
    }

    void SendSnapshot() {
        ServerSnapshotMessage snapshot;
        snapshot.tick = tick_;
        uint8_t count = 0;
        for (uint8_t i = 0; i < kMaxPlayers; ++i) {
            if (!players_[i].active) continue;
            snapshot.players[count] = players_[i].current;
            snapshot.players[count].id = i;
            // Life state comes off the slot, not off `current`. The host owns
            // it for every player, and only the local slot's copy is refreshed
            // in Update -- a remote player's would otherwise ship whatever was
            // last received rather than what the host just decided.
            snapshot.players[count].health = players_[i].health;
            snapshot.players[count].downed = players_[i].downed ? 1 : 0;
            snapshot.players[count].reviver = players_[i].reviver;
            // Raw seconds; the receiver normalises. StepRevives is the only
            // writer and it is host-only, so this send is the single thing that
            // makes the number exist anywhere else.
            snapshot.players[count].reviveProgress = players_[i].reviveProgress;
            // Same value on every entry: it is the session's, not the player's.
            snapshot.players[count].godMode = sessionGodMode_ ? 1 : 0;
            ++count;
        }
        snapshot.playerCount = count;
        transport_->Broadcast(&snapshot, sizeof(snapshot), Channel::Unreliable);
    }

    // Per-connection rather than broadcast: "nearest" is a different set for
    // every player, so each client gets its own message.
    // Broadcast rather than per-connection: unlike the enemy snapshot there is
    // no "nearest" to compute -- there are two airframes and everyone sees the
    // same two.
    void SendVehicleState() {
        if (!hasHostHelicopters_ || !transport_) return;
        ServerVehicleStateMessage message;
        message.tick = tick_;
        message.escapeBoat = hostEscapeBoat_;
        for (uint8_t i = 0; i < kEnemyHelicopterCount; ++i) {
            const EnemyHelicopterState& source = hostHelicopters_[i];
            EnemyHelicopterSnapshot& out = message.helicopters[i];
            out.present = source.present ? 1 : 0;
            out.dead = source.dead ? 1 : 0;
            out.crashed = source.crashed ? 1 : 0;
            out.x = source.x; out.y = source.y; out.z = source.z;
            out.yaw = source.yaw;
            out.pitch = source.pitch;
            out.roll = source.roll;
            out.health = source.health;
        }
        transport_->Broadcast(&message, sizeof(message), Channel::Unreliable);
    }

    void SendArmorState() {
        if (!hasHostArmor_ || !transport_) return;
        hostArmor_.tick = tick_;
        transport_->Broadcast(&hostArmor_, sizeof(hostArmor_),
                              Channel::Unreliable);
    }

    void HandleClientChargeStuck(Event& event) {
        if (event.payload.size() < sizeof(ClientChargeStuckMessage)) return;
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        ClientChargeStuckMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (!ValidChargeStick(message.charge)) return;
        chargeStickRequests_.push_back({ it->second, message.charge });
    }

    void HandleServerChargeStuck(Event& event) {
        if (event.payload.size() < sizeof(ServerChargeStuckMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerChargeStuckMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.owner >= kMaxPlayers || message.chargeId == 0 ||
            !ValidChargeStick(message.charge) ||
            !receivedChargeIds_.insert(message.chargeId).second) return;
        chargeSticks_.push_back({ message.owner, message.chargeId,
                                  message.charge });
    }

    void HandleClientChargeDetonate(Event& event) {
        if (event.payload.size() < sizeof(ClientChargeDetonateMessage)) return;
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        RemoveActiveChargesFor(it->second);
        chargeDetonations_.push_back({ it->second });
        ServerChargeDetonateMessage out;
        out.owner = it->second;
        transport_->Broadcast(&out, sizeof(out), Channel::Reliable);
    }

    // Copies a line into a wire buffer, truncating rather than refusing, and
    // always leaving it NUL-terminated. Control characters are dropped: a tab
    // or a stray newline would break the single-line layout the log draws, and
    // a lone CR could hide the rest of a line entirely.
    static void CopyChatText(char (&destination)[kMaxChatTextLength + 1],
                             const char* source) {
        size_t written = 0;
        for (const char* c = source; *c && written < kMaxChatTextLength; ++c) {
            const unsigned char value = static_cast<unsigned char>(*c);
            // Printable ASCII only, DEL and above included in the rejection:
            // the HUD reads text as UTF-8, and a stray high byte from a peer
            // would draw as a broken glyph on every machine.
            if (value < ' ' || value > '~') continue;
            destination[written++] = *c;
        }
        destination[written] = '\0';
    }

    void QueueChat(PlayerId speaker, const char* text) {
        ChatLine line;
        line.speaker = speaker;
        line.text = text ? text : "";
        line.fromLocalPlayer = speaker == localId_;
        // Bounded so a long session cannot grow this without limit if nothing
        // ever drains it -- the oldest line is the one worth losing.
        constexpr size_t kMaxQueued = 64;
        if (chatLines_.size() >= kMaxQueued) chatLines_.erase(chatLines_.begin());
        chatLines_.push_back(std::move(line));
    }

    void HandleClientChat(Event& event) {
        if (event.payload.size() < sizeof(ClientChatMessage)) return;
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        ClientChatMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        // The sender's terminator is not trusted: 128 non-zero bytes would run
        // every later read off the end of the buffer.
        message.text[kMaxChatTextLength] = '\0';

        ServerChatMessage out;
        out.speaker = it->second;
        // Re-copied rather than forwarded, so a client cannot inject control
        // characters into everyone else's log.
        CopyChatText(out.text, message.text);
        if (out.text[0] == '\0') return;
        transport_->Broadcast(&out, sizeof(out), Channel::Reliable);
        // Broadcast reaches the clients; the host queues its own copy.
        QueueChat(out.speaker, out.text);
    }

    void HandleServerChat(Event& event) {
        if (event.payload.size() < sizeof(ServerChatMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerChatMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        message.text[kMaxChatTextLength] = '\0';
        if (message.text[0] == '\0') return;
        QueueChat(message.speaker, message.text);
    }

    void HandleClientMedicCall(Event& event) {
        if (event.payload.size() < sizeof(ClientMedicCallMessage)) return;
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        PlayerId callerId = it->second;

        ServerMedicCallMessage out;
        out.callerId = callerId;
        transport_->Broadcast(&out, sizeof(out), Channel::Reliable);
        // Host queues its own call directly.
        medicCalls_.push_back(callerId);
    }

    void HandleClientMarineDrop(Event& event) {
        if (event.payload.size() < sizeof(ClientMarineDropMessage)) return;
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        ClientMarineDropMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.count == 0 || !Finite3(message.x, message.y, message.z))
            return;
        MarineDropRequest request;
        request.requester = it->second;
        request.count = message.count;
        request.x = message.x; request.y = message.y; request.z = message.z;
        marineDrops_.push_back(request);
    }

    void HandleServerMedicCall(Event& event) {
        if (event.payload.size() < sizeof(ServerMedicCallMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerMedicCallMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.callerId >= kMaxPlayers) return;
        medicCalls_.push_back(message.callerId);
    }

    void HandleServerScoreboard(Event& event) {
        if (event.payload.size() < sizeof(ServerScoreboardMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerScoreboardMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        const uint8_t count = message.playerCount < kMaxPlayers
            ? message.playerCount : kMaxPlayers;
        scoreboard_.clear();
        scoreboard_.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            if (message.entries[i].id >= kMaxPlayers) continue;
            scoreboard_.push_back(message.entries[i]);
        }
    }

    void HandleServerChargeDetonate(Event& event) {
        if (event.payload.size() < sizeof(ServerChargeDetonateMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerChargeDetonateMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        chargeDetonations_.push_back({ message.owner });
    }

    void HandleClientShotFired(Event& event) {
        if (event.payload.size() < sizeof(ClientShotFiredMessage)) return;
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        ClientShotFiredMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (!Finite3(message.x, message.y, message.z) ||
            !Finite3(message.dirX, message.dirY, message.dirZ)) return;
        // The host presents it too, then passes it on to everyone else. Sent
        // rather than re-broadcast to all so the shooter is not told about its
        // own round.
        remoteShots_.push_back({ it->second, message.x, message.y, message.z,
                                 message.dirX, message.dirY, message.dirZ });
        ServerShotFiredMessage out;
        out.shooter = it->second;
        out.x = message.x; out.y = message.y; out.z = message.z;
        out.dirX = message.dirX; out.dirY = message.dirY;
        out.dirZ = message.dirZ;
        for (const PlayerSlot& slot : players_) {
            if (!slot.active || slot.peer == kInvalidPeer ||
                slot.id == it->second) continue;
            transport_->Send(slot.peer, &out, sizeof(out), Channel::Unreliable);
        }
    }

    void HandleServerShotFired(Event& event) {
        if (event.payload.size() < sizeof(ServerShotFiredMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerShotFiredMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.shooter == localId_ ||
            !Finite3(message.x, message.y, message.z) ||
            !Finite3(message.dirX, message.dirY, message.dirZ)) return;
        remoteShots_.push_back({ message.shooter, message.x, message.y,
                                 message.z, message.dirX, message.dirY,
                                 message.dirZ });
    }

    void HandleVehicleState(Event& event) {
        if (event.payload.size() < sizeof(ServerVehicleStateMessage)) return;
        ServerVehicleStateMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (hasRemoteHelicopters_ &&
            static_cast<int32_t>(message.tick - remoteVehicleTick_) <= 0) return;
        const auto& boat = message.escapeBoat;
        if (boat.active > 1 || !Finite3(boat.x, boat.y, boat.z) ||
            !std::isfinite(boat.yaw) || !std::isfinite(boat.bobTime)) return;
        for (uint8_t i = 0; i < kEnemyHelicopterCount; ++i) {
            const EnemyHelicopterSnapshot& in = message.helicopters[i];
            // Unreliable, so a corrupt or partial packet must not be allowed to
            // teleport a gunship to a NaN and take the flight code with it.
            if (!Finite3(in.x, in.y, in.z) ||
                !Finite3(in.yaw, in.pitch, in.roll) ||
                !std::isfinite(in.health)) return;
        }
        for (uint8_t i = 0; i < kEnemyHelicopterCount; ++i) {
            const EnemyHelicopterSnapshot& in = message.helicopters[i];
            EnemyHelicopterState& out = remoteHelicopters_[i];
            out.present = in.present != 0;
            out.dead = in.dead != 0;
            out.crashed = in.crashed != 0;
            out.x = in.x; out.y = in.y; out.z = in.z;
            out.yaw = in.yaw;
            out.pitch = in.pitch;
            out.roll = in.roll;
            out.health = in.health;
        }
        remoteEscapeBoat_ = message.escapeBoat;
        remoteVehicleTick_ = message.tick;
        hasRemoteHelicopters_ = true;
    }

    void HandleArmorState(Event& event) {
        if (event.payload.size() < sizeof(ServerArmorStateMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerArmorStateMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (hasRemoteArmor_ &&
            static_cast<int32_t>(message.tick - remoteArmorTick_) <= 0) return;
        if (message.tankCount > kMaxReplicatedTanks ||
            message.turretCount > kMaxReplicatedAATurrets ||
            message.humveeCount > kMaxReplicatedHumvees ||
            message.planeCount > kMaxReplicatedPlanes) return;
        // Unreliable, so a corrupt packet must not hand the pose code a NaN.
        for (uint8_t i = 0; i < message.tankCount; ++i) {
            const EnemyTankSnapshot& tank = message.tanks[i];
            if (!Finite3(tank.x, tank.y, tank.z) ||
                !Finite3(tank.qx, tank.qy, tank.qz) ||
                !std::isfinite(tank.qw) || !std::isfinite(tank.turretYaw) ||
                !std::isfinite(tank.health)) return;
        }
        for (uint8_t i = 0; i < message.turretCount; ++i) {
            const AATurretSnapshot& turret = message.turrets[i];
            if (!Finite3(turret.x, turret.z, turret.yaw) ||
                !Finite3(turret.pitch, turret.heat, turret.health)) return;
        }
        for (uint8_t i = 0; i < message.humveeCount; ++i) {
            const EnemyHumveeSnapshot& humvee = message.humvees[i];
            if (!Finite3(humvee.x, humvee.y, humvee.z) ||
                !Finite3(humvee.qx, humvee.qy, humvee.qz) ||
                !Finite3(humvee.qw, humvee.turretYaw, 0.0f)) return;
        }
        for (uint8_t i = 0; i < message.planeCount; ++i) {
            const ObjectivePlaneSnapshot& plane = message.planes[i];
            if (!Finite3(plane.holdTimer, plane.takeoffTimer, 0.0f) ||
                !Finite3(plane.crashX, plane.crashY, plane.crashZ) ||
                !Finite3(plane.crashVX, plane.crashVY, plane.crashVZ) ||
                !Finite3(plane.crashPitch, plane.crashRoll, plane.crashYaw))
                return;
        }
        remoteArmor_ = message;
        remoteArmorTick_ = message.tick;
        hasRemoteArmor_ = true;
    }

    void HandleSquadDeploy(Event& event) {
        if (event.payload.size() < sizeof(ServerSquadDeployMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerSquadDeployMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.insertionMode > 2 || message.airframe > 1 ||
            !Finite3(message.x, message.y, message.z)) return;
        squadDeploy_.insertionMode = message.insertionMode;
        squadDeploy_.airframe = message.airframe;
        squadDeploy_.hostLeftSeat = message.hostLeftSeat != 0;
        squadDeploy_.x = message.x;
        squadDeploy_.y = message.y;
        squadDeploy_.z = message.z;
        squadDeployPending_ = true;
    }

    void HandleEnemyFire(Event& event) {
        if (event.payload.size() < sizeof(ServerEnemyFireMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerEnemyFireMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.kind != EnemyFireKind::TankShell &&
            message.kind != EnemyFireKind::AAShell &&
            message.kind != EnemyFireKind::InfantryShot) return;
        if (message.kind == EnemyFireKind::InfantryShot &&
            message.weapon != InfantryWeapon::Rifle &&
            message.weapon != InfantryWeapon::Shotgun &&
            message.weapon != InfantryWeapon::Sniper) return;
        if (!Finite3(message.x, message.y, message.z) ||
            !Finite3(message.dirX, message.dirY, message.dirZ) ||
            !std::isfinite(message.speed) ||
            !std::isfinite(message.lifetime) ||
            !std::isfinite(message.damageScale)) return;
        // Bounded: a client that is not draining (a loading screen) must not
        // bank minutes of AA bursts and fire them all at once afterwards.
        if (enemyFire_.size() >= 256) return;
        RemoteEnemyFire fire;
        fire.kind = message.kind;
        fire.weapon = message.weapon;
        fire.x = message.x; fire.y = message.y; fire.z = message.z;
        fire.dirX = message.dirX; fire.dirY = message.dirY;
        fire.dirZ = message.dirZ;
        fire.speed = message.speed;
        fire.lifetime = message.lifetime;
        // Clamped: the scale multiplies damage to this machine's player.
        fire.damageScale =
            (std::max)(0.0f, (std::min)(message.damageScale, 4.0f));
        enemyFire_.push_back(fire);
    }

    void SendEnemySnapshots() {
        if (hostEnemies_.empty()) return;
        for (const auto& entry : peerToPlayer_) {
            const PeerId peer = entry.first;
            const PlayerId id = entry.second;
            if (id >= kMaxPlayers || !players_[id].active) continue;
            const PlayerSlot& slot = players_[id];

            // Nearest first. Partial rather than a full sort: only the front
            // kMaxReplicatedEnemies matter and a level can hold many times that.
            enemyOrder_.clear();
            enemyOrder_.reserve(hostEnemies_.size());
            for (size_t i = 0; i < hostEnemies_.size(); ++i) {
                const HostEnemyState& enemy = hostEnemies_[i];
                const float dx = enemy.x - slot.current.x;
                const float dz = enemy.z - slot.current.z;
                enemyOrder_.push_back({ dx * dx + dz * dz, i });
            }
            const size_t send =
                enemyOrder_.size() < kMaxReplicatedEnemies
                    ? enemyOrder_.size() : kMaxReplicatedEnemies;
            std::partial_sort(
                enemyOrder_.begin(), enemyOrder_.begin() + send,
                enemyOrder_.end(),
                [](const RankedEnemy& a, const RankedEnemy& b) {
                    return a.distanceSquared < b.distanceSquared;
                });

            ServerEnemySnapshotMessage message;
            message.tick = tick_;
            message.enemyCount = static_cast<uint8_t>(send);
            for (size_t i = 0; i < send; ++i) {
                const HostEnemyState& source =
                    hostEnemies_[enemyOrder_[i].index];
                EnemySnapshot& out = message.enemies[i];
                out.id = source.id;
                out.x = source.x; out.y = source.y; out.z = source.z;
                out.yaw = source.yaw;
                out.aimYaw = source.aimYaw;
                out.aimPitch = source.aimPitch;
                out.health = source.health;
                out.moving = source.moving ? 1 : 0;
                out.dead = source.dead ? 1 : 0;
                out.killer = source.killer;
                out.marine = source.marine ? 1 : 0;
            }
            transport_->Send(peer, &message, sizeof(message),
                             Channel::Unreliable);
        }
    }

    void SendScoreboard() {
        if (role_ != Role::Host || !transport_) return;
        bool anyDirty = false;
        for (uint8_t i = 0; i < kMaxPlayers; ++i) {
            if (players_[i].scoreboardDirty) {
                anyDirty = true;
                players_[i].scoreboardDirty = false;
            }
        }
        if (!anyDirty) return;
        ServerScoreboardMessage message;
        message.playerCount = 0;
        for (uint8_t i = 0; i < kMaxPlayers; ++i) {
            const PlayerSlot& slot = players_[i];
            if (!slot.active) continue;
            message.entries[message.playerCount].id = slot.id;
            message.entries[message.playerCount].kills = slot.kills;
            message.entries[message.playerCount].deaths = slot.deaths;
            message.entries[message.playerCount].revives = slot.revives;
            message.playerCount++;
        }
        transport_->Broadcast(&message, sizeof(message), Channel::Reliable);
    }

    void HandleEnemySnapshot(Event& event) {
        if (event.payload.size() < sizeof(ServerEnemySnapshotMessage)) return;
        ServerEnemySnapshotMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        // Same discard rule as the player snapshot: older than what we have is
        // dropped rather than rewound, which is what makes UDP reordering
        // harmless. An enemy that flickered back to a stale position would read
        // as the AI teleporting.
        if (message.tick <= lastEnemyTick_ && lastEnemyTick_ != 0) return;
        lastEnemyTick_ = message.tick;

        const uint8_t count = message.enemyCount < kMaxReplicatedEnemies
            ? message.enemyCount : kMaxReplicatedEnemies;
        remoteEnemies_.clear();
        remoteEnemies_.reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            const EnemySnapshot& incoming = message.enemies[i];
            if (incoming.id == kInvalidEnemyId) continue;
            RemoteEnemy enemy;
            enemy.id = incoming.id;
            enemy.x = incoming.x; enemy.y = incoming.y; enemy.z = incoming.z;
            enemy.yaw = incoming.yaw;
            enemy.aimYaw = incoming.aimYaw;
            enemy.aimPitch = incoming.aimPitch;
            enemy.health = incoming.health;
            enemy.moving = incoming.moving != 0;
            enemy.dead = incoming.dead != 0;
            enemy.killer = incoming.killer;
            enemy.marine = incoming.marine != 0;
            remoteEnemies_.push_back(enemy);
        }
    }

    void HandleEnemyHitReport(Event& event) {
        if (event.payload.size() < sizeof(ClientEnemyHitReportMessage)) return;
        ClientEnemyHitReportMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end()) return;
        // Clamped for the same reason player damage is: shooter-authoritative
        // means unvalidated, and a bug must not be able to send a wild value
        // into the gameplay code.
        if (!(message.damage >= 0.0f)) return;
        EnemyHitRequest request;
        request.target = message.target;
        request.shooter = it->second;
        request.damage = message.damage < 1000.0f ? message.damage : 1000.0f;
        request.headshot = message.headshot != 0;
        request.dirX = message.dirX;
        request.dirY = message.dirY;
        request.dirZ = message.dirZ;
        request.hitX = message.hitX;
        request.hitY = message.hitY;
        request.hitZ = message.hitZ;
        enemyHits_.push_back(request);
    }

    void HandleWorldImpact(Event& event) {
        if (event.payload.size() < sizeof(ClientWorldImpactMessage)) return;
        ClientWorldImpactMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        const auto it = peerToPlayer_.find(event.peer);
        // A surface or tree hit names no entity, so its impactId is the only
        // stable key. A client that sent one gets exact-once delivery; one that
        // sent zero still gets its damage applied, under an id minted here --
        // rejecting it instead silently drops every unkeyed bullet.
        const uint32_t impactId =
            (message.impactId == 0 && message.entityId == 0 &&
             ValidWorldKind(message.kind) && message.kind != 1)
                ? AllocateImpactId() : message.impactId;
        if (it == peerToPlayer_.end() ||
            !ValidWorldTarget(message.entityId, message.kind, impactId) ||
            !(message.damage > 0.0f) || !std::isfinite(message.damage) ||
            !std::isfinite(message.radius) || !std::isfinite(message.impulse) ||
            !Finite3(message.dirX, message.dirY, message.dirZ) ||
            message.radius < 0.0f ||
            !Finite3(message.hitX, message.hitY, message.hitZ)) return;
        // Dedupe on the sender's own key only. An id minted above is unique by
        // construction, so putting it in the set would only grow the set.
        if (message.impactId != 0) {
            const uint64_t impactKey =
                (uint64_t(it->second) << 32) | message.impactId;
            if (!receivedWorldImpacts_.insert(impactKey).second) return;
        }
        worldImpacts_.push_back({ impactId, message.kind, it->second,
                                  message.playerOwned != 0,
                                  message.remoteCharge != 0,
                                  message.entityId,
                                  message.damage, message.radius,
                                  message.impulse, message.dirX,
                                  message.dirY, message.dirZ,
                                  message.hitX,
                                  message.hitY, message.hitZ });
    }

    void HandleWorldBreak(Event& event) {
        if (event.payload.size() < sizeof(ServerWorldBreakMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerWorldBreakMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (!ValidWorldTarget(message.entityId, message.kind,
                              message.impactId) ||
            !std::isfinite(message.damage) || !std::isfinite(message.radius) ||
            !std::isfinite(message.impulse) ||
            !Finite3(message.dirX, message.dirY, message.dirZ) ||
            message.radius < 0.0f ||
            !Finite3(message.hitX, message.hitY, message.hitZ)) return;
        const uint64_t dedupe = message.impactId != 0
            ? ((uint64_t(1) << 63) | message.impactId) : message.entityId;
        if (!receivedWorldBreaks_.insert(dedupe).second) return;
        worldBreaks_.push_back({ message.impactId, message.kind,
                                 message.shooter, message.playerOwned != 0,
                                 message.remoteCharge != 0,
                                 message.entityId, message.damage,
                                 message.radius, message.impulse,
                                 message.dirX, message.dirY, message.dirZ,
                                 message.hitX, message.hitY, message.hitZ });
    }

    void HandleGrenadeThrow(Event& event) {
        if (event.payload.size() < sizeof(ClientGrenadeThrowMessage)) return;
        ClientGrenadeThrowMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        const auto it = peerToPlayer_.find(event.peer);
        if (it == peerToPlayer_.end() || message.clientToken == 0 ||
            !ValidGrenade(message.kind) || !Finite3(message.x, message.y,
                                                     message.z) ||
            !Finite3(message.velocityX, message.velocityY,
                     message.velocityZ) || !std::isfinite(message.fuse) ||
            message.fuse < 0.0f) return;
        // The client is allowed to supply its launch pose, but one token may
        // only become one canonical grenade. Reliable retransmission must not
        // spawn duplicate physics bodies or detonation events.
        const uint64_t tokenKey =
            (uint64_t(it->second) << 32) | message.clientToken;
        if (!acceptedGrenadeTokens_.insert(tokenKey).second) return;
        const uint32_t id = AllocateGrenadeId();
        // Remembered so the thrower, and only the thrower, can later say where
        // this grenade finished up.
        grenadeOwners_[id] = it->second;
        GrenadeSpawnEvent spawn{ id, message.clientToken, it->second,
                                 message.kind, false, message.x, message.y,
                                 message.z, message.velocityX,
                                 message.velocityY, message.velocityZ,
                                 message.fuse };
        grenadeSpawns_.push_back(spawn);
        ServerGrenadeSpawnMessage reply;
        reply.grenadeId = id; reply.clientToken = spawn.clientToken;
        reply.owner = spawn.owner; reply.kind = spawn.kind;
        reply.hostile = 0;
        reply.x = spawn.x; reply.y = spawn.y; reply.z = spawn.z;
        reply.velocityX = spawn.velocityX; reply.velocityY = spawn.velocityY;
        reply.velocityZ = spawn.velocityZ; reply.fuse = spawn.fuse;
        transport_->Broadcast(&reply, sizeof(reply), Channel::Reliable);
    }

    void HandleGrenadeSpawn(Event& event) {
        if (event.payload.size() < sizeof(ServerGrenadeSpawnMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerGrenadeSpawnMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.grenadeId == 0 ||
            (message.owner >= kMaxPlayers &&
             message.owner != kInvalidPlayerId) ||
            !ValidGrenade(message.kind) || !Finite3(message.x, message.y,
                                                     message.z) ||
            !Finite3(message.velocityX, message.velocityY,
                     message.velocityZ) || !std::isfinite(message.fuse) ||
            message.fuse < 0.0f) return;
        // A retransmitted reliable packet or a delayed duplicate must not
        // create a second grenade. The app can match owner/token to its local
        // predicted throw and transfer the canonical id.
        if (!receivedGrenadeIds_.insert(message.grenadeId).second) return;
        grenadeSpawns_.push_back({ message.grenadeId, message.clientToken,
            message.owner, message.kind, message.hostile != 0,
            message.x, message.y, message.z,
            message.velocityX, message.velocityY, message.velocityZ,
            message.fuse });
    }

    void HandleGrenadeDetonated(Event& event) {
        if (event.payload.size() < sizeof(ServerGrenadeDetonatedMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerGrenadeDetonatedMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.grenadeId == 0 || !ValidGrenade(message.kind) ||
            !Finite3(message.x, message.y, message.z)) return;
        if (!receivedGrenadeDetonations_.insert(message.grenadeId).second) return;
        grenadeDetonations_.push_back({ message.grenadeId, message.kind,
                                        message.hostile != 0, message.x,
                                        message.y, message.z });
    }

    void HandleGrenadeDetonationReport(Event& event) {
        if (event.payload.size() < sizeof(ClientGrenadeDetonationMessage)) return;
        const auto peer = peerToPlayer_.find(event.peer);
        if (peer == peerToPlayer_.end()) return;
        ClientGrenadeDetonationMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.grenadeId == 0 ||
            !Finite3(message.x, message.y, message.z)) return;
        // Only the player who threw it may say where it landed. Without this a
        // client could walk anyone else's grenade -- including the host's --
        // across the map and drop the blast at its own choice of coordinates.
        const auto owner = grenadeOwners_.find(message.grenadeId);
        if (owner == grenadeOwners_.end() || owner->second != peer->second)
            return;
        // One report per grenade. Reliable delivery can repeat, and the host
        // must not be told twice to move a blast it has already run.
        if (!reportedGrenadeDetonations_.insert(message.grenadeId).second)
            return;
        grenadeDetonationReports_.push_back({ message.grenadeId,
                                              GrenadeKind::Frag, false,
                                              message.x, message.y,
                                              message.z });
    }

    void HandleTerrainDeformRequest(Event& event) {
        if (event.payload.size() < sizeof(ClientTerrainDeformMessage)) return;
        // Authenticated peers only, like world damage: a machine that has not
        // completed the handshake must not be able to reshape the map.
        if (peerToPlayer_.find(event.peer) == peerToPlayer_.end()) return;
        ClientTerrainDeformMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (!ValidDeform(message.deform)) return;
        requestedTerrainDeforms_.push_back(message.deform);
    }

    void HandleTerrainDeform(Event& event) {
        if (event.payload.size() < sizeof(ServerTerrainDeformMessage)) return;
        if (event.peer != serverPeer_) return;
        ServerTerrainDeformMessage message{};
        std::memcpy(&message, event.payload.data(), sizeof(message));
        if (message.deformId == 0) return;
        const TerrainDeform& deform = message.deform;
        // Same clamping bargain as every other event: these numbers drive a
        // height field and a physics rebuild, so a NaN radius from a bad packet
        // would corrupt the ground rather than fail loudly.
        if (!Finite3(deform.x, deform.z, deform.radius) ||
            !Finite3(deform.value, deform.strength, deform.edgeFalloff) ||
            !std::isfinite(deform.baseHeight) ||
            !std::isfinite(deform.impactY) ||
            deform.radius <= 0.0f) return;
        // The join backlog and the live broadcast overlap for a cut made while
        // a player was connecting, so the same id arrives twice by design.
        if (!receivedTerrainDeforms_.insert(message.deformId).second) return;
        terrainDeformEvents_.push_back({ message.deformId, deform });
    }

    void SendInput(const LocalPlayerState& local) {
        if (localId_ == kInvalidPlayerId || serverPeer_ == kInvalidPeer) return;
        ClientInputMessage message;
        message.input = local.input;
        message.x = local.x;
        message.y = local.y;
        message.z = local.z;
        message.helicopter = local.helicopter;
        message.vehicle = local.vehicle;
        message.input.sequence = ++inputSequence_;
        transport_->Send(serverPeer_, &message, sizeof(message),
                         Channel::Unreliable);
    }

    PlayerId AllocatePlayer(PeerId peer) {
        for (uint8_t i = 0; i < kMaxPlayers; ++i) {
            if (players_[i].active) continue;
            players_[i] = PlayerSlot{};
            players_[i].id = i;
            players_[i].active = true;
            players_[i].peer = peer;
            // A scoreboard only goes out on change; flag one so the newcomer
            // gets the standings now and everyone else gets the new row.
            players_[i].scoreboardDirty = true;
            peerToPlayer_[peer] = i;
            return i;
        }
        return kInvalidPlayerId;
    }

    void ReleasePeer(PeerId peer) {
        const auto it = peerToPlayer_.find(peer);
        if (it == peerToPlayer_.end()) return;
        const PlayerId id = it->second;
        peerToPlayer_.erase(it);
        if (id < kMaxPlayers) players_[id] = PlayerSlot{};
        // Sweep the departed player out of everyone else's revive state. A slot
        // still naming them as its reviver would hold progress that can never
        // advance, and a stale pendingReviveTarget pointing at a freed id would
        // credit whoever is allocated that slot next.
        for (PlayerSlot& other : players_) {
            if (other.reviver == id) {
                other.reviver = kInvalidPlayerId;
                other.reviveProgress = 0.0f;
            }
            if (other.pendingReviveTarget == id) {
                other.pendingReviveTarget = kInvalidPlayerId;
                other.pendingReviveHolding = false;
            }
        }
        if (role_ == Role::Host) {
            RemoveActiveChargesFor(id);
            PlayerLeftMessage left;
            left.id = id;
            transport_->Broadcast(&left, sizeof(left), Channel::Reliable);
        }
    }

    // Life-state edges waiting to be drained by the game this frame.
    std::vector<PlayerStateChange> stateChanges_;
    std::vector<ChatLine> chatLines_;
    // Medic callouts from teammates, waiting to be drained.
    std::vector<PlayerId> medicCalls_;
    std::vector<MarineDropRequest> marineDrops_;
    // Scoreboard entries, the host publishes when stats change.
    std::vector<ScoreboardEntry> scoreboard_;
    // The host's god mode toggle. Written by the host from its own local state
    // every Update, and by a client from each snapshot.
    bool sessionGodMode_ = false;
    // Client-side: whether a snapshot has told us yet. Until one has, the
    // client keeps its own toggle rather than being switched off by a default.
    bool sessionGodModeKnown_ = false;

    // Enemy replication. hostEnemies_ is what the host published this frame;
    // remoteEnemies_ is what a client was last told. Only one is ever populated
    // on a given machine.
    std::vector<HostEnemyState> hostEnemies_;
    std::vector<RemoteEnemy> remoteEnemies_;
    std::vector<RemoteShot> remoteShots_;
    std::vector<RemoteChargeStuck> chargeSticks_;
    std::vector<ChargeStickRequest> chargeStickRequests_;
    std::vector<RemoteChargeDetonate> chargeDetonations_;
    std::vector<ServerChargeStuckMessage> activeCharges_;
    std::unordered_set<uint32_t> receivedChargeIds_;
    uint32_t nextChargeId_ = 1;
    EnemyHelicopterState hostHelicopters_[kEnemyHelicopterCount]{};
    EnemyHelicopterState remoteHelicopters_[kEnemyHelicopterCount]{};
    EscapeBoatSnapshot hostEscapeBoat_{};
    EscapeBoatSnapshot remoteEscapeBoat_{};
    uint32_t remoteVehicleTick_ = 0;
    bool hasHostHelicopters_ = false;
    bool hasRemoteHelicopters_ = false;
    // Tanks and AA guns: the host's outgoing set, and a client's last received.
    ServerArmorStateMessage hostArmor_{};
    ServerArmorStateMessage remoteArmor_{};
    uint32_t remoteArmorTick_ = 0;
    bool hasHostArmor_ = false;
    bool hasRemoteArmor_ = false;
    std::vector<RemoteEnemyFire> enemyFire_;
    SquadDeployOrder squadDeploy_;
    bool squadDeployPending_ = false;
    std::vector<EnemyHitRequest> enemyHits_;
    std::vector<WorldImpactRequest> worldImpacts_;
    std::vector<WorldBreakEvent> worldBreaks_;
    std::vector<GrenadeSpawnEvent> grenadeSpawns_;
    std::vector<GrenadeDetonationEvent> grenadeDetonations_;
    // Host only: which player threw each live replicated grenade, and where
    // they say it ended up. The kind and hostile fields of the report events go
    // unused -- the host already knows both from the spawn -- but reusing the
    // event type keeps one shape for a grenade blast position.
    std::unordered_map<uint32_t, PlayerId> grenadeOwners_;
    std::vector<GrenadeDetonationEvent> grenadeDetonationReports_;
    std::unordered_set<uint32_t> reportedGrenadeDetonations_;
    // Reliable does not mean a caller cannot retry a send. Retain these keys
    // beyond queue draining so delayed duplicates cannot create another body or
    // replay a destruction edge on a later frame.
    std::unordered_set<uint64_t> acceptedGrenadeTokens_;
    std::unordered_set<uint64_t> receivedWorldImpacts_;
    std::unordered_set<uint32_t> receivedGrenadeIds_;
    std::unordered_set<uint32_t> receivedGrenadeDetonations_;
    std::unordered_set<uint64_t> receivedWorldBreaks_;
    std::unordered_set<uint32_t> receivedTerrainDeforms_;
    // On the host, every cut it has published, for replay to a joining peer. On
    // a client, terrainDeformEvents_ is what arrived and has yet to be applied.
    // Only one is ever populated on a given machine.
    std::vector<TerrainDeformEvent> terrainDeforms_;
    std::vector<TerrainDeformEvent> terrainDeformEvents_;
    // Host only: cuts clients have asked for, waiting to be applied and then
    // published back out as the host's own.
    std::vector<TerrainDeform> requestedTerrainDeforms_;
    std::vector<BarrelEventRecord> barrelEvents_;
    std::vector<BarrelEventRecord> requestedBarrelEvents_;
    std::vector<BlastRecord> requestedBlasts_;
    uint32_t nextGrenadeId_ = 1;
    uint32_t nextImpactId_ = 1;
    uint32_t nextDeformId_ = 1;
    // The map everyone is on. On the host this is what it publishes; on a
    // client it is the last thing the host said, and the pending flag is the
    // one-shot that makes the game load it.
    LevelKind levelKind_ = LevelKind::None;
    std::string levelFile_;
    bool hasPendingLevel_ = false;
    bool levelRestartPending_ = false;
    uint8_t levelRestartSerial_ = 0;
    std::string serverAddress_;
    uint16_t port_ = 0;
    // Whether this session is riding Steam's relay rather than a UDP port.
    // Decides what a friend is handed to join with, which is the only thing
    // above the transport that can tell the difference.
    bool overSteam_ = false;
    // Set when the session ended by itself, and read by the menu so a client
    // that never got in says why instead of waiting on a host that is gone.
    std::string lastError_;
    bool disconnectPending_ = false;
    uint32_t lastEnemyTick_ = 0;
    // Scratch for the per-client nearest-enemy sort, kept so the send does not
    // allocate once per client per tick.
    struct RankedEnemy { float distanceSquared; size_t index; };
    std::vector<RankedEnemy> enemyOrder_;

    std::unique_ptr<NetTransport> transport_;
    Role role_ = Role::Offline;
    PlayerId localId_ = kInvalidPlayerId;
    PeerId serverPeer_ = kInvalidPeer;
    std::array<PlayerSlot, kMaxPlayers> players_{};
    std::unordered_map<PeerId, PlayerId> peerToPlayer_;
    std::vector<Event> events_;
    FixedStepClock clock_;
    uint32_t tick_ = 0;
    uint32_t lastSnapshotTick_ = 0;
    uint32_t inputSequence_ = 0;
    float interpolationTime_ = 0.0f;
};

} // namespace net

#endif // NET_SESSION_H
