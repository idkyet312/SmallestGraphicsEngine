#include "NetSession.h"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
std::vector<net::Event> inbox;
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
template<class T> void Receive(const T& message) {
    net::Event event;
    event.peer = 1;
    event.payload.resize(sizeof(message));
    std::memcpy(event.payload.data(), &message, sizeof(message));
    inbox.push_back(std::move(event));
}
class TestTransport : public net::NetTransport {
public:
    bool Listen(uint16_t, std::string*) override { return true; }
    bool Connect(const std::string&, uint16_t, std::string*) override { return true; }
    void Disconnect() override {}
    void Poll(std::vector<net::Event>& events) override {
        events.clear(); events.swap(inbox);
    }
    bool Send(net::PeerId, const void*, uint32_t, net::Channel) override { return true; }
    void Broadcast(const void*, uint32_t, net::Channel, net::PeerId) override {}
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
    NetSession host;
    Check(host.StartHost(27100, nullptr), "host failed");
    LocalPlayerState local;
    local.x = 100.0f;
    local.z = 200.0f;
    Receive(ClientHello{});
    host.Update(1.0f / 30.0f, local);
    host.ApplyHitToPlayer(1, 0, kBodyShotDamage, true, 0, 0, 0);
    ClientInputMessage input;
    input.x = local.x + 1.0f;
    input.z = local.z;
    input.input.sequence = 2;
    Receive(input);
    ClientReviveProgressMessage revive;
    revive.target = 0;
    revive.holding = 1;
    Receive(revive);
    host.Update(1.0f / 30.0f, local);
    LocalPlayerStatus status;
    Check(host.LocalStatus(status) && status.downed && status.reviveProgress > 0,
          "client beside host must start reviving player 1");
    // An older movement packet must not move the reviver out of range.
    input.input.sequence = 1;
    input.x = -1000.0f;
    Receive(input);
    // Nor may a malformed position poison the distance check.
    input.input.sequence = 3;
    input.x = std::numeric_limits<float>::quiet_NaN();
    Receive(input);
    for (int i = 0; i < 150; ++i) {
        Receive(revive);
        host.Update(1.0f / 30.0f, local);
    }
    Check(host.LocalStatus(status) && !status.downed && status.health == kReviveHealth,
          "player 2 must revive player 1 to revive health");
    std::vector<PlayerStateChange> changes;
    host.DrainStateChanges(changes);
    Check(changes.size() == 2 && changes.back().event == PlayerStateEvent::Revived &&
          changes.back().id == 0 && changes.back().instigator == 1,
          "revive must credit player 2");
}

