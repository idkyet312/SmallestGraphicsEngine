// The host's map is session state: a client that joins has to be told which
// level to load, or two players share coordinates on terrain only one of them
// has. These exercise that seam against a fake transport -- what the host puts
// on the wire, and what a client accepts off it.
#include "NetSession.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {
struct WireMessage {
    net::PeerId peer;
    net::Channel channel;
    std::vector<uint8_t> bytes;
};
std::vector<net::Event> inbox;
std::vector<WireMessage> wire;

void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
template<class T> void Receive(net::PeerId peer, const T& message) {
    net::Event event;
    event.peer = peer;
    event.payload.resize(sizeof(message));
    std::memcpy(event.payload.data(), &message, sizeof(message));
    inbox.push_back(std::move(event));
}
class TestTransport final : public net::NetTransport {
public:
    bool Listen(uint16_t, std::string*) override { return true; }
    bool Connect(const std::string&, uint16_t, std::string*) override { return true; }
    void Disconnect() override {}
    void Poll(std::vector<net::Event>& events) override {
        events.clear(); events.swap(inbox);
    }
    bool Send(net::PeerId peer, const void* data, uint32_t bytes,
              net::Channel channel) override {
        WireMessage message{peer, channel, std::vector<uint8_t>(bytes)};
        std::memcpy(message.bytes.data(), data, bytes);
        wire.push_back(std::move(message));
        return true;
    }
    void Broadcast(const void* data, uint32_t bytes, net::Channel channel,
                   net::PeerId = net::kInvalidPeer) override {
        Send(net::kInvalidPeer, data, bytes, channel);
    }
    bool IsActive() const override { return true; }
};

// The last level message on the wire, or false when there is none after `from`.
bool LastLevel(size_t from, net::ServerLevelMessage& out) {
    bool found = false;
    for (size_t i = from; i < wire.size(); ++i) {
        if (wire[i].bytes.size() != sizeof(net::ServerLevelMessage)) continue;
        net::MessageHeader header{};
        std::memcpy(&header, wire[i].bytes.data(), sizeof(header));
        if (header.type != net::MessageType::ServerLevel) continue;
        std::memcpy(&out, wire[i].bytes.data(), sizeof(out));
        found = true;
    }
    return found;
}
}
namespace net {
std::unique_ptr<NetTransport> MakeNullTransport() {
    return std::make_unique<TestTransport>();
}
}

