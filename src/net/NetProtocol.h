#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include "PlayerInput.h"

#include <cstdint>

// Wire format for milestone 1: connect, move, see each other. Nothing here
// describes shooting, AI, vehicles or destruction -- those are later milestones
// and adding their fields now would be guessing at a format before the traffic
// that shapes it exists.
//
// Every struct is a fixed-width POD written and read by memcpy. No pointers, no
// std types, no virtuals. Endianness is deliberately ignored: both ends are
// x86-64 Windows builds of the same engine, and the version handshake below
// rejects anything else before a single field is read.
namespace net {

// Bumped whenever any struct in this file changes shape. A mismatch is refused
// at connect time with a clear message -- silently misreading a peer's bytes is
// far worse to debug than being told the builds differ.
// 2: PlayerSnapshot gained health/downed/reviver, and PvP hit reporting,
// player state events and revive progress were added.
inline constexpr uint32_t kProtocolVersion = 2;

// A magic word in the hello guards against something other than this game
// connecting to the port and having its bytes read as a handshake.
inline constexpr uint32_t kProtocolMagic = 0x53474531u; // "SGE1"

inline constexpr uint8_t kMaxPlayers = 4;
using PlayerId = uint8_t;
inline constexpr PlayerId kInvalidPlayerId = 0xFF;

enum class MessageType : uint8_t {
    ClientHello = 1,   // client -> host, reliable
    ServerWelcome,     // host -> client, reliable
    ServerReject,      // host -> client, reliable (then disconnect)
    ClientInput,       // client -> host, unreliable, every net tick
    ServerSnapshot,    // host -> client, unreliable, every net tick
    PlayerJoined,      // host -> client, reliable
    PlayerLeft,        // host -> client, reliable
    ClientHitReport,   // client -> host, reliable
    ServerPlayerStateChanged, // host -> client, reliable
    ClientReviveProgress,     // client -> host, unreliable
};

// One-shot transitions in a player's life state. Carried by a reliable message
// rather than inferred from the snapshot, because each one drives an effect
// that must happen exactly once: the pose change, the audio, the callout.
enum class PlayerStateEvent : uint8_t {
    Downed = 1,
    Revived,
    // Reserved so adding respawn later costs no version bump.
    Respawned,
};

enum class RejectReason : uint8_t {
    VersionMismatch = 1,
    ServerFull,
};

// Every message starts with this so the receiver can dispatch before it knows
// the payload shape.
struct MessageHeader {
    MessageType type = MessageType::ClientHello;
    uint8_t padding[3] = {};
};

struct ClientHello {
    MessageHeader header{ MessageType::ClientHello, {} };
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
};

struct ServerWelcome {
    MessageHeader header{ MessageType::ServerWelcome, {} };
    uint32_t version = kProtocolVersion;
    PlayerId assignedId = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

struct ServerReject {
    MessageHeader header{ MessageType::ServerReject, {} };
    RejectReason reason = RejectReason::VersionMismatch;
    uint8_t padding[3] = {};
    uint32_t serverVersion = kProtocolVersion;
};

struct ClientInputMessage {
    MessageHeader header{ MessageType::ClientInput, {} };
    PlayerInput input;
};

// One player's replicated state. Weapons and ammo are still local-only; health
// and the downed flag are here because they are what everyone has to agree on.
//
// This is the authoritative copy. Whatever a snapshot says is true, and because
// it arrives every tick it self-heals a client that missed an event. The
// reliable ServerPlayerStateChanged below carries only the *edge* and never
// writes state, which is what stops the two from ever disagreeing.
struct PlayerSnapshot {
    PlayerId id = kInvalidPlayerId;
    // Packed movement state so the remote body can pick a clip without the
    // receiver re-deriving it from position deltas.
    uint8_t moving = 0;
    uint8_t crouching = 0;
    uint8_t sprinting = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    float health = 100.0f;
    // Not derived from health <= 0 on the receiver: a revived player sits at
    // partial health and must not read as downed, and a downed player sits at
    // exactly 0 for as long as it takes someone to reach them.
    uint8_t downed = 0;
    // Who is currently reviving this player, or kInvalidPlayerId. Replicated so
    // every client can draw the progress, not just the two players involved.
    PlayerId reviver = kInvalidPlayerId;
    uint8_t padding[2] = {};
};

struct ServerSnapshotMessage {
    MessageHeader header{ MessageType::ServerSnapshot, {} };
    // Host tick this snapshot describes. The client interpolates between the
    // two most recent and discards anything older than what it already has,
    // which is what makes out-of-order UDP delivery harmless.
    uint32_t tick = 0;
    uint8_t playerCount = 0;
    uint8_t padding[3] = {};
    PlayerSnapshot players[kMaxPlayers];
};

struct PlayerJoinedMessage {
    MessageHeader header{ MessageType::PlayerJoined, {} };
    PlayerId id = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

struct PlayerLeftMessage {
    MessageHeader header{ MessageType::PlayerLeft, {} };
    PlayerId id = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

// Shooter-authoritative hit. The client that fired ran the geometry test
// locally, against the body it could actually see on its own screen, and
// reports the result rather than mutating anything itself.
//
// Reliable: a dropped hit is a round that silently did nothing. The shooter saw
// blood and a hit marker while the victim took no damage, which reads as the
// game being broken rather than as packet loss.
struct ClientHitReportMessage {
    MessageHeader header{ MessageType::ClientHitReport, {} };
    PlayerId target = kInvalidPlayerId;
    uint8_t headshot = 0;
    uint8_t padding[2] = {};
    float damage = 0.0f;
    // Where the round landed, for the victim's directional hit indicator.
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    // The shooter's tick when it fired. Unused today; carried so that adding
    // lag compensation later -- rewinding the world to what the shooter saw --
    // needs no protocol bump.
    uint32_t shooterTick = 0;
};

// The edge of a life-state transition, not the state itself. Delivered reliably
// so the pose change, the ragdoll-equivalent and the audio each happen exactly
// once on every client, including one that dropped the snapshot the flip
// happened on.
struct ServerPlayerStateChangedMessage {
    MessageHeader header{ MessageType::ServerPlayerStateChanged, {} };
    PlayerId id = kInvalidPlayerId;
    PlayerStateEvent event = PlayerStateEvent::Downed;
    // Who put them down or picked them up. kInvalidPlayerId when nobody did --
    // a fall, a bandit, or their own grenade.
    PlayerId instigator = kInvalidPlayerId;
    uint8_t padding = 0;
    float health = 0.0f;
    // Direction the felling round travelled, so the downed body settles the
    // same way on every client rather than differently on each.
    float impulseX = 0.0f, impulseY = 0.0f, impulseZ = 0.0f;
    float impactX = 0.0f, impactY = 0.0f, impactZ = 0.0f;
};

// Sent every tick while the reviver holds the key in range. Unreliable and
// idempotent: losing one costs a single tick of progress, and the host re-checks
// the distance itself rather than trusting this to have stopped arriving.
struct ClientReviveProgressMessage {
    MessageHeader header{ MessageType::ClientReviveProgress, {} };
    PlayerId target = kInvalidPlayerId;
    uint8_t holding = 0;
    uint8_t padding[2] = {};
};

} // namespace net

#endif // NET_PROTOCOL_H
