#include "NetSession.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

// The ground is host-authoritative: a client cuts no craters of its own, so
// whatever these messages carry IS the terrain it ends up standing on. What is
// worth pinning is therefore not the arithmetic -- there is none on this side --
// but the delivery: that a cut reaches a client intact, that it is applied
// exactly once however many times it arrives, and that someone joining midway
// is handed the holes everyone else is already standing in.
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

net::TerrainDeform MakeCrater(float x, float z) {
    net::TerrainDeform deform;
    deform.x = x;
    deform.z = z;
    deform.radius = 6.5f;
    deform.value = 2.25f;
    deform.strength = 2.0f;
    deform.edgeFalloff = 0.4f;
    deform.baseHeight = 12.5f;
    deform.impactY = 13.0f;
    deform.operation = 3; // Crater
    return deform;
}

// Counts only the terrain messages: the session sends level and join traffic
// through the same wire, and a test that indexed by position would break the
// next time either of those changes.
size_t TerrainMessagesFrom(size_t start) {
    size_t count = 0;
    for (size_t i = start; i < wire.size(); ++i) {
        if (wire[i].bytes.size() != sizeof(net::ServerTerrainDeformMessage))
            continue;
        net::MessageHeader header{};
        std::memcpy(&header, wire[i].bytes.data(), sizeof(header));
        if (header.type == net::MessageType::ServerTerrainDeform) ++count;
    }
    return count;
}

