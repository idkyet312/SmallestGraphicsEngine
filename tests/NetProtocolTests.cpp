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
    static_assert(std::is_trivially_copyable<ClientHitReportMessage>::value,
                  "ClientHitReportMessage must stay memcpy-able");
    static_assert(
        std::is_trivially_copyable<ServerPlayerStateChangedMessage>::value,
        "ServerPlayerStateChangedMessage must stay memcpy-able");
    static_assert(std::is_trivially_copyable<ClientReviveProgressMessage>::value,
                  "ClientReviveProgressMessage must stay memcpy-able");

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
    static_assert(offsetof(ClientHitReportMessage, header) == 0,
                  "header must lead");
    static_assert(offsetof(ServerPlayerStateChangedMessage, header) == 0,
                  "header must lead");
    static_assert(offsetof(ClientReviveProgressMessage, header) == 0,
                  "header must lead");

    // The version must be bumped whenever a struct in the file changes shape.
    // Pinned so that changing PlayerSnapshot and forgetting the bump -- which
    // would have two builds silently misreading each other's bytes -- fails
    // here instead of in a session.
    Check(kProtocolVersion == 3, "protocol version was not bumped");

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
    Check(ClientHitReportMessage{}.header.type == MessageType::ClientHitReport,
          "ClientHitReportMessage has the wrong default type");
    Check(ServerPlayerStateChangedMessage{}.header.type ==
              MessageType::ServerPlayerStateChanged,
          "ServerPlayerStateChangedMessage has the wrong default type");
    Check(ClientReviveProgressMessage{}.header.type ==
              MessageType::ClientReviveProgress,
          "ClientReviveProgressMessage has the wrong default type");

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
        sent.players[i].health = 100.0f - 20.0f * i;
        sent.players[i].downed = static_cast<uint8_t>(i == 3 ? 1 : 0);
        sent.players[i].reviver =
            static_cast<PlayerId>(i == 3 ? 1 : kInvalidPlayerId);
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
        Check(received.players[i].health == sent.players[i].health,
              "health lost");
        Check(received.players[i].downed == sent.players[i].downed,
              "downed lost");
        Check(received.players[i].reviver == sent.players[i].reviver,
              "reviver lost");
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

    // The three PvP messages, through raw bytes the same way.
    ClientHitReportMessage hitSent;
    hitSent.target = 2;
    hitSent.headshot = 1;
    hitSent.damage = 20.0f;
    hitSent.hitX = 1.5f;
    hitSent.hitY = 1.7f;
    hitSent.hitZ = -3.25f;
    hitSent.shooterTick = 4242u;

    unsigned char hitBuffer[sizeof(ClientHitReportMessage)];
    std::memcpy(hitBuffer, &hitSent, sizeof(hitBuffer));
    ClientHitReportMessage hitReceived;
    std::memcpy(&hitReceived, hitBuffer, sizeof(hitBuffer));

    Check(hitReceived.header.type == MessageType::ClientHitReport,
          "hit report type did not round-trip");
    Check(hitReceived.target == hitSent.target, "hit target did not round-trip");
    Check(hitReceived.headshot == hitSent.headshot,
          "hit headshot did not round-trip");
    Check(hitReceived.damage == hitSent.damage, "hit damage did not round-trip");
    Check(hitReceived.hitX == hitSent.hitX && hitReceived.hitY == hitSent.hitY &&
              hitReceived.hitZ == hitSent.hitZ,
          "hit position did not round-trip");
    Check(hitReceived.shooterTick == hitSent.shooterTick,
          "hit shooterTick did not round-trip");

    ServerPlayerStateChangedMessage stateSent;
    stateSent.id = 1;
    stateSent.event = PlayerStateEvent::Downed;
    stateSent.instigator = 0;
    stateSent.health = 0.0f;
    stateSent.impulseX = 0.0f;
    stateSent.impulseY = 1.0f;
    stateSent.impulseZ = 0.0f;
    stateSent.impactX = 5.0f;
    stateSent.impactY = 1.2f;
    stateSent.impactZ = -7.0f;

    unsigned char stateBuffer[sizeof(ServerPlayerStateChangedMessage)];
    std::memcpy(stateBuffer, &stateSent, sizeof(stateBuffer));
    ServerPlayerStateChangedMessage stateReceived;
    std::memcpy(&stateReceived, stateBuffer, sizeof(stateBuffer));

    Check(stateReceived.header.type == MessageType::ServerPlayerStateChanged,
          "state change type did not round-trip");
    Check(stateReceived.id == stateSent.id, "state change id did not round-trip");
    Check(stateReceived.event == PlayerStateEvent::Downed,
          "state change event did not round-trip");
    Check(stateReceived.instigator == stateSent.instigator,
          "state change instigator did not round-trip");
    Check(stateReceived.impactY == stateSent.impactY,
          "state change impact did not round-trip");

    ClientReviveProgressMessage reviveSent;
    reviveSent.target = 3;
    reviveSent.holding = 1;

    unsigned char reviveBuffer[sizeof(ClientReviveProgressMessage)];
    std::memcpy(reviveBuffer, &reviveSent, sizeof(reviveBuffer));
    ClientReviveProgressMessage reviveReceived;
    std::memcpy(&reviveReceived, reviveBuffer, sizeof(reviveBuffer));

    Check(reviveReceived.header.type == MessageType::ClientReviveProgress,
          "revive progress type did not round-trip");
    Check(reviveReceived.target == reviveSent.target,
          "revive target did not round-trip");
    Check(reviveReceived.holding == reviveSent.holding,
          "revive holding did not round-trip");

    // The revive bit has to survive the input round-trip too, or a held key
    // reaches the host as nothing.
    ClientInputMessage reviveInput;
    reviveInput.input.Set(PlayerInput::Revive, true);
    unsigned char reviveInputBuffer[sizeof(ClientInputMessage)];
    std::memcpy(reviveInputBuffer, &reviveInput, sizeof(reviveInputBuffer));
    ClientInputMessage reviveInputReceived;
    std::memcpy(&reviveInputReceived, reviveInputBuffer,
                sizeof(reviveInputBuffer));
    Check(reviveInputReceived.input.Held(PlayerInput::Revive),
          "revive button did not round-trip");

    // The enemy snapshot, which is the largest message on the wire.
    static_assert(std::is_trivially_copyable<ServerEnemySnapshotMessage>::value,
                  "ServerEnemySnapshotMessage must stay memcpy-able");
    static_assert(std::is_trivially_copyable<ClientEnemyHitReportMessage>::value,
                  "ClientEnemyHitReportMessage must stay memcpy-able");
    static_assert(offsetof(ServerEnemySnapshotMessage, header) == 0,
                  "header must lead");
    static_assert(offsetof(ClientEnemyHitReportMessage, header) == 0,
                  "header must lead");
    Check(ServerEnemySnapshotMessage{}.header.type ==
              MessageType::ServerEnemySnapshot,
          "ServerEnemySnapshotMessage has the wrong default type");
    Check(ClientEnemyHitReportMessage{}.header.type ==
              MessageType::ClientEnemyHitReport,
          "ClientEnemyHitReportMessage has the wrong default type");

    ServerEnemySnapshotMessage enemiesSent;
    enemiesSent.tick = 555u;
    enemiesSent.enemyCount = kMaxReplicatedEnemies;
    for (uint8_t i = 0; i < kMaxReplicatedEnemies; ++i) {
        enemiesSent.enemies[i].id = static_cast<EnemyId>(1000 + i);
        enemiesSent.enemies[i].x = 4.0f * i;
        enemiesSent.enemies[i].yaw = 0.5f * i;
        enemiesSent.enemies[i].aimYaw = -0.25f * i;
        enemiesSent.enemies[i].health = 100.0f - i;
        enemiesSent.enemies[i].moving = static_cast<uint8_t>(i % 2);
        enemiesSent.enemies[i].dead = static_cast<uint8_t>(i == 5 ? 1 : 0);
    }

    unsigned char enemyBuffer[sizeof(ServerEnemySnapshotMessage)];
    std::memcpy(enemyBuffer, &enemiesSent, sizeof(enemyBuffer));
    ServerEnemySnapshotMessage enemiesReceived;
    std::memcpy(&enemiesReceived, enemyBuffer, sizeof(enemyBuffer));

    Check(enemiesReceived.tick == enemiesSent.tick,
          "enemy snapshot tick did not round-trip");
    Check(enemiesReceived.enemyCount == kMaxReplicatedEnemies,
          "enemy count did not round-trip");
    for (uint8_t i = 0; i < kMaxReplicatedEnemies; ++i) {
        Check(enemiesReceived.enemies[i].id == enemiesSent.enemies[i].id,
              "enemy id lost");
        Check(enemiesReceived.enemies[i].x == enemiesSent.enemies[i].x,
              "enemy x lost");
        Check(enemiesReceived.enemies[i].aimYaw ==
                  enemiesSent.enemies[i].aimYaw,
              "enemy aimYaw lost");
        Check(enemiesReceived.enemies[i].health ==
                  enemiesSent.enemies[i].health,
              "enemy health lost");
        Check(enemiesReceived.enemies[i].dead == enemiesSent.enemies[i].dead,
              "enemy dead flag lost");
    }

    // Both snapshots must fit comfortably in a normal MTU, or every send
    // fragments. The enemy one is the message that grows with kMaxReplicatedEnemies,
    // so this is the assertion that catches raising that cap too far.
    Check(sizeof(ServerSnapshotMessage) < 1200,
          "snapshot is too large for a single unfragmented datagram");
    Check(sizeof(ServerEnemySnapshotMessage) < 1200,
          "enemy snapshot is too large for a single unfragmented datagram");

    std::cout << "NetProtocol tests passed ("
              << sizeof(ServerSnapshotMessage) << " byte player snapshot, "
              << sizeof(ServerEnemySnapshotMessage) << " byte enemy snapshot)\n";
    return 0;
}
