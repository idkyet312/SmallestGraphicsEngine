#ifndef NET_SESSION_H
#define NET_SESSION_H

#include "FixedStepClock.h"
#include "NetProtocol.h"
#include "NetTransport.h"

#include <array>
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

// Where a remote player is, already interpolated and ready to drive a body.
struct RemotePlayer {
    PlayerId id = kInvalidPlayerId;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    bool moving = false;
    bool crouching = false;
    bool sprinting = false;
    bool active = false;
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
        }

        clock_.Accumulate(deltaTime);
        float step = 0.0f;
        while (clock_.Consume(step)) {
            ++tick_;
            if (role_ == Role::Host) SendSnapshot();
            else SendInput(local.input);
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

    void HandleEvent(Event& event) {
        switch (event.type) {
        case EventType::Connected:
            // The host waits for a hello before assigning a player id; a
            // client sends one as soon as the connection is up.
            if (role_ == Role::Client) {
                ClientHello hello;
                transport_->Send(event.peer, &hello, sizeof(hello),
                                 Channel::Reliable);
                serverPeer_ = event.peer;
            }
            break;
        case EventType::Disconnected:
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
        default:
            break;
        }
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
            ServerReject reject;
            reject.reason = RejectReason::VersionMismatch;
            transport_->Send(event.peer, &reject, sizeof(reject),
                             Channel::Reliable);
            return;
        }
        const PlayerId assigned = AllocatePlayer(event.peer);
        if (assigned == kInvalidPlayerId) {
            ServerReject reject;
            reject.reason = RejectReason::ServerFull;
            transport_->Send(event.peer, &reject, sizeof(reject),
                             Channel::Reliable);
            return;
        }
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
        if (slot.hasInput && message.input.sequence < slot.lastInput.sequence)
            return;
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
            ++count;
        }
        snapshot.playerCount = count;
        transport_->Broadcast(&snapshot, sizeof(snapshot), Channel::Unreliable);
    }

    void SendInput(const PlayerInput& input) {
        if (localId_ == kInvalidPlayerId || serverPeer_ == kInvalidPeer) return;
        ClientInputMessage message;
        message.input = input;
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
        if (role_ == Role::Host) {
            PlayerLeftMessage left;
            left.id = id;
            transport_->Broadcast(&left, sizeof(left), Channel::Reliable);
        }
    }

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
