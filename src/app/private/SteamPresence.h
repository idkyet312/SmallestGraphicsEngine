#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Safe after <windows.h> only because main.cpp defines WIN32_LEAN_AND_MEAN,
// which keeps the old winsock.h out; without it these would redefine it.
// Used for one thing: the LAN address a friend has to dial to reach a host.
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Steam integration: tell the Steam client this process is the game, so it
// shows as played, counts playtime, and can carry a status line to friends.
//
// Loaded at runtime rather than linked. Linking steam_api64.lib makes the DLL a
// hard launch requirement: a machine without Steam -- or a build tree without
// the Steamworks SDK dropped in -- dies in the loader before any of our code
// runs, which is a terrible trade for a feature that is decoration. Resolving
// the handful of flat-API exports by hand keeps this a normal standalone build
// that lights up only when Steam is actually there. Every entry point below is
// a no-op when it is not.
//
// The flat API is the C surface of the SDK (steam_api_flat.h): plain exported
// functions taking the interface pointer explicitly, which is what makes
// GetProcAddress viable without a single Steamworks header in the tree.

// One AppID owns the process. Until this game has its own on Steamworks, 480 --
// Spacewar, Valve's public test app -- is what the client will accept from an
// unregistered build; the overlay and playtime work, but Steam names it
// Spacewar because that is the app it was told about. Set SGE_STEAM_APPID in
// the environment, or drop the real id in build/steam_appid.txt, to change it.
static constexpr const char* kDefaultSteamAppId = "480";

// Callback ids and shapes from the SDK's isteamfriends.h. They are part of the
// wire contract between the DLL and any app, so they are stable across SDK
// revisions -- but nothing here depends on getting them right: an id we do not
// recognise is skipped, and a short payload is refused before it is read.
static constexpr int kGameRichPresenceJoinRequestedCallback = 337;
static constexpr int kSteamApiCallCompletedCallback = 703;

// What the Steam client hands back through manual dispatch. Laid out to match
// CallbackMsg_t exactly; the static_assert is the guard, because a mismatch
// here reads garbage rather than failing.
struct SteamCallbackMsg {
    int32_t steamUser;
    int32_t callback;
    uint8_t* param;
    int32_t paramSize;
};
static_assert(sizeof(SteamCallbackMsg) == 24,
              "CallbackMsg_t is 24 bytes on x64; manual dispatch reads it raw");

struct SteamApi {
    HMODULE module = nullptr;
    bool initialized = false;
    // SDK 1.59 replaced the old bool SteamAPI_Init() with an init that reports
    // why it failed. Both are resolved: which one exists tells us which SDK the
    // player's Steam install shipped, and we are in no position to demand one.
    bool (*Init)() = nullptr;
    int (*InitFlat)(char* outErrorMessage) = nullptr;
    void (*RunCallbacks)() = nullptr;
    void (*Shutdown)() = nullptr;
    void* friends = nullptr;
    bool (*SetRichPresence)(void*, const char*, const char*) = nullptr;
    // Manual dispatch is the only way to read callbacks without the SDK's C++
    // headers: the client hands over raw (id, bytes) pairs and we decode the
    // one we care about. It replaces SteamAPI_RunCallbacks rather than
    // supplementing it -- calling both would eat each other's callbacks.
    void (*ManualDispatchInit)() = nullptr;
    void (*ManualDispatchRunFrame)(int32_t pipe) = nullptr;
    bool (*ManualDispatchGetNextCallback)(int32_t pipe,
                                          SteamCallbackMsg* message) = nullptr;
    void (*ManualDispatchFreeLastCallback)(int32_t pipe) = nullptr;
    int32_t (*GetHSteamPipe)() = nullptr;
    int32_t pipe = 0;
    bool manualDispatch = false;
};

static SteamApi g_steam;
// What Steam was last told. Rich presence is a network message, so it is sent
// on change rather than every frame.
static std::string g_steamStatus;

