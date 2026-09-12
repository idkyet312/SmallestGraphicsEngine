#include "NetProtocol.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <type_traits>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// These structs are written and read by memcpy, so the properties worth pinning
// are the ones that break silently: a type that stops being trivially copyable,
// a header that stops sitting at offset 0, or a field that does not survive the
// round trip. All pure -- no socket, no device.
int main() {
    using namespace net;

    // Every message must stay memcpy-able. A std::string or a virtual slipped
    // into any of them would make the wire format garbage rather than fail to
    // compile at the call site, so assert it here.
    static_assert(std::is_trivially_copyable<PlayerInput>::value,
                  "PlayerInput must stay memcpy-able");
    static_assert(std::is_trivially_copyable<ClientHello>::value,
                  "ClientHello must stay memcpy-able");
    static_assert(std::is_trivially_copyable<ServerWelcome>::value,
                  "ServerWelcome must stay memcpy-able");
    static_assert(std::is_trivially_copyable<ClientInputMessage>::value,
                  "ClientInputMessage must stay memcpy-able");
    static_assert(std::is_trivially_copyable<ServerSnapshotMessage>::value,
                  "ServerSnapshotMessage must stay memcpy-able");

    // Dispatch reads the header before it knows the payload type, so the header
    // must be at offset 0 of every message.
    static_assert(offsetof(ClientHello, header) == 0, "header must lead");
    static_assert(offsetof(ServerWelcome, header) == 0, "header must lead");
    static_assert(offsetof(ServerReject, header) == 0, "header must lead");
    static_assert(offsetof(ClientInputMessage, header) == 0, "header must lead");
    static_assert(offsetof(ServerSnapshotMessage, header) == 0,
                  "header must lead");
    static_assert(offsetof(PlayerJoinedMessage, header) == 0, "header must lead");
    static_assert(offsetof(PlayerLeftMessage, header) == 0, "header must lead");

    // Default-constructed messages must already carry their own type, or a
    // sender that forgets to set it produces a message that reads as something
    // else entirely.
    Check(ClientHello{}.header.type == MessageType::ClientHello,
          "ClientHello has the wrong default type");
    Check(ServerWelcome{}.header.type == MessageType::ServerWelcome,
          "ServerWelcome has the wrong default type");
    Check(ServerReject{}.header.type == MessageType::ServerReject,
          "ServerReject has the wrong default type");
    Check(ClientInputMessage{}.header.type == MessageType::ClientInput,
          "ClientInputMessage has the wrong default type");
    Check(ServerSnapshotMessage{}.header.type == MessageType::ServerSnapshot,
          "ServerSnapshotMessage has the wrong default type");
    Check(PlayerJoinedMessage{}.header.type == MessageType::PlayerJoined,
          "PlayerJoinedMessage has the wrong default type");
    Check(PlayerLeftMessage{}.header.type == MessageType::PlayerLeft,
          "PlayerLeftMessage has the wrong default type");

    // The hello carries the magic and version by default: a peer that sends a
    // default-constructed hello must be accepted by a matching build.
    Check(ClientHello{}.magic == kProtocolMagic, "hello lost its magic");
    Check(ClientHello{}.version == kProtocolVersion, "hello lost its version");

    // A full snapshot through raw bytes, which is what the transport does.
    ServerSnapshotMessage sent;
    sent.tick = 987654u;
    sent.playerCount = kMaxPlayers;
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
        sent.players[i].id = i;
        sent.players[i].x = 1.0f * i;
        sent.players[i].y = 2.0f * i;
        sent.players[i].z = 3.0f * i;
        sent.players[i].yaw = 45.0f * i;
        sent.players[i].pitch = -10.0f * i;
        sent.players[i].moving = static_cast<uint8_t>(i % 2);
        sent.players[i].crouching = static_cast<uint8_t>((i + 1) % 2);
        sent.players[i].sprinting = static_cast<uint8_t>(i % 2);
    }

    unsigned char buffer[sizeof(ServerSnapshotMessage)];
    std::memcpy(buffer, &sent, sizeof(buffer));
    ServerSnapshotMessage received;
    std::memcpy(&received, buffer, sizeof(buffer));

    Check(received.header.type == MessageType::ServerSnapshot,
          "snapshot type did not round-trip");
    Check(received.tick == sent.tick, "snapshot tick did not round-trip");
    Check(received.playerCount == sent.playerCount,
          "snapshot playerCount did not round-trip");
    for (uint8_t i = 0; i < kMaxPlayers; ++i) {
        Check(received.players[i].id == sent.players[i].id, "id lost");
        Check(received.players[i].x == sent.players[i].x, "x lost");
        Check(received.players[i].y == sent.players[i].y, "y lost");
        Check(received.players[i].z == sent.players[i].z, "z lost");
        Check(received.players[i].yaw == sent.players[i].yaw, "yaw lost");
        Check(received.players[i].pitch == sent.players[i].pitch, "pitch lost");
        Check(received.players[i].moving == sent.players[i].moving,
              "moving lost");
        Check(received.players[i].crouching == sent.players[i].crouching,
              "crouching lost");
        Check(received.players[i].sprinting == sent.players[i].sprinting,
              "sprinting lost");
    }

    // An input message must carry the PlayerInput through untouched, since the
    // host feeds it straight back into Camera::ApplyInput.
    ClientInputMessage inputSent;
    inputSent.input.forward = -1.0f;
    inputSent.input.strafe = 0.25f;
    inputSent.input.yaw = 270.0f;
    inputSent.input.sequence = 77u;
    inputSent.input.Set(PlayerInput::Crouch, true);

    unsigned char inputBuffer[sizeof(ClientInputMessage)];
    std::memcpy(inputBuffer, &inputSent, sizeof(inputBuffer));
    ClientInputMessage inputReceived;
    std::memcpy(&inputReceived, inputBuffer, sizeof(inputBuffer));

    Check(inputReceived.input.forward == inputSent.input.forward,
          "input forward did not round-trip");
    Check(inputReceived.input.strafe == inputSent.input.strafe,
          "input strafe did not round-trip");
    Check(inputReceived.input.yaw == inputSent.input.yaw,
          "input yaw did not round-trip");
    Check(inputReceived.input.sequence == inputSent.input.sequence,
          "input sequence did not round-trip");
    Check(inputReceived.input.Held(PlayerInput::Crouch),
          "input buttons did not round-trip");

    // A snapshot must fit comfortably in a normal MTU, or every send fragments.
    Check(sizeof(ServerSnapshotMessage) < 1200,
          "snapshot is too large for a single unfragmented datagram");

    std::cout << "NetProtocol tests passed ("
              << sizeof(ServerSnapshotMessage) << " byte snapshot)\n";
    return 0;
}
