#pragma once

// Private application implementation; included once by main.cpp in dependency
// order.
//
// The translation layer between NetSession and the game's globals. NetSession
// deliberately knows nothing about the camera, SkinnedEnemy or the AppState
// globals, so that conversion has to happen somewhere -- it happens here, in
// one file, rather than being spread through main.cpp.

#include <sstream>

static net::NetSession g_netSession;
// The local player's intent for this frame, published by ProcessInput. Held
// here rather than passed down through the frame so the multiplayer update can
// be a single call at one clear point in the loop.
static PlayerInput g_localPlayerInput;
// Remote player bodies, keyed by the id the host assigned. They live in
// g_bandits alongside the AI actors so they are drawn, lit and shadowed by the
// existing paths, but they are flagged networkControlled so the AI skips them.
static std::unordered_map<net::PlayerId, SkinnedEnemy*> g_netPlayerBodies;
// Reused across frames so the per-frame snapshot read does not allocate.
static std::vector<net::RemotePlayer> g_netRemoteScratch;

static bool MultiplayerActive() { return g_netSession.Active(); }

// Spawns the body for a player that just appeared in a snapshot. Mirrors
// SpawnMarine: same model, same faction, so the remote player is rendered by
// the ally path that already exists rather than by new rendering code.
static SkinnedEnemy* SpawnNetworkPlayerBody(net::PlayerId id) {
    if (!g_marineModel.valid) return nullptr;
    auto body = std::make_unique<SkinnedEnemy>();
    if (!body->Init(g_marineModel)) return nullptr;
    body->faction = Faction::Marine;
    body->networkControlled = true;
    char callsign[32];
    std::snprintf(callsign, sizeof(callsign), "Player-%d",
                  static_cast<int>(id) + 1);
    body->callsign = callsign;
    body->leftArmReach = g_banditLeftArmReach;
    body->health = 100.0f;
    // Milestone 1 does not replicate damage, so a remote body must not be
    // killable locally -- one client shooting it would desync every other
    // client's view of that player. Combat replication is milestone 2.
    // Scaling incoming damage to zero routes through the existing path rather
    // than adding a second notion of invulnerability.
    body->damageTakenScale = 0.0f;
    SkinnedEnemy* raw = body.get();
    g_bandits.push_back(std::move(body));
    g_netPlayerBodies[id] = raw;
    return raw;
}

static void RemoveNetworkPlayerBody(net::PlayerId id) {
    const auto it = g_netPlayerBodies.find(id);
    if (it == g_netPlayerBodies.end()) return;
    SkinnedEnemy* body = it->second;
    g_netPlayerBodies.erase(it);
    for (auto entry = g_bandits.begin(); entry != g_bandits.end(); ++entry) {
        if (entry->get() != body) continue;
        g_bandits.erase(entry);
        return;
    }
}

static void ShutdownMultiplayer() {
    for (const auto& entry : g_netPlayerBodies) {
        SkinnedEnemy* body = entry.second;
        for (auto it = g_bandits.begin(); it != g_bandits.end(); ++it) {
            if (it->get() != body) continue;
            g_bandits.erase(it);
            break;
        }
    }
    g_netPlayerBodies.clear();
    g_netSession.Shutdown();
}

// Parses -host [port] / -join <address> [port] out of the command line and
// starts the session. A failure is reported and then ignored: not being able
// to host is a reason to stay single-player, not a reason to refuse to launch.
static void StartMultiplayerFromCommandLine(const std::string& commandLine) {
    std::vector<std::string> arguments;
    std::istringstream stream(commandLine);
    for (std::string token; stream >> token;) arguments.push_back(token);

    constexpr uint16_t kDefaultPort = 27015;
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string& argument = arguments[i];
        std::string error;
        if (argument == "-host") {
            uint16_t port = kDefaultPort;
            if (i + 1 < arguments.size() && arguments[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(arguments[++i].c_str()));
            if (g_netSession.StartHost(port, &error))
                std::cout << "Hosting on port " << port << "\n";
            else
                std::cerr << "Failed to host: " << error << "\n";
            return;
        }
        if (argument == "-join") {
            if (i + 1 >= arguments.size()) {
                std::cerr << "-join needs an address\n";
                return;
            }
            const std::string address = arguments[++i];
            uint16_t port = kDefaultPort;
            if (i + 1 < arguments.size() && arguments[i + 1][0] != '-')
                port = static_cast<uint16_t>(std::atoi(arguments[++i].c_str()));
            if (g_netSession.StartClient(address, port, &error))
                std::cout << "Joining " << address << ":" << port << "\n";
            else
                std::cerr << "Failed to join: " << error << "\n";
            return;
        }
    }
}