// Steam identifies a process by the AppID it finds, and a build started from
// Explorer or a debugger -- rather than from the library -- has no such id.
// steam_appid.txt next to the exe is the documented answer; the environment
// variable is the same thing without a file, which keeps a fresh clone working
// with nothing to copy. A real launch through Steam sets SteamAppId itself, and
// that one wins: it is the only one that is actually authoritative.
static void EnsureSteamAppId() {
    if (GetEnvironmentVariableA("SteamAppId", nullptr, 0) > 0) return;
    if (std::filesystem::exists("steam_appid.txt")) return;
    char configured[32] = {};
    const DWORD length =
        GetEnvironmentVariableA("SGE_STEAM_APPID", configured, sizeof(configured));
    const char* appId =
        (length > 0 && length < sizeof(configured)) ? configured : kDefaultSteamAppId;
    SetEnvironmentVariableA("SteamAppId", appId);
    SetEnvironmentVariableA("SteamGameId", appId);
}

static void InitSteam() {
    EnsureSteamAppId();
    // No DLL is the ordinary case on a machine that has never had Steam, and it
    // is not an error: the game is expected to run standalone. 64-bit only,
    // matching the only architecture this engine builds for.
    g_steam.module = LoadLibraryA("steam_api64.dll");
    if (!g_steam.module) {
        SGE_LOG("LogSteam", EngineLog::Level::Display,
            "steam_api64.dll not found; running without Steam");
        return;
    }
    g_steam.ManualDispatchInit = reinterpret_cast<void (*)()>(
        GetProcAddress(g_steam.module, "SteamAPI_ManualDispatch_Init"));
    g_steam.ManualDispatchRunFrame = reinterpret_cast<void (*)(int32_t)>(
        GetProcAddress(g_steam.module, "SteamAPI_ManualDispatch_RunFrame"));
    g_steam.ManualDispatchGetNextCallback =
        reinterpret_cast<bool (*)(int32_t, SteamCallbackMsg*)>(
            GetProcAddress(g_steam.module,
                           "SteamAPI_ManualDispatch_GetNextCallback"));
    g_steam.ManualDispatchFreeLastCallback =
        reinterpret_cast<void (*)(int32_t)>(
            GetProcAddress(g_steam.module,
                           "SteamAPI_ManualDispatch_FreeLastCallback"));
    g_steam.GetHSteamPipe = reinterpret_cast<int32_t (*)()>(
        GetProcAddress(g_steam.module, "SteamAPI_GetHSteamPipe"));
    // Resolve support here; manual dispatch must be initialized only after
    // SteamAPI_Init succeeds, or join and connection callbacks are lost.
    g_steam.manualDispatch =
        g_steam.ManualDispatchInit && g_steam.ManualDispatchRunFrame &&
        g_steam.ManualDispatchGetNextCallback &&
        g_steam.ManualDispatchFreeLastCallback && g_steam.GetHSteamPipe;

    g_steam.Init = reinterpret_cast<bool (*)()>(
        GetProcAddress(g_steam.module, "SteamAPI_Init"));
    g_steam.InitFlat = reinterpret_cast<int (*)(char*)>(
        GetProcAddress(g_steam.module, "SteamAPI_InitFlat"));
    g_steam.RunCallbacks = reinterpret_cast<void (*)()>(
        GetProcAddress(g_steam.module, "SteamAPI_RunCallbacks"));
    g_steam.Shutdown = reinterpret_cast<void (*)()>(
        GetProcAddress(g_steam.module, "SteamAPI_Shutdown"));
    if (!g_steam.RunCallbacks || !g_steam.Shutdown ||
        (!g_steam.Init && !g_steam.InitFlat)) {
        SGE_LOG("LogSteam", EngineLog::Level::Warning,
            "steam_api64.dll is missing the flat API exports; ignoring it");
        FreeLibrary(g_steam.module);
        g_steam.module = nullptr;
        return;
    }

    if (g_steam.InitFlat) {
        // SteamErrMsg is a 1024-byte buffer the SDK writes a sentence into.
        // Logging it verbatim is the difference between "Steam failed" and
        // "the client is not running", which are fixed very differently.
        char error[1024] = {};
        const int result = g_steam.InitFlat(error);
        g_steam.initialized = result == 0; // k_ESteamAPIInitResult_OK
        if (!g_steam.initialized)
            SGE_LOG("LogSteam", EngineLog::Level::Display,
                std::string("Steam init failed (") + std::to_string(result) +
                "): " + error);
    } else {
        g_steam.initialized = g_steam.Init();
        if (!g_steam.initialized)
            SGE_LOG("LogSteam", EngineLog::Level::Display,
                "Steam init failed; is the client running?");
    }
    if (!g_steam.initialized) {
        // A failed init is the normal state when Steam is installed but closed,
        // so this stays a Display line and the game carries on regardless.
        FreeLibrary(g_steam.module);
        g_steam.module = nullptr;
        return;
    }

    // The friends interface is versioned in its accessor name, and the version
    // belongs to whichever SDK built the DLL on this machine -- not to us. Try
    // the ones in circulation, newest first, and treat a miss as "no rich
    // presence": playtime and the overlay do not depend on it.
    static const char* const kFriendsAccessors[] = {
        "SteamAPI_SteamFriends_v018",
        "SteamAPI_SteamFriends_v017",
        "SteamAPI_SteamFriends_v015",
    };
    for (const char* accessor : kFriendsAccessors) {
        auto resolve = reinterpret_cast<void* (*)()>(
            GetProcAddress(g_steam.module, accessor));
        if (!resolve) continue;
        g_steam.friends = resolve();
        if (g_steam.friends) break;
    }
    g_steam.SetRichPresence = reinterpret_cast<bool (*)(void*, const char*,
                                                        const char*)>(
        GetProcAddress(g_steam.module,
                       "SteamAPI_ISteamFriends_SetRichPresence"));

    if (g_steam.manualDispatch) {
        g_steam.ManualDispatchInit();
        g_steam.pipe = g_steam.GetHSteamPipe();
    }

    char appId[32] = {};
    GetEnvironmentVariableA("SteamAppId", appId, sizeof(appId));
    SGE_LOG("LogSteam", EngineLog::Level::Display,
        std::string("Steam connected as app ") + appId +
        (g_steam.friends && g_steam.SetRichPresence
             ? "" : " (no rich presence interface)"));
}

