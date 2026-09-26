#include "NetSession.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
struct Sent {
    net::PeerId peer;
    net::Channel channel;
    std::vector<uint8_t> bytes;
};
std::vector<net::Event> inbox;
std::vector<Sent> wire;
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
    bool Send(net::PeerId peer, const void* data, uint32_t size,
              net::Channel channel) override {
        Sent sent{ peer, channel, std::vector<uint8_t>(size) };
        std::memcpy(sent.bytes.data(), data, size);
        wire.push_back(std::move(sent));
        return true;
    }
    void Broadcast(const void* data, uint32_t size, net::Channel channel,
                   net::PeerId = net::kInvalidPeer) override {
        Send(net::kInvalidPeer, data, size, channel);
    }
    bool IsActive() const override { return true; }
};
template<class T> std::vector<T> Messages(size_t first, net::MessageType type) {
    std::vector<T> result;
    for (size_t i = first; i < wire.size(); ++i) {
        if (wire[i].bytes.size() != sizeof(T)) continue;
        T message{};
        std::memcpy(&message, wire[i].bytes.data(), sizeof(message));
        if (message.header.type == type) result.push_back(message);
    }
    return result;
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
    host.SetHostLevel(LevelKind::TestLevel, "");
    ChargeStickData charge;
    charge.x = 2.0f; charge.y = 3.0f; charge.z = 4.0f;
    charge.part = SGE::ChargeAnchorPart::AATurretElevation;
    charge.turretEntityId = 45;
    charge.turretOrdinal = 2;
    charge.turretX = 1.0f; charge.turretY = 0.0f; charge.turretZ = 1.0f;
    charge.localX = 0.2f; charge.localZ = 2.5f;
    size_t mark = wire.size();
    host.ReportChargeStuck(charge);
    auto planted = Messages<ServerChargeStuckMessage>(
        mark, MessageType::ServerChargeStuck);
    Check(planted.size() == 1 && planted[0].chargeId != 0 &&
          planted[0].charge.turretEntityId == 45 &&
          planted[0].charge.part == SGE::ChargeAnchorPart::AATurretElevation,
          "host must reliably commit attachment and id");
    std::vector<RemoteChargeStuck> applied;
    host.DrainChargeSticks(applied);
    Check(applied.size() == 1 && applied[0].chargeId == planted[0].chargeId,
          "host must apply its own committed charge");

    host.UpdateActiveChargePose(planted[0].chargeId,
        3.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.5f, 0.0f, 0.8660254f, true);

    ClientHello hello;
    mark = wire.size();
    Receive(7, hello);
    host.Update(0.0f, local);
    auto replay = Messages<ServerChargeStuckMessage>(
        mark, MessageType::ServerChargeStuck);
    Check(replay.size() == 1 && replay[0].chargeId == planted[0].chargeId &&
          replay[0].charge.frozen == 1 && replay[0].charge.y == 2.0f &&
          replay[0].charge.qy == 0.5f,
          "joiner must receive the active charge after handshake");

    NetSession client;
    Check(client.StartClient("host", 27120, nullptr), "client failed");
    ServerWelcome welcome;
    welcome.assignedId = 1;
    Receive(3, welcome);
    Receive(3, replay[0]);
    Receive(3, replay[0]);
    client.Update(0.0f, local);
    client.DrainChargeSticks(applied);
    Check(applied.size() == 1 && applied[0].chargeId == planted[0].chargeId,
          "join replay must apply once despite duplicate delivery");
    Receive(3, replay[0]);
    client.Update(0.0f, local);
    client.DrainChargeSticks(applied);
    Check(applied.empty(), "committed charge id must deduplicate");

    AATurretSnapshot destroyedTurret;
    destroyedTurret.dead = 1;
    destroyedTurret.killer = 1;
    destroyedTurret.x = 12.0f;
    destroyedTurret.z = 24.0f;
    mark = wire.size();
    host.PublishArmor(nullptr, 0, &destroyedTurret, 1, nullptr, 0);
    host.Update(1.0f / 20.0f, local);
    const auto armor = Messages<ServerArmorStateMessage>(
        mark, MessageType::ServerArmorState);
    Check(!armor.empty(), "host must publish the destroyed turret");
    Receive(3, armor.back());
    client.Update(0.0f, local);
    const auto* remoteArmor = client.RemoteArmor();
    Check(remoteArmor && remoteArmor->turretCount == 1 &&
          remoteArmor->turrets[0].dead == 1 &&
          remoteArmor->turrets[0].killer == 1,
          "client must receive turret death and planter credit");

    ServerChargeStuckMessage oldLevelCharge = replay[0];
    oldLevelCharge.chargeId += 100;
    ServerLevelMessage newLevel;
    newLevel.kind = LevelKind::Level1;
    Receive(3, oldLevelCharge);
    Receive(3, newLevel);
    client.Update(0.0f, local);
    client.DrainChargeSticks(applied);
    Check(applied.empty(), "level change must discard queued old charges");

    host.ReportChargeDetonate();
    mark = wire.size();
    Receive(9, hello);
    host.Update(0.0f, local);
    Check(Messages<ServerChargeStuckMessage>(mark,
          MessageType::ServerChargeStuck).empty(),
          "detonated charges must not replay");

    host.ReportChargeStuck(charge);
    host.RestartHostLevel();
    mark = wire.size();
    Receive(10, hello);
    host.Update(0.0f, local);
    Check(Messages<ServerChargeStuckMessage>(mark,
          MessageType::ServerChargeStuck).empty(),
          "restart must clear active charge replay");
    return 0;
}