int main() {
    using namespace net;
    LocalPlayerState local;

    NetSession host;
    Check(host.StartHost(27120, nullptr), "host failed");
    host.SetHostLevel(LevelKind::LevelFile, "Base.json");

    // A player joining a session already in progress must be told the map as
    // part of being let in -- not on the next change, which may never come.
    const size_t beforeJoin = wire.size();
    ClientHello hello;
    Receive(9, hello);
    host.Update(1.0f / 30.0f, local);
    ServerLevelMessage sent{};
    Check(LastLevel(beforeJoin, sent), "a joining peer must be told the level");
    Check(sent.kind == LevelKind::LevelFile &&
          std::strcmp(sent.file, "Base.json") == 0,
          "the join answer must name the host's level");

    // Re-stating the same level is not a change, and re-broadcasting it would
    // have every client reload the map it is already standing on.
    const size_t beforeRepeat = wire.size();
    host.SetHostLevel(LevelKind::LevelFile, "Base.json");
    ServerLevelMessage ignored{};
    Check(!LastLevel(beforeRepeat, ignored),
          "repeating the same level must not go on the wire");

    const size_t beforeChange = wire.size();
    host.SetHostLevel(LevelKind::TestLevel, "");
    ServerLevelMessage changed{};
    Check(LastLevel(beforeChange, changed), "a level change must be sent");
    Check(changed.kind == LevelKind::TestLevel && changed.file[0] == '\0',
          "the test level carries no file name");
    Check(wire.back().channel == Channel::Reliable,
          "a level change is reliable; a dropped one strands a client");

    // A client only follows the server it is connected to, and only a bare file
    // name: a path from the network points this machine wherever the sender
    // likes.
    NetSession client;
    Check(client.StartClient("host", 27120, nullptr), "client failed");
    ServerWelcome welcome;
    welcome.assignedId = 1;
    Receive(3, welcome);
    client.Update(0.0f, local);

    LevelKind kind = LevelKind::None;
    std::string file;
    Check(!client.TakePendingLevel(kind, file),
          "a client with no level message must have nothing pending");

    ServerLevelMessage level;
    level.kind = LevelKind::LevelFile;
    std::strcpy(level.file, "Islandv10.json");
    Receive(3, level);
    client.Update(0.0f, local);
    Check(client.TakePendingLevel(kind, file), "the level must be pending");
    Check(kind == LevelKind::LevelFile && file == "Islandv10.json",
          "the client must read the level it was sent");
    Check(!client.TakePendingLevel(kind, file),
          "taking the level twice would start the load twice");

    // A load already running is why this exists: the caller puts the one-shot
    // back rather than dropping the level it could not act on.
    client.RequeuePendingLevel();
    Check(client.TakePendingLevel(kind, file) && file == "Islandv10.json",
          "a requeued level must come back intact");

    ServerLevelMessage same = level;
    Receive(3, same);
    client.Update(0.0f, local);
    Check(!client.TakePendingLevel(kind, file),
          "being told the level it is already on must not reload it");

    ServerLevelMessage traversal;
    traversal.kind = LevelKind::LevelFile;
    std::strcpy(traversal.file, "../../Windows/System32/config.json");
    Receive(3, traversal);
    client.Update(0.0f, local);
    Check(!client.TakePendingLevel(kind, file),
          "a path, rather than a file name, must be refused");

    ServerLevelMessage spoof;
    spoof.kind = LevelKind::Level1;
    Receive(999, spoof);
    client.Update(0.0f, local);
    Check(!client.TakePendingLevel(kind, file),
          "only the server may name the level");

    // An unterminated field is what a hostile or broken sender produces; the
    // reader must not walk off the end of it.
    ServerLevelMessage unterminated;
    unterminated.kind = LevelKind::LevelFile;
    std::memset(unterminated.file, 'a', sizeof(unterminated.file));
    Receive(3, unterminated);
    client.Update(0.0f, local);
    Check(client.TakePendingLevel(kind, file) &&
          file.size() == kMaxLevelFileName - 1,
          "an unterminated file name must be clamped, not overrun");

    ServerLevelMessage bogus;
    bogus.kind = static_cast<LevelKind>(42);
    Receive(3, bogus);
    client.Update(0.0f, local);
    Check(!client.TakePendingLevel(kind, file),
          "an unknown level kind must be ignored");

    // Each player's insertion travels through the host, independently of
    // their feet position or the other player's airframe choice.
    local.helicopter.visible = 1;
    local.helicopter.airframe = 1;
    local.helicopter.x = 120.0f;
    local.helicopter.y = 35.0f;
    local.helicopter.yaw = 350.0f;
    local.helicopter.scale = 2.0f;
    ClientInputMessage insertion;
    insertion.input.sequence = 1;
    insertion.helicopter.visible = 1;
    insertion.helicopter.airframe = 0;
    insertion.helicopter.x = -80.0f;
    insertion.helicopter.y = 42.0f;
    insertion.helicopter.roll = 12.0f;
    Receive(9, insertion);
    wire.clear();
    host.Update(1.0f / 30.0f, local);
    std::vector<RemotePlayer> remotes;
    host.GetRemotePlayers(remotes);
    Check(remotes.size() == 1 && remotes[0].helicopter.visible &&
          remotes[0].helicopter.airframe == 0 &&
          remotes[0].helicopter.x == -80.0f &&
          remotes[0].helicopter.roll == 12.0f,
          "host must see the client's helicopter pose and chosen model");

    const auto lastPlayerSnapshot = []() {
        ServerSnapshotMessage result;
        bool found = false;
        for (const auto& packet : wire) {
            if (packet.bytes.size() != sizeof(result)) continue;
            MessageHeader header;
            std::memcpy(&header, packet.bytes.data(), sizeof(header));
            if (header.type != MessageType::ServerSnapshot) continue;
            std::memcpy(&result, packet.bytes.data(), sizeof(result));
            found = true;
        }
        Check(found, "host must send a player snapshot");
        return result;
    };
    const auto flight = lastPlayerSnapshot();
    Check(flight.playerCount == 2 && flight.players[0].helicopter.visible &&
          flight.players[1].helicopter.visible,
          "both helicopters must coexist in the same snapshot");
    Receive(3, flight);
    client.Update(0.0f, {});
    client.GetRemotePlayers(remotes);
    Check(remotes.size() == 1 && remotes[0].helicopter.visible &&
          remotes[0].helicopter.airframe == 1 &&
          remotes[0].helicopter.x == 120.0f &&
          remotes[0].helicopter.scale == 2.0f,
          "client must see the host's helicopter without duplicating its own");

    // Invalid poses must not reach rendering. A newer hidden state must also
    // remove a departed helicopter even if a late flight packet follows it.
    insertion.input.sequence = 2;
    insertion.helicopter.x = std::numeric_limits<float>::quiet_NaN();
    Receive(9, insertion);
    local.helicopter = {};
    wire.clear();
    host.Update(1.0f / 30.0f, local);
    host.GetRemotePlayers(remotes);
    Check(!remotes[0].helicopter.visible, "non-finite helicopter pose must be hidden");
    Receive(3, lastPlayerSnapshot());
    client.Update(0.0f, {});
    Receive(3, flight);
    client.Update(0.0f, {});
    client.GetRemotePlayers(remotes);
    Check(!remotes[0].helicopter.visible,
          "an old snapshot must not resurrect a departed helicopter");

    // A 42 m/s flight sampled at 30 Hz must still move at 42 m/s on every
    // 60 Hz render frame after the interpolation buffer fills. Keeping only
    // two samples with a two-tick delay used to alternate holds and jumps.
    NetSession watcher;
    Check(watcher.StartClient("host", 27120, nullptr), "watcher failed");
    Receive(3, welcome);
    watcher.Update(0.0f, {});
    constexpr float frameTime = 1.0f / 60.0f;
    float lastFlightX = 0.0f;
    ServerSnapshotMessage movingFlight;
    movingFlight.playerCount = 1;
    movingFlight.players[0].id = 0;
    movingFlight.players[0].helicopter.visible = 1;
    for (int frame = 0; frame < 120; ++frame) {
        if (frame % 2 == 0) {
            ++movingFlight.tick;
            auto& pilot = movingFlight.players[0];
            pilot.helicopter.x = 42.0f * frame * frameTime;
            pilot.x = pilot.helicopter.x + 2.0f;
            pilot.helicopter.yaw = std::fmod(350.0f + frame * 2.0f, 360.0f);
            Receive(3, movingFlight);
        }
        watcher.Update(frameTime, {});
        watcher.GetRemotePlayers(remotes);
        Check(remotes.size() == 1, "watcher must retain the pilot");
        const float flightX = remotes[0].helicopter.x;
        if (frame >= 8) {
            const float expected = 42.0f *
                ((frame + 1) * frameTime - NetSession::kInterpolationDelay);
            Check(std::abs(flightX - expected) < 0.001f,
                  "remote helicopter must follow the delayed flight timeline");
            Check(std::abs(flightX - lastFlightX - 42.0f * frameTime) < 0.001f,
                  "remote helicopter must move between packet arrivals");
            Check(std::abs(remotes[0].x - flightX - 2.0f) < 0.001f,
                  "remote pilot must stay aligned with the helicopter");
        }
        lastFlightX = flightX;
    }
    watcher.Update(0.5f, {});
    watcher.GetRemotePlayers(remotes);
    Check(remotes[0].helicopter.x == movingFlight.players[0].helicopter.x,
          "a stalled connection must not extrapolate the helicopter indefinitely");
}