net::ServerTerrainDeformMessage LastTerrainMessage() {
    for (size_t i = wire.size(); i > 0; --i) {
        const WireMessage& sent = wire[i - 1];
        if (sent.bytes.size() != sizeof(net::ServerTerrainDeformMessage))
            continue;
        net::ServerTerrainDeformMessage message{};
        std::memcpy(&message, sent.bytes.data(), sizeof(message));
        if (message.header.type == net::MessageType::ServerTerrainDeform)
            return message;
    }
    Check(false, "no terrain deform was sent");
    return {};
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
    ClientHello hello;
    Receive(7, hello);
    host.Update(1.0f / 30.0f, local);

    // A cut goes out reliably. Unreliable would be wrong in a way that only
    // shows up as two players disagreeing about the shape of the ground.
    size_t mark = wire.size();
    host.PublishTerrainDeform(MakeCrater(10.0f, -20.0f));
    Check(TerrainMessagesFrom(mark) == 1, "host must send the cut");
    Check(wire.back().channel == Channel::Reliable,
          "terrain deform must go out reliably");
    const ServerTerrainDeformMessage firstCut = LastTerrainMessage();
    Check(firstCut.deformId != 0, "a cut must carry an id");

    mark = wire.size();
    host.PublishTerrainDeform(MakeCrater(11.0f, -21.0f));
    const ServerTerrainDeformMessage secondCut = LastTerrainMessage();
    Check(secondCut.deformId != firstCut.deformId,
          "each cut needs its own id, or the second is taken for a duplicate");

    // Garbage must not reach the height field: these numbers drive a physics
    // rebuild, and a NaN radius corrupts the ground rather than failing loudly.
    mark = wire.size();
    TerrainDeform broken = MakeCrater(1.0f, 1.0f);
    broken.radius = std::numeric_limits<float>::quiet_NaN();
    host.PublishTerrainDeform(broken);
    broken = MakeCrater(1.0f, 1.0f);
    broken.radius = 0.0f;
    host.PublishTerrainDeform(broken);
    broken = MakeCrater(1.0f, 1.0f);
    broken.baseHeight = std::numeric_limits<float>::infinity();
    host.PublishTerrainDeform(broken);
    Check(TerrainMessagesFrom(mark) == 0,
          "a malformed cut must never leave the host");

    // A client applies what it is told, once.
    NetSession client;
    Check(client.StartClient("host", 27120, nullptr), "client failed");
    ServerWelcome welcome;
    welcome.assignedId = 1;
    Receive(3, welcome);
    Receive(3, firstCut);
    Receive(3, secondCut);
    client.Update(0.0f, local);
    std::vector<TerrainDeformEvent> deforms;
    client.DrainTerrainDeforms(deforms);
    Check(deforms.size() == 2, "client must receive both cuts");
    Check(deforms[0].deform.x == 10.0f && deforms[1].deform.x == 11.0f,
          "cuts must arrive in the order they were made");
    Check(deforms[0].deform.radius == 6.5f &&
          deforms[0].deform.value == 2.25f &&
          deforms[0].deform.edgeFalloff == 0.4f &&
          deforms[0].deform.baseHeight == 12.5f &&
          deforms[0].deform.impactY == 13.0f &&
          deforms[0].deform.operation == 3,
          "the whole resolved stamp must survive the trip");

    // The join backlog and the live broadcast overlap by design, so the same id
    // arrives twice for a cut made while someone was connecting. Applying it
    // twice would dig the hole twice as deep.
    Receive(3, firstCut);
    client.Update(0.0f, local);
    client.DrainTerrainDeforms(deforms);
    Check(deforms.empty(), "a repeated cut must be ignored");

    ServerTerrainDeformMessage spoof = secondCut;
    spoof.deformId += 500;
    Receive(999, spoof);
    client.Update(0.0f, local);
    client.DrainTerrainDeforms(deforms);
    Check(deforms.empty(), "only the host may cut the ground");

    net::Event truncated;
    truncated.peer = 3;
    truncated.payload.resize(sizeof(ServerTerrainDeformMessage) - 1);
    inbox.push_back(std::move(truncated));
    client.Update(0.0f, local);
    client.DrainTerrainDeforms(deforms);
    Check(deforms.empty(), "a truncated cut must be rejected");

    ServerTerrainDeformMessage nonsense = secondCut;
    nonsense.deformId += 900;
    nonsense.deform.radius = std::numeric_limits<float>::quiet_NaN();
    Receive(3, nonsense);
    client.Update(0.0f, local);
    client.DrainTerrainDeforms(deforms);
    Check(deforms.empty(), "a NaN radius must not reach the height field");

    // A client's own explosion. Its rocket, C4 and barrels are simulated only
    // on its own machine, so this request is the host's only word that the
    // ground changed -- and the client applies nothing until the host says so.
    mark = wire.size();
    client.RequestTerrainDeform(MakeCrater(30.0f, 40.0f));
    Check(TerrainMessagesFrom(mark) == 0,
          "a client must not broadcast a cut as though it were the host");
    size_t requests = 0;
    for (size_t i = mark; i < wire.size(); ++i) {
        if (wire[i].bytes.size() != sizeof(ClientTerrainDeformMessage))
            continue;
        MessageHeader header{};
        std::memcpy(&header, wire[i].bytes.data(), sizeof(header));
        if (header.type == MessageType::ClientTerrainDeform) ++requests;
    }
    Check(requests == 1, "a client must ask the host for its cut");

    ClientTerrainDeformMessage request;
    request.deform = MakeCrater(30.0f, 40.0f);
    Receive(7, request); // peer 7 completed the handshake at the top
    host.Update(0.0f, local);
    std::vector<TerrainDeform> requested;
    host.DrainRequestedTerrainDeforms(requested);
    Check(requested.size() == 1 && requested[0].x == 30.0f &&
          requested[0].z == 40.0f && requested[0].radius == 6.5f,
          "the host must take an authenticated client's cut");

    // The host republishes it as its own, which is what gets it to every other
    // client and into the join backlog.
    mark = wire.size();
    host.PublishTerrainDeform(requested[0]);
    Check(TerrainMessagesFrom(mark) == 1,
          "a client's cut must go back out as the host's");

    Receive(999, request);
    host.Update(0.0f, local);
    host.DrainRequestedTerrainDeforms(requested);
    Check(requested.empty(), "an unknown peer must not reshape the map");

    // Clamped, not trusted. A radius far past anything the game can produce
    // would carve a canyon out of the map for everyone.
    ClientTerrainDeformMessage absurd;
    absurd.deform = MakeCrater(5.0f, 5.0f);
    absurd.deform.radius = 5000.0f;
    Receive(7, absurd);
    ClientTerrainDeformMessage deep;
    deep.deform = MakeCrater(6.0f, 6.0f);
    deep.deform.value = 900.0f;
    Receive(7, deep);
    ClientTerrainDeformMessage notANumber;
    notANumber.deform = MakeCrater(7.0f, 7.0f);
    notANumber.deform.value = std::numeric_limits<float>::quiet_NaN();
    Receive(7, notANumber);
    host.Update(0.0f, local);
    host.DrainRequestedTerrainDeforms(requested);
    Check(requested.empty(), "an out-of-range cut must be refused");

    net::Event shortRequest;
    shortRequest.peer = 7;
    shortRequest.payload.resize(sizeof(ClientTerrainDeformMessage) - 1);
    inbox.push_back(std::move(shortRequest));
    host.Update(0.0f, local);
    host.DrainRequestedTerrainDeforms(requested);
    Check(requested.empty(), "a truncated request must be rejected");

    // Someone joining mid-session is handed the ground as it stands: the two
    // host cuts that survived validation, plus the client's republished one.
    mark = wire.size();
    Receive(9, hello);
    host.Update(0.0f, local);
    Check(TerrainMessagesFrom(mark) == 3,
          "a joining peer must be replayed every cut made so far, including "
          "the ones a client asked for");

    // Changing level retires them. Replaying a crater onto the next map would
    // cut its ground at the previous one's coordinates.
    host.SetHostLevel(LevelKind::LevelFile, "Islandv10.json");
    mark = wire.size();
    Receive(11, hello);
    host.Update(0.0f, local);
    Check(TerrainMessagesFrom(mark) == 0,
          "cuts must not outlive the level they were made on");

    std::cout << "NetTerrainSync tests passed ("
              << sizeof(ServerTerrainDeformMessage)
              << " byte terrain deform)\n";
    return 0;
}
