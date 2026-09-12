#include "NetTransport.h"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <cstring>
#include <iostream>
#include <unordered_map>

// The only translation unit that knows a socket library exists. Everything
// above it talks to NetTransport, so swapping this out costs one file.
//
// GameNetworkingSockets is used in its standalone (non-Steam) mode: no Steam
// client, no app id, just UDP with reliable/unreliable channels, fragmentation
// and encryption handled for us.
namespace net {
namespace {

// GNS reports connection changes through a global callback with no user
// pointer in this API shape, so the active transport registers itself here for
// the duration of its connection. Only one transport is ever live -- the game
// is either hosting or joining, never both.
class GNSTransport;
GNSTransport* g_activeTransport = nullptr;

// Reference-counted library init: GNS wants exactly one Init/Kill pair per
// process, and the transport may be created and destroyed repeatedly as the
// player hosts, leaves and hosts again.
int g_libraryUsers = 0;

bool EnsureLibrary(std::string* error) {
    if (g_libraryUsers > 0) { ++g_libraryUsers; return true; }
    SteamNetworkingErrMsg message = {};
    if (!GameNetworkingSockets_Init(nullptr, message)) {
        if (error) *error = message;
        return false;
    }
    ++g_libraryUsers;
    return true;
}

void ReleaseLibrary() {
    if (g_libraryUsers <= 0) return;
    if (--g_libraryUsers == 0) GameNetworkingSockets_Kill();
}

class GNSTransport final : public NetTransport {
public:
    ~GNSTransport() override { Disconnect(); }

    bool Listen(uint16_t port, std::string* error) override {
        if (!EnsureLibrary(error)) return false;
        sockets_ = SteamNetworkingSockets();
        if (!sockets_) {
            if (error) *error = "GameNetworkingSockets interface unavailable";
            ReleaseLibrary();
            return false;
        }
        SteamNetworkingIPAddr address;
        address.Clear();
        address.m_port = port;

        g_activeTransport = this;
        SteamNetworkingConfigValue_t option;
        option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                      reinterpret_cast<void*>(OnConnectionStatusChanged));
        listenSocket_ = sockets_->CreateListenSocketIP(address, 1, &option);
        if (listenSocket_ == k_HSteamListenSocket_Invalid) {
            if (error) *error = "failed to listen on the requested port";
            g_activeTransport = nullptr;
            ReleaseLibrary();
            return false;
        }
        pollGroup_ = sockets_->CreatePollGroup();
        host_ = true;
        active_ = true;
        return true;
    }

    bool Connect(const std::string& host, uint16_t port,
                 std::string* error) override {
        if (!EnsureLibrary(error)) return false;
        sockets_ = SteamNetworkingSockets();
        if (!sockets_) {
            if (error) *error = "GameNetworkingSockets interface unavailable";
            ReleaseLibrary();
            return false;
        }
        SteamNetworkingIPAddr address;
        address.Clear();
        // ParseString takes host:port together, which also gives us IPv6 and
        // bracket syntax for free rather than parsing it here.
        const std::string endpoint = host + ":" + std::to_string(port);
        if (!address.ParseString(endpoint.c_str())) {
            if (error) *error = "could not parse address: " + endpoint;
            ReleaseLibrary();
            return false;
        }

        g_activeTransport = this;
        SteamNetworkingConfigValue_t option;
        option.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                      reinterpret_cast<void*>(OnConnectionStatusChanged));
        serverConnection_ = sockets_->ConnectByIPAddress(address, 1, &option);
        if (serverConnection_ == k_HSteamNetConnection_Invalid) {
            if (error) *error = "failed to start a connection to " + endpoint;
            g_activeTransport = nullptr;
            ReleaseLibrary();
            return false;
        }
        host_ = false;
        active_ = true;
        return true;
    }

    void Disconnect() override {
        if (!active_) return;
        if (sockets_) {
            for (const auto& entry : peers_)
                sockets_->CloseConnection(entry.second, 0, "closing", false);
            if (serverConnection_ != k_HSteamNetConnection_Invalid)
                sockets_->CloseConnection(serverConnection_, 0, "closing", false);
            if (pollGroup_ != k_HSteamNetPollGroup_Invalid)
                sockets_->DestroyPollGroup(pollGroup_);
            if (listenSocket_ != k_HSteamListenSocket_Invalid)
                sockets_->CloseListenSocket(listenSocket_);
        }
        peers_.clear();
        peerByConnection_.clear();
        serverConnection_ = k_HSteamNetConnection_Invalid;
        listenSocket_ = k_HSteamListenSocket_Invalid;
        pollGroup_ = k_HSteamNetPollGroup_Invalid;
        sockets_ = nullptr;
        if (g_activeTransport == this) g_activeTransport = nullptr;
        active_ = false;
        ReleaseLibrary();
    }