// Sends a status line, but only when it has changed: rich presence goes over
// the wire to the friends network, and re-sending the same string every frame
// would be pure traffic.
//
// Whether anyone sees it depends on the AppID. Steam renders the `steam_display`
// token through the localization strings configured for that app on the partner
// site, so an unregistered build (or Spacewar) has nothing to render and shows
// nothing -- the call still succeeds. `status` is the legacy key kept alongside
// it because some surfaces still read it.
static void SetSteamStatus(const std::string& status) {
    if (!g_steam.initialized || !g_steam.friends || !g_steam.SetRichPresence)
        return;
    if (status == g_steamStatus) return;
    g_steamStatus = status;
    g_steam.SetRichPresence(g_steam.friends, "status", status.c_str());
    g_steam.SetRichPresence(g_steam.friends, "steam_display",
                            "#Status_Generic");
}

// This machine's address as a friend would have to dial it. The loopback and
// link-local addresses are skipped because handing a friend 127.0.0.1 gives
// them a connect button that quietly dials their own machine.
static std::string LocalNetworkAddress() {
    WSADATA winsock = {};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return {};
    std::string best;
    char name[256] = {};
    if (gethostname(name, sizeof(name)) == 0) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        addrinfo* results = nullptr;
        if (getaddrinfo(name, nullptr, &hints, &results) == 0) {
            for (addrinfo* entry = results; entry; entry = entry->ai_next) {
                char text[INET_ADDRSTRLEN] = {};
                auto* address = reinterpret_cast<sockaddr_in*>(entry->ai_addr);
                if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)))
                    continue;
                const std::string candidate(text);
                if (candidate.rfind("127.", 0) == 0) continue;
                if (candidate.rfind("169.254.", 0) == 0) continue;
                best = candidate;
                break;
            }
            freeaddrinfo(results);
        }
    }
    WSACleanup();
    return best;
}

