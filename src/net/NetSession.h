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
    PlayerId id = kInvalidPlayerId;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    bool moving = false;
    bool crouching = false;
    bool sprinting = false;
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

// What the local player is doing this frame, handed to the session.
struct LocalPlayerState {
    PlayerInput input;
    float x = 0.0f, y = 0.0f, z = 0.0f;
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

    bool StartHost(uint16_t port, std::string* error) {
        Shutdown();
        transport_ = MakeTransport();
        if (!transport_->Listen(port, error)) { transport_.reset(); return false; }
        role_ = Role::Host;
        // The host is always player 0, assigned without a handshake: it never
        // connects to itself.
        localId_ = 0;
        players_[0].active = true;
        players_[0].id = 0;
        return true;
    }

    bool StartClient(const std::string& host, uint16_t port,
                     std::string* error) {
        Shutdown();
        transport_ = MakeTransport();
        if (!transport_->Connect(host, port, error)) {
            transport_.reset();
            return false;
        }
        role_ = Role::Client;
        // Stays invalid until the welcome arrives; nothing is sent before then.
        localId_ = kInvalidPlayerId;
        return true;
    }

    void Shutdown() {
        if (transport_) transport_->Disconnect();
        transport_.reset();
        role_ = Role::Offline;
        localId_ = kInvalidPlayerId;
        players_ = {};
        peerToPlayer_.clear();
        stateChanges_.clear();
        hostEnemies_.clear();
        remoteEnemies_.clear();
        enemyHits_.clear();
        lastEnemyTick_ = 0;
        tick_ = 0;
        clock_.Reset();
    }

    Role CurrentRole() const { return role_; }
    bool Active() const { return role_ != Role::Offline; }
    PlayerId LocalId() const { return localId_; }

    // Called once per frame. Drains the transport every frame so connection
    // events are handled promptly, but only sends on a net tick.
    void Update(float deltaTime, const LocalPlayerState& local) {
        if (!Active() || !transport_) return;

        transport_->Poll(events_);
        for (Event& event : events_) HandleEvent(event);

        // Remember the local player's own state so the host's snapshot
        // includes it and a client can be told where it thinks it is.
        if (localId_ != kInvalidPlayerId) {
            PlayerSlot& slot = players_[localId_];
            slot.active = true;
            slot.id = localId_;
            slot.current.x = local.x;
            slot.current.y = local.y;
            slot.current.z = local.z;
            slot.current.yaw = local.input.yaw;
            slot.current.pitch = local.input.pitch;
            slot.current.moving = local.input.Moving() ? 1 : 0;
            slot.current.crouching =
                local.input.Held(PlayerInput::Crouch) ? 1 : 0;
            slot.current.sprinting =
                local.input.Held(PlayerInput::Sprint) ? 1 : 0;
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
            // Interpolate between the last two snapshots. Falls back to the
            // newest sample when there is only one, which is what happens on
            // the first tick after a player joins.
            const float span = slot.currentTime - slot.previousTime;
            const float alpha = span > 1e-5f
                ? Clamp01((RenderTime() - slot.previousTime) / span)
                : 1.0f;
            remote.x = Lerp(slot.previous.x, slot.current.x, alpha);
            remote.y = Lerp(slot.previous.y, slot.current.y, alpha);
            remote.z = Lerp(slot.previous.z, slot.current.z, alpha);
            remote.yaw = LerpAngle(slot.previous.yaw, slot.current.yaw, alpha);
            remote.pitch = Lerp(slot.previous.pitch, slot.current.pitch, alpha);
            remote.moving = slot.current.moving != 0;
            remote.crouching = slot.current.crouching != 0;
            remote.sprinting = slot.current.sprinting != 0;
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
    struct PlayerSlot {
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
        case MessageType::ClientEnemyHitReport:
            if (role_ == Role::Host) HandleEnemyHitReport(event);
            break;
        default:
            break;
        }
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
    }

    void HandleWelcome(Event& event) {
        if (event.payload.size() < sizeof(ServerWelcome)) return;
        ServerWelcome welcome{};
        std::memcpy(&welcome, event.payload.data(), sizeof(welcome));
        if (welcome.assignedId >= kMaxPlayers) return;
        localId_ = welcome.assignedId;
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
        slot.hasInput = true;
        slot.current.yaw = message.input.yaw;
        slot.current.pitch = message.input.pitch;
        slot.current.moving = message.input.Moving() ? 1 : 0;
        slot.current.crouching =
            message.input.Held(PlayerInput::Crouch) ? 1 : 0;
        slot.current.sprinting =
            message.input.Held(PlayerInput::Sprint) ? 1 : 0;
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
        for (uint8_t i = 0; i < count; ++i) {
            const PlayerSnapshot& incoming = snapshot.players[i];
            if (incoming.id >= kMaxPlayers) continue;
            PlayerSlot& slot = players_[incoming.id];
            slot.previous = slot.current;
            slot.previousTime = slot.currentTime;
            slot.current = incoming;
            slot.currentTime = interpolationTime_;
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
            ++count;
        }
        snapshot.playerCount = count;
        transport_->Broadcast(&snapshot, sizeof(snapshot), Channel::Unreliable);
    }

    // Per-connection rather than broadcast: "nearest" is a different set for
    // every player, so each client gets its own message.
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
            }
            transport_->Send(peer, &message, sizeof(message),
                             Channel::Unreliable);
        }
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

    void SendInput(const LocalPlayerState& local) {
        if (localId_ == kInvalidPlayerId || serverPeer_ == kInvalidPeer) return;
        ClientInputMessage message;
        message.input = local.input;
        message.x = local.x;
        message.y = local.y;
        message.z = local.z;
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
            PlayerLeftMessage left;
            left.id = id;
            transport_->Broadcast(&left, sizeof(left), Channel::Reliable);
        }
    }

    // Life-state edges waiting to be drained by the game this frame.
    std::vector<PlayerStateChange> stateChanges_;

    // Enemy replication. hostEnemies_ is what the host published this frame;
    // remoteEnemies_ is what a client was last told. Only one is ever populated
    // on a given machine.
    std::vector<HostEnemyState> hostEnemies_;
    std::vector<RemoteEnemy> remoteEnemies_;
    std::vector<EnemyHitRequest> enemyHits_;
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