// Once per frame, after the local player has moved. Feeds the session what the
// local player did, then applies what came back to the remote bodies.
static void UpdateMultiplayer(float frameDelta, const PlayerInput& localInput) {
    if (!MultiplayerActive()) return;

    net::LocalPlayerState local;
    local.input = localInput;
    local.x = scene.camera.Position.x;
    // Report the feet rather than the eye: a remote body is placed by its feet,
    // and sending the eye would sink every other player waist-deep in terrain.
    local.y = scene.camera.Position.y - scene.camera.PlayerHeight;
    local.z = scene.camera.Position.z;
    g_netSession.Update(frameDelta, local);

    // The host owns remote players' movement: it integrates them from the
    // input they sent rather than from a position they claimed, then feeds the
    // result back so the next snapshot carries it. Milestone 1 keeps that
    // integration deliberately simple -- flat ground-follow, no collision --
    // because the point is to prove the pipe, and a full second mover is
    // milestone 2's problem.
    if (g_netSession.CurrentRole() == net::Role::Host) {
        for (const auto& entry : g_netPlayerBodies) {
            const net::PlayerId id = entry.first;
            const PlayerInput* input = g_netSession.PendingInput(id);
            if (!input) continue;
            SkinnedEnemy* body = entry.second;
            const float yawRadians = DirectX::XMConvertToRadians(input->yaw);
            const float forwardX = std::sin(yawRadians);
            const float forwardZ = std::cos(yawRadians);
            const float speed = scene.camera.MovementSpeed *
                                input->movementMultiplier * input->deltaTime;
            body->position.x +=
                (forwardX * input->forward - forwardZ * input->strafe) * speed;
            body->position.z +=
                (forwardZ * input->forward + forwardX * input->strafe) * speed;
            auto params = CurrentTerrainParams();
            params.heightScale = scene.terrainHeightScale;
            body->position.y = TerrainRendererDX12::HeightAt(
                params, body->position.x, body->position.z);
            g_netSession.SetPlayerPosition(id, body->position.x,
                                           body->position.y, body->position.z);
            g_netSession.ClearPendingInput(id);
        }
    }

    // Apply the session's interpolated view to the bodies. On the host this
    // only moves other players; on a client it moves everyone but the local
    // player, whose own position stays locally predicted.
    g_netSession.GetRemotePlayers(g_netRemoteScratch);
    for (const net::RemotePlayer& remote : g_netRemoteScratch) {
        SkinnedEnemy* body = nullptr;
        const auto it = g_netPlayerBodies.find(remote.id);
        if (it == g_netPlayerBodies.end()) {
            body = SpawnNetworkPlayerBody(remote.id);
            if (!body) continue;
            // Start exactly where the snapshot says instead of interpolating
            // in from the origin, which would drag the new body across the map.
            body->position = { remote.x, remote.y, remote.z };
        } else {
            body = it->second;
        }
        // The host already integrated its own remote bodies above; overwriting
        // them with the snapshot it just produced would be circular.
        if (g_netSession.CurrentRole() != net::Role::Host)
            body->position = { remote.x, remote.y, remote.z };
        // SkinnedEnemy stores facing in radians; the wire carries degrees, in
        // the same units the camera uses.
        const float yawRadians = DirectX::XMConvertToRadians(remote.yaw);
        body->yaw = yawRadians;
        body->aimYaw = yawRadians;
        body->UpdateNetworkedPose(frameDelta, remote.moving, remote.sprinting);
    }

    // Drop bodies for players that are no longer in the session.
    for (auto it = g_netPlayerBodies.begin(); it != g_netPlayerBodies.end();) {
        bool stillPresent = false;
        for (const net::RemotePlayer& remote : g_netRemoteScratch)
            if (remote.id == it->first) { stillPresent = true; break; }
        if (stillPresent) { ++it; continue; }
        const net::PlayerId id = it->first;
        ++it;
        RemoveNetworkPlayerBody(id);
    }
}
