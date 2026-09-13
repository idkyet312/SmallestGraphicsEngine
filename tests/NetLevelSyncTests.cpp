// The host's map is session state: a client that joins has to be told which
// level to load, or two players share coordinates on terrain only one of them
// has. These exercise that seam against a fake transport -- what the host puts
// on the wire, and what a client accepts off it.
#include "NetSession.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
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
}
