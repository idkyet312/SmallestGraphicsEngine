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
    Check(host.StartHost(27110, nullptr), "host failed");
    // Establish peer 1 so request authority is exercised through the real map.
    ClientHello hello;
    Receive(7, hello);
    host.Update(1.0f / 30.0f, local);
    // A second authenticated peer proves impact identity is per request, not
    // accidentally keyed only by the entity.
    Receive(8, hello);
    host.Update(0.0f, local);
    ClientWorldImpactMessage impact;
    impact.impactId = 1;
    impact.entityId = 42; impact.damage = 3.5f; impact.hitX = 1.0f;
    Receive(7, impact);
    impact.entityId = 43; impact.hitX = 2.0f;
    Receive(8, impact);
    host.Update(0.0f, local);
    std::vector<WorldImpactRequest> impacts;
    host.DrainWorldImpacts(impacts);
    Check(impacts.size() == 2 && impacts[0].entityId == 42 &&
          impacts[0].shooter == 1 && impacts[1].entityId == 43 &&
          impacts[1].shooter == 2,
          "host must relay authenticated impacts from both peers");
    // Surface impacts may use no authored entity; the session supplies an id.
    impact = {};
    impact.kind = 0; impact.entityId = 0; impact.damage = 1.0f;
    Receive(7, impact);
    host.Update(0.0f, local);
    host.DrainWorldImpacts(impacts);
    Check(impacts.size() == 1 && impacts[0].impactId != 0 &&
          impacts[0].kind == 0, "surface impact must receive an automatic id");
    impact.kind = 1; impact.entityId = 0;
    Receive(7, impact);
    host.Update(0.0f, local);
    host.DrainWorldImpacts(impacts);
    Check(impacts.empty(), "malformed world impact must be rejected");
    ClientWorldImpactMessage spoof = {};
    spoof.entityId = 99; spoof.damage = 1.0f;
    Receive(999, spoof);
    host.Update(0.0f, local);
    host.DrainWorldImpacts(impacts);
    Check(impacts.empty(), "unknown peer must not author world damage");

    // Indexed from here rather than read twice off the back: both publishes
    // have to be inspected, and wire.back() is only ever the second one.
    const size_t firstBreakIndex = wire.size();
    host.PublishWorldBreak(42, 1.0f, 2.0f, 3.0f);
    host.PublishWorldBreak(43, 2.0f, 2.0f, 3.0f);
    Check(wire.size() == firstBreakIndex + 2, "both host breaks must be sent");
    Check(wire.back().channel == Channel::Reliable &&
          wire.back().bytes.size() == sizeof(ServerWorldBreakMessage),
          "world break must use reliable wire format");
    ServerWorldBreakMessage breakMessage;
    std::memcpy(&breakMessage, wire[firstBreakIndex].bytes.data(),
                sizeof(breakMessage));
    ServerWorldBreakMessage secondBreak;
    std::memcpy(&secondBreak, wire[firstBreakIndex + 1].bytes.data(),
                sizeof(secondBreak));

    NetSession client;
    Check(client.StartClient("host", 27110, nullptr), "client failed");
    ServerWelcome welcome;
    welcome.assignedId = 1;
    Receive(3, welcome);
    Receive(3, breakMessage);
    Receive(3, secondBreak);
    client.Update(0.0f, local);
    std::vector<WorldBreakEvent> breaks;
    client.DrainWorldBreaks(breaks);
    Check(breaks.size() == 2 && breaks[0].entityId == 42 &&
          breaks[1].entityId == 43 &&
          breaks[0].impactId != breaks[1].impactId,
          "committed world breaks need unique server ids");
    // The shooter has to survive the round trip: it is the only thing that
    // tells a receiver whether to draw the impact or leave it to the machine
    // that already drew it when it fired.
    const size_t attributedBreakIndex = wire.size();
    host.PublishWorldBreak(0, 0, 3.0f, 1.0f, 5.0f, 7.0f, 8.0f, 9.0f,
                           1.0f, 0.0f, 0.0f, /*shooter=*/2);
    Check(wire.size() == attributedBreakIndex + 1,
          "surface break must be sent");
    ServerWorldBreakMessage attributed;
    std::memcpy(&attributed, wire[attributedBreakIndex].bytes.data(),
                sizeof(attributed));
    Check(attributed.shooter == 2, "break must name its shooter on the wire");
    Receive(3, attributed);
    client.Update(0.0f, local);
    client.DrainWorldBreaks(breaks);
    Check(breaks.size() == 1 && breaks[0].shooter == 2 && breaks[0].kind == 0,
          "client must read the shooter off a committed break");
    Receive(3, breakMessage); // duplicate arriving on a later frame
    client.Update(0.0f, local);
    client.DrainWorldBreaks(breaks);
    Check(breaks.empty(), "delayed duplicate world break must be ignored");
    secondBreak.entityId = 44; secondBreak.impactId += 1000;
    Receive(999, secondBreak);
    client.Update(0.0f, local);
    client.DrainWorldBreaks(breaks);
    Check(breaks.empty(), "wrong server must not author world breaks");
    std::vector<uint8_t> truncated(sizeof(ServerWorldBreakMessage) - 1);
    net::Event malformed;
    malformed.peer = 3;
    malformed.payload = truncated;
    inbox.push_back(std::move(malformed));
    client.Update(0.0f, local);
    client.DrainWorldBreaks(breaks);
    Check(breaks.empty(), "truncated world break must be rejected");

    // A client throw is one request/token, even if reliable delivery repeats.
    ClientGrenadeThrowMessage throwMessage;
    throwMessage.clientToken = 77;
    throwMessage.kind = GrenadeKind::Frag;
    throwMessage.x = 4.0f; throwMessage.velocityZ = 8.0f; throwMessage.fuse = 2.0f;
    Receive(7, throwMessage);
    Receive(7, throwMessage);
    host.Update(0.0f, local);
    std::vector<GrenadeSpawnEvent> spawns;
    host.DrainGrenadeSpawns(spawns);
    Check(spawns.size() == 1 && spawns[0].clientToken == 77 &&
          spawns[0].owner == 1 && spawns[0].grenadeId != 0,
          "duplicate grenade request must produce one canonical spawn");
    // Held now: the next drain clears the vector, so reading spawns[0] after it
    // would be a read off an empty vector, not a stale-but-valid id.
    const uint32_t grenadeId = spawns[0].grenadeId;
    Receive(7, throwMessage); // retransmit after the first request was drained
    host.Update(0.0f, local);
    host.DrainGrenadeSpawns(spawns);
    Check(spawns.empty(), "late duplicate throw token must stay deduped");
    Check(!wire.empty() && wire.back().channel == Channel::Reliable &&
          wire.back().bytes.size() == sizeof(ServerGrenadeSpawnMessage),
          "grenade spawn must use reliable wire format");
    const WireMessage spawnWire = wire.back();
    host.PublishGrenadeDetonation(grenadeId, GrenadeKind::Frag, 4, 0, 8);

    ServerGrenadeDetonatedMessage detonation;
    detonation.grenadeId = grenadeId; detonation.x = 4.0f; detonation.z = 8.0f;
    Receive(3, detonation);
    client.Update(0.0f, local);
    std::vector<GrenadeDetonationEvent> detonations;
    client.DrainGrenadeDetonations(detonations);
    Check(detonations.size() == 1 && detonations[0].grenadeId == grenadeId,
          "duplicate grenade detonation must drain once");
    Receive(3, detonation); // duplicate after the first drain/update
    client.Update(0.0f, local);
    client.DrainGrenadeDetonations(detonations);
    Check(detonations.empty(), "delayed grenade detonation must be ignored");

    // The server spawn is accepted only from the negotiated server peer and
    // retains the client's token so the app can match its prediction.
    ServerGrenadeSpawnMessage spawnMessage;
    std::memcpy(&spawnMessage, spawnWire.bytes.data(),
                sizeof(spawnMessage));
    Receive(3, spawnMessage);
    client.Update(0.0f, local);
    client.DrainGrenadeSpawns(spawns);
    Check(spawns.size() == 1 && spawns[0].grenadeId == grenadeId &&
          spawns[0].clientToken == 77,
          "client must accept server spawn and preserve prediction token");
    Receive(999, spawnMessage);
    client.Update(0.0f, local);
    client.DrainGrenadeSpawns(spawns);
    Check(spawns.empty(), "wrong server must not spawn grenades");

    ClientGrenadeThrowMessage invalidThrow = throwMessage;
    invalidThrow.clientToken = 78;
    invalidThrow.kind = static_cast<GrenadeKind>(99);
    Receive(7, invalidThrow);
    invalidThrow = throwMessage;
    invalidThrow.clientToken = 79;
    invalidThrow.fuse = std::numeric_limits<float>::quiet_NaN();
    Receive(7, invalidThrow);
    host.Update(0.0f, local);
    host.DrainGrenadeSpawns(spawns);
    Check(spawns.empty(), "invalid grenade requests must be rejected");

    // Host-authored hostile and non-hostile kinds retain their wire meaning.
    for (GrenadeKind kind : { GrenadeKind::Frag, GrenadeKind::Molotov,
                              GrenadeKind::Vortex }) {
        const size_t before = wire.size();
        const uint32_t id = host.PublishGrenadeSpawn(
            0, 0, kind, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 1.0f, true);
        Check(id != 0 && wire.size() == before + 1,
              "host grenade publish must emit one packet");
        ServerGrenadeSpawnMessage hostileSpawn;
        std::memcpy(&hostileSpawn, wire.back().bytes.data(),
                    sizeof(hostileSpawn));
        Check(hostileSpawn.kind == kind && hostileSpawn.hostile != 0,
              "hostile grenade kind/flag must survive publish");
        Receive(3, hostileSpawn);
    }
    client.Update(0.0f, local);
    client.DrainGrenadeSpawns(spawns);
    Check(spawns.size() == 3 && spawns[0].hostile &&
          spawns[0].kind == GrenadeKind::Frag &&
          spawns[1].kind == GrenadeKind::Molotov &&
          spawns[2].kind == GrenadeKind::Vortex,
          "client must receive all hostile grenade kinds");
}
