#include "NetTransport.h"

namespace net {
namespace {

// Accepts every call and delivers nothing. See MakeNullTransport in
// NetTransport.h for why this exists: it lets the session, dispatch and
// remote-player code be written and tested before a socket library is chosen,
// and keeps a build with networking disabled compiling unchanged.
class NullTransport final : public NetTransport {
public:
    bool Listen(uint16_t, std::string*) override {
        active_ = true;
        return true;
    }
    bool Connect(const std::string&, uint16_t, std::string* error) override {
        // Deliberately fails rather than pretending: a client that "connects"
        // to nothing and then sits in an empty session is a confusing state to
        // debug. Hosting alone is a legitimate offline case; joining nothing
        // is not.
        if (error) *error = "networking is not built into this binary";
        return false;
    }
    void Disconnect() override { active_ = false; }
    void Poll(std::vector<Event>& events) override { events.clear(); }
    bool Send(PeerId, const void*, uint32_t, Channel) override { return false; }
    void Broadcast(const void*, uint32_t, Channel, PeerId) override {}
    bool IsActive() const override { return active_; }

private:
    bool active_ = false;
};

} // namespace

std::unique_ptr<NetTransport> MakeNullTransport() {
    return std::make_unique<NullTransport>();
}

} // namespace net
