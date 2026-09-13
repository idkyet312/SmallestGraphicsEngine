#include "NetTransport.h"

#ifdef SGE_WITH_STEAM_SOCKETS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "EngineLogger.h"

#include <steamworks/isteamnetworkingsockets.h>
#include <steamworks/isteamnetworkingutils.h>
#include <steamworks/isteamuser.h>
#include <steamworks/steamnetworkingtypes.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// Peer-to-peer transport over Steam's relay network, addressed by SteamID.
//
// The IP transport beside this one (NetTransportGNS.cpp) dials a host:port,
// which is exactly what a friend on the internet cannot do: the host sits
// behind a router that drops the packet, and the fix -- port forwarding -- is
// not something you can ask a player to do. Connecting to a SteamID instead
// hands the routing to Steam, which punches through or falls back to its own
// relays, and neither end ever learns the other's address.
//
// The interfaces are reached through the DLL the game already loaded for rich
// presence, resolved by name rather than linked. Two reasons, both practical:
// linking steam_api64.lib would make the DLL a hard launch requirement on
// machines that have no Steam, and its flat exports collide by name with the
// standalone GameNetworkingSockets build that the IP transport links against.
// Resolving the versioned accessor by hand keeps the two side by side.
namespace net {
namespace {

struct SteamNetworkingApi {
    ISteamNetworkingSockets* sockets = nullptr;
    ISteamNetworkingUtils* utils = nullptr;
    ISteamUser* user = nullptr;
};

SteamNetworkingApi g_api;

// The accessors are versioned in their export name, and the version is part of
// the ABI contract: v012 returns a pointer whose vtable matches the
// ISteamNetworkingSockets declared in the headers this file was built against.
// A DLL too old to export it gets no Steam transport rather than a wrong one.
bool EnsureApi(std::string* error) {
    if (g_api.sockets && g_api.utils) return true;
    // Already in the process: the app loads it at startup for rich presence,
    // and a second LoadLibrary here would only add a reference to free.
    HMODULE module = GetModuleHandleA("steam_api64.dll");
    if (!module) {
        if (error) *error = "Steam is not running (steam_api64.dll not loaded)";
        return false;
    }
    auto sockets = reinterpret_cast<ISteamNetworkingSockets* (*)()>(
        GetProcAddress(module, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012"));
    auto utils = reinterpret_cast<ISteamNetworkingUtils* (*)()>(
        GetProcAddress(module, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004"));
    auto user = reinterpret_cast<ISteamUser* (*)()>(
        GetProcAddress(module, "SteamAPI_SteamUser_v023"));
    if (!sockets || !utils) {
        if (error) *error = "steam_api64.dll does not export the networking "
                            "interfaces this build expects";
        return false;
    }
    g_api.sockets = sockets();
    g_api.utils = utils();
    g_api.user = user ? user() : nullptr;
    if (!g_api.sockets || !g_api.utils) {
        if (error) *error = "Steam networking interfaces unavailable; is the "
                            "client signed in?";
        g_api.sockets = nullptr;
        g_api.utils = nullptr;
        return false;
    }
    return true;
}

class SteamP2PTransport;
// Connection state arrives as a process-wide callback with no user pointer, so
// the live transport registers itself. Only one is ever live: the game is
// either hosting or joining, never both.
SteamP2PTransport* g_activeTransport = nullptr;

class SteamP2PTransport final : public NetTransport {
public:
    ~SteamP2PTransport() override { Disconnect(); }

    bool Listen(uint16_t, std::string* error) override {
        if (!Begin(error)) return false;
        // The port is ignored on purpose. A P2P listen socket is addressed by
        // SteamID and a virtual port, and virtual port 0 is this game's only
        // one -- there is nothing for a UDP port number to mean here.
        listenSocket_ = g_api.sockets->CreateListenSocketP2P(0, 0, nullptr);
        if (listenSocket_ == k_HSteamListenSocket_Invalid) {
            if (error) *error = "Steam refused a P2P listen socket";
            End();
            return false;
        }
        host_ = true;
        return true;
    }

    // `host` is a SteamID64, with or without the "steam:" prefix the rest of
    // the game uses to mark an address as one.
    bool Connect(const std::string& host, uint16_t, std::string* error) override {
        if (!Begin(error)) return false;
        const uint64_t steamId = ParseSteamId(host);
        if (steamId == 0) {
            if (error) *error = "'" + host + "' is not a SteamID";
            End();
            return false;
        }
        SteamNetworkingIdentity identity;
        identity.SetSteamID64(steamId);
        serverConnection_ =
            g_api.sockets->ConnectP2P(identity, 0, 0, nullptr);
        if (serverConnection_ == k_HSteamNetConnection_Invalid) {
            if (error) *error = "Steam refused the connection to " + host;
            End();
            return false;
        }
        g_api.sockets->SetConnectionPollGroup(serverConnection_, pollGroup_);
        connections_[serverConnection_] = true;
        return true;
    }

    void Disconnect() override {
        if (!g_api.sockets) { End(); return; }
        for (const auto& entry : connections_)
            g_api.sockets->CloseConnection(entry.first, 0, "closing", false);
        connections_.clear();
        if (listenSocket_ != k_HSteamListenSocket_Invalid) {
            g_api.sockets->CloseListenSocket(listenSocket_);
            listenSocket_ = k_HSteamListenSocket_Invalid;
        }
        serverConnection_ = k_HSteamNetConnection_Invalid;
        host_ = false;
        End();
    }

    void Poll(std::vector<Event>& events) override {
        events.clear();
        if (!active_) return;
        // Connection state was queued by the callback since the last poll.
        events.swap(pending_);
        SteamNetworkingMessage_t* messages[64] = {};
        for (;;) {
            const int count = g_api.sockets->ReceiveMessagesOnPollGroup(
                pollGroup_, messages, 64);
            if (count <= 0) break;
            for (int i = 0; i < count; ++i) {
                SteamNetworkingMessage_t* message = messages[i];
                Event event;
                event.type = EventType::Message;
                event.peer = message->m_conn;
                const auto* bytes = static_cast<const uint8_t*>(message->m_pData);
                event.payload.assign(bytes, bytes + message->m_cbSize);
                events.push_back(std::move(event));
                // Steam owns the buffer until this call; the copy above is why
                // the game thread can hold the payload afterwards.
                message->Release();
            }
            if (count < 64) break;
        }
    }

    bool Send(PeerId peer, const void* data, uint32_t bytes,
              Channel channel) override {
        if (!active_ || peer == kInvalidPeer) return false;
        const int flags = channel == Channel::Reliable
            ? k_nSteamNetworkingSend_Reliable
            : k_nSteamNetworkingSend_UnreliableNoDelay;
        const EResult result = g_api.sockets->SendMessageToConnection(
            peer, data, bytes, flags, nullptr);
        return result == k_EResultOK;
    }

    void Broadcast(const void* data, uint32_t bytes, Channel channel,
                   PeerId except) override {
        for (const auto& entry : connections_) {
            if (entry.first == except) continue;
            Send(entry.first, data, bytes, channel);
        }
    }

    bool IsActive() const override { return active_; }

    // Called from the callback pump with the process-wide connection event.
    void OnStatusChanged(const SteamNetConnectionStatusChangedCallback_t& info) {
        switch (info.m_info.m_eState) {
        case k_ESteamNetworkingConnectionState_Connecting:
            // Inbound: a friend is dialling the listen socket. Outbound
            // connections pass through this state too, which is why the
            // listen-socket check is what distinguishes them.
            if (!host_ || info.m_info.m_hListenSocket != listenSocket_) break;
            if (g_api.sockets->AcceptConnection(info.m_hConn) != k_EResultOK) {
                g_api.sockets->CloseConnection(info.m_hConn, 0, nullptr, false);
                break;
            }
            g_api.sockets->SetConnectionPollGroup(info.m_hConn, pollGroup_);
            connections_[info.m_hConn] = true;
            break;
        case k_ESteamNetworkingConnectionState_Connected: {
            // Announced here rather than on accept: until the handshake
            // finishes there is nothing that can be sent down it.
            connections_[info.m_hConn] = true;
            Event event;
            event.type = EventType::Connected;
            event.peer = info.m_hConn;
            pending_.push_back(std::move(event));
            break;
        }
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
            // The reason is the whole diagnosis. Steam distinguishes "the
            // relay is not ready yet", "no route to that peer" and "that app
            // is not allowed to do this", and without these two fields every
            // one of them reaches the game as a bare disconnect.
            SGE_LOG("LogNet", EngineLog::Level::Warning,
                std::string("Steam connection closed (reason ") +
                std::to_string(info.m_info.m_eEndReason) + "): " +
                info.m_info.m_szEndDebug);
            // Both ends of a closed connection have to be cleaned up here:
            // Steam keeps the handle alive until it is closed explicitly, and
            // a connection left open is a handle leak per disconnect.
            if (connections_.erase(info.m_hConn) > 0) {
                Event event;
                event.type = EventType::Disconnected;
                event.peer = info.m_hConn;
                pending_.push_back(std::move(event));
            }
            g_api.sockets->CloseConnection(info.m_hConn, 0, nullptr, false);
            if (info.m_hConn == serverConnection_)
                serverConnection_ = k_HSteamNetConnection_Invalid;
            break;
        }
        default:
            break;
        }
    }

private:
    static uint64_t ParseSteamId(const std::string& address) {
        std::string digits = address;
        const size_t prefix = digits.rfind("steam:", 0) == 0 ? 6 : 0;
        digits.erase(0, prefix);
        if (digits.empty()) return 0;
        for (char c : digits) if (c < '0' || c > '9') return 0;
        return std::strtoull(digits.c_str(), nullptr, 10);
    }

    bool Begin(std::string* error) {
        if (!EnsureApi(error)) return false;
        // Relay access takes a few seconds to negotiate and is needed by both
        // ends. Asking early means the first connection is not the thing that
        // pays for it; it is safe to call more than once.
        g_api.utils->InitRelayNetworkAccess();
        pollGroup_ = g_api.sockets->CreatePollGroup();
        if (pollGroup_ == k_HSteamNetPollGroup_Invalid) {
            if (error) *error = "Steam refused a poll group";
            return false;
        }
        g_activeTransport = this;
        active_ = true;
        return true;
    }

    void End() {
        if (pollGroup_ != k_HSteamNetPollGroup_Invalid && g_api.sockets) {
            g_api.sockets->DestroyPollGroup(pollGroup_);
            pollGroup_ = k_HSteamNetPollGroup_Invalid;
        }
        if (g_activeTransport == this) g_activeTransport = nullptr;
        active_ = false;
        pending_.clear();
    }

    bool active_ = false;
    bool host_ = false;
    HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup_ = k_HSteamNetPollGroup_Invalid;
    HSteamNetConnection serverConnection_ = k_HSteamNetConnection_Invalid;
    std::unordered_map<PeerId, bool> connections_;
    // Connection edges arrive on the callback, which can fire at any point
    // inside the pump; they are handed over on the next Poll so the game sees
    // them at the same place it sees messages.
    std::vector<Event> pending_;
};

} // namespace

std::unique_ptr<NetTransport> MakeSteamTransport() {
    return std::make_unique<SteamP2PTransport>();
}

bool SteamTransportAvailable() {
    return EnsureApi(nullptr);
}

uint64_t SteamTransportLocalId() {
    if (!EnsureApi(nullptr) || !g_api.user) return 0;
    return g_api.user->GetSteamID().ConvertToUint64();
}

void SteamTransportHandleCallback(int callbackId, const void* data, int bytes) {
    if (callbackId != SteamNetConnectionStatusChangedCallback_t::k_iCallback)
        return;
    if (!g_activeTransport || !data ||
        bytes < int(sizeof(SteamNetConnectionStatusChangedCallback_t)))
        return;
    SteamNetConnectionStatusChangedCallback_t info;
    std::memcpy(&info, data, sizeof(info));
    g_activeTransport->OnStatusChanged(info);
}

} // namespace net

#endif // SGE_WITH_STEAM_SOCKETS
