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
inline constexpr uint32_t kProtocolVersion = 1;

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

// One player's replicated state. Position and angles only: health, weapons and
// ammo are milestone 2, and PlayerState already holds them locally.
struct PlayerSnapshot {
    PlayerId id = kInvalidPlayerId;
    // Packed movement state so the remote body can pick a clip without the
    // receiver re-deriving it from position deltas.
    uint8_t moving = 0;
    uint8_t crouching = 0;
    uint8_t sprinting = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
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

} // namespace net

#endif // NET_PROTOCOL_H