    void Poll(std::vector<Event>& events) override {
        events.clear();
        if (!active_ || !sockets_) return;

        // Connection state changes are delivered into pending_ by the callback
        // this call runs, so drain them after rather than before.
        pending_.clear();
        sockets_->RunCallbacks();
        for (Event& event : pending_) events.push_back(std::move(event));
        pending_.clear();

        ISteamNetworkingMessage* messages[32] = {};
        for (;;) {
            const int count = host_
                ? sockets_->ReceiveMessagesOnPollGroup(pollGroup_, messages, 32)
                : sockets_->ReceiveMessagesOnConnection(serverConnection_,
                                                        messages, 32);
            if (count <= 0) break;
            for (int i = 0; i < count; ++i) {
                ISteamNetworkingMessage* message = messages[i];
                Event event;
                event.type = EventType::Message;
                event.peer = PeerFor(message->m_conn);
                const auto* bytes =
                    static_cast<const uint8_t*>(message->m_pData);
                event.payload.assign(bytes, bytes + message->m_cbSize);
                events.push_back(std::move(event));
                message->Release();
            }
            if (count < 32) break;
        }
    }

    bool Send(PeerId peer, const void* data, uint32_t bytes,
              Channel channel) override {
        if (!active_ || !sockets_) return false;
        const HSteamNetConnection connection = ConnectionFor(peer);
        if (connection == k_HSteamNetConnection_Invalid) return false;
        const int flags = channel == Channel::Reliable
            ? k_nSteamNetworkingSend_Reliable
            : k_nSteamNetworkingSend_Unreliable;
        return sockets_->SendMessageToConnection(
            connection, data, bytes, flags, nullptr) == k_EResultOK;
    }

    void Broadcast(const void* data, uint32_t bytes, Channel channel,
                   PeerId except) override {
        if (!active_ || !sockets_) return;
        for (const auto& entry : peers_) {
            if (entry.first == except) continue;
            Send(entry.first, data, bytes, channel);
        }
    }

    bool IsActive() const override { return active_; }

private:
    static void OnConnectionStatusChanged(
        SteamNetConnectionStatusChangedCallback_t* info) {
        if (g_activeTransport) g_activeTransport->HandleStatusChange(info);
    }

    void HandleStatusChange(SteamNetConnectionStatusChangedCallback_t* info) {
        switch (info->m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Inbound on the listen socket: accept and put it in the poll
            // group so ReceiveMessagesOnPollGroup sees it.
            if (host_ && info->m_eOldState == k_ESteamNetworkingConnectionState_None) {
                if (sockets_->AcceptConnection(info->m_hConn) != k_EResultOK) {
                    sockets_->CloseConnection(info->m_hConn, 0, nullptr, false);
                    break;
                }
                sockets_->SetConnectionPollGroup(info->m_hConn, pollGroup_);
            }
            break;
        case k_ESteamNetworkingConnectionState_Connected: {
            const PeerId peer = AddPeer(info->m_hConn);
            Event event;
            event.type = EventType::Connected;
            event.peer = peer;
            pending_.push_back(std::move(event));
            break;
        }
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            const PeerId peer = PeerFor(info->m_hConn);
            if (peer != kInvalidPeer) {
                Event event;
                event.type = EventType::Disconnected;
                event.peer = peer;
                pending_.push_back(std::move(event));
                peers_.erase(peer);
                peerByConnection_.erase(info->m_hConn);
            }
            sockets_->CloseConnection(info->m_hConn, 0, nullptr, false);
            break;
        }
        default:
            break;
        }
    }

    PeerId AddPeer(HSteamNetConnection connection) {
        const auto existing = peerByConnection_.find(connection);
        if (existing != peerByConnection_.end()) return existing->second;
        // Ids start at 1 so kInvalidPeer (0) always means "no peer".
        const PeerId peer = nextPeer_++;
        peers_[peer] = connection;
        peerByConnection_[connection] = peer;
        return peer;
    }

    PeerId PeerFor(HSteamNetConnection connection) const {
        const auto it = peerByConnection_.find(connection);
        return it == peerByConnection_.end() ? kInvalidPeer : it->second;
    }

    HSteamNetConnection ConnectionFor(PeerId peer) const {
        const auto it = peers_.find(peer);
        return it == peers_.end() ? k_HSteamNetConnection_Invalid : it->second;
    }

    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup_ = k_HSteamNetPollGroup_Invalid;
    HSteamNetConnection serverConnection_ = k_HSteamNetConnection_Invalid;
    std::unordered_map<PeerId, HSteamNetConnection> peers_;
    std::unordered_map<HSteamNetConnection, PeerId> peerByConnection_;
    std::vector<Event> pending_;
    PeerId nextPeer_ = 1;
    bool host_ = false;
    bool active_ = false;
};

} // namespace

std::unique_ptr<NetTransport> MakeGNSTransport() {
    return std::make_unique<GNSTransport>();
}

} // namespace net
