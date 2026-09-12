#ifndef NET_TRANSPORT_H
#define NET_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The seam between the game and whichever socket library is underneath.
//
// The game never includes a networking library's headers: it talks to this
// interface, and exactly one translation unit knows what actually moves the
// bytes. That keeps a heavyweight dependency (GameNetworkingSockets pulls in
// OpenSSL and protobuf) from leaking into engine headers, and means the choice
// can be revisited after measuring rather than being baked in everywhere.
namespace net {

// Transport-level identity for one peer. Distinct from PlayerId in
// NetProtocol.h, which is assigned by the game layer after a successful
// handshake -- a peer exists at this level before it is a player.
using PeerId = uint32_t;
inline constexpr PeerId kInvalidPeer = 0;

enum class Channel : uint8_t {
    // Ordered and retransmitted. Handshakes and join/leave, where a dropped
    // message leaves the two ends permanently disagreeing.
    Reliable = 0,
    // Dropped rather than retransmitted when late. Input and snapshots, where
    // a stale packet is worth less than the next fresh one.
    Unreliable = 1,
};

enum class EventType : uint8_t {
    Connected,
    Disconnected,
    Message,
};

struct Event {
    EventType type = EventType::Message;
    PeerId peer = kInvalidPeer;
    // Populated for Message events only. Owned by the event; copied out of the
    // transport's buffers so the game thread can hold it safely.
    std::vector<uint8_t> payload;
};

class NetTransport {
public:
    virtual ~NetTransport() = default;

    // Host: accept connections on this port. Client: connect to host:port.
    // Both return false and leave the transport idle on failure, so a failed
    // listen is a recoverable "stay single-player", not a crash.
    virtual bool Listen(uint16_t port, std::string* error) = 0;
    virtual bool Connect(const std::string& host, uint16_t port,
                         std::string* error) = 0;
    virtual void Disconnect() = 0;

    // Pulls everything that arrived since the last call. The game drives this
    // from its own loop, so message handling always happens at a point where
    // touching game state is safe -- no callbacks from a socket thread.
    virtual void Poll(std::vector<Event>& events) = 0;

    virtual bool Send(PeerId peer, const void* data, uint32_t bytes,
                      Channel channel) = 0;
    // Host-side convenience: same payload to every connected peer, optionally
    // skipping one (usually the peer the message originated from).
    virtual void Broadcast(const void* data, uint32_t bytes, Channel channel,
                           PeerId except = kInvalidPeer) = 0;

    virtual bool IsActive() const = 0;
};

// Loopback implementation: no sockets, no dependency. Connect() and Listen()
// succeed, Poll() returns nothing. It exists so the session layer, the message
// dispatch and the remote-player rendering can all be built and tested before
// a real socket library is wired up -- and so a build without networking
// enabled still compiles and runs single-player unchanged.
std::unique_ptr<NetTransport> MakeNullTransport();

} // namespace net

#endif // NET_TRANSPORT_H