// The `connect` key is what puts "Join Game" in a friend's list. Steam hands
// this exact string back to the joining machine -- as launch arguments if the
// game is closed, as a callback if it is already running -- and both ends are
// parsed by JoinFromConnectString.
//
// A client advertises the host it is connected to, not itself: "join my game"
// means the session, and there is only one machine in it accepting players.
static void UpdateSteamConnectString() {
    if (!g_steam.initialized || !g_steam.friends || !g_steam.SetRichPresence)
        return;
    static std::string advertised;
    std::string connect;
    if (MultiplayerActive()) {
        // A relayed session is addressed by SteamID and needs no port: that is
        // the whole point of it, and it is the only form a friend outside this
        // network can act on.
        const std::string address = g_netSession.ConnectAddress();
        if (g_netSession.OverSteam()) {
            if (!address.empty()) connect = "+connect " + address;
        } else {
            std::string ip = address;
            if (ip.empty()) {
                // IP host: resolved once. The adapter list does not change
                // often, and this walks winsock every time it is asked.
                static const std::string local = LocalNetworkAddress();
                ip = local;
            }
            if (!ip.empty())
                connect = "+connect " + ip + ":" +
                          std::to_string(g_netSession.Port());
        }
    }
    if (connect == advertised) return;
    // Retry next frame if Steam has not accepted the address yet.
    if (!g_steam.SetRichPresence(g_steam.friends, "connect", connect.c_str()))
        return;
    advertised = connect;
    // What a friend's client will actually be handed. Logged because the only
    // other way to find out is to have a friend try it and report back.
    SGE_LOG("LogSteam", EngineLog::Level::Display,
        connect.empty() ? std::string("cleared the Steam connect string")
                        : "advertising Steam connect string: " + connect);
}

// Reads the callbacks the client has queued for us. Manual dispatch is raw: the
// id says what the bytes are, and anything unrecognised has to be freed anyway
// or the queue stops moving.
static void PumpSteamCallbacks() {
    g_steam.ManualDispatchRunFrame(g_steam.pipe);
    SteamCallbackMsg message = {};
    while (g_steam.ManualDispatchGetNextCallback(g_steam.pipe, &message)) {
        // Connection state for the P2P transport arrives here too: it is a
        // process-wide callback, and this pump is the only thing reading them.
        net::SteamTransportHandleCallback(message.callback, message.param,
                                          message.paramSize);
        if (message.callback == kGameRichPresenceJoinRequestedCallback &&
            message.param && message.paramSize >= 9) {
            // GameRichPresenceJoinRequested_t: a CSteamID, then the friend's
            // `connect` string. Bounded by the reported payload rather than by
            // a terminator we have to hope is there.
            const char* connect =
                reinterpret_cast<const char*>(message.param + 8);
            const size_t available = size_t(message.paramSize) - 8;
            const std::string request(connect, strnlen(connect, available));
            SGE_LOG("LogSteam", EngineLog::Level::Display,
                "Steam join requested: " + request);
            // Leaves the level alone: the session tells this client which map
            // the host is on, and FollowHostLevel loads it.
            JoinFromConnectString(request);
        }
        g_steam.ManualDispatchFreeLastCallback(g_steam.pipe);
    }
}

// Once a frame. Running callbacks is what lets the client deliver anything it
// has for us -- overlay activation, a friend's join request -- and skipping it
// is how an integration ends up looking connected while doing nothing.
static void UpdateSteam() {
    if (!g_steam.initialized) return;
    if (g_steam.manualDispatch) PumpSteamCallbacks();
    else g_steam.RunCallbacks();
    UpdateSteamConnectString();
    const char* screen = "In the menus";
    switch (g_game.session.Screen()) {
    case GameScreen::Level1: screen = "In a mission"; break;
    case GameScreen::LevelEditor: screen = "In the level editor"; break;
    case GameScreen::WinScreen:
        screen = g_missionFailReason.empty() ? "Mission complete"
                                             : "Mission failed";
        break;
    default: break;
    }
    // Multiplayer is worth saying out loud: it is the one state where a friend
    // reading the line might act on it.
    SetSteamStatus(MultiplayerActive() ? std::string(screen) + " (multiplayer)"
                                       : std::string(screen));
}

static void ShutdownSteam() {
    if (g_steam.initialized) {
        g_steam.initialized = false;
        g_steam.Shutdown();
    }
    if (g_steam.module) {
        FreeLibrary(g_steam.module);
        g_steam.module = nullptr;
    }
}
