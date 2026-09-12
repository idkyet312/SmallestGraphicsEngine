#pragma once

// Small opt-in multiplayer regression: kill one replicated enemy, then keep
// both copies alive long enough to prove the corpse palette is still moving.
// The frame loop calls UpdateEnemyDeathSmoke only after the level is ready.

namespace EnemyDeathSmoke {

struct State {
    bool active = false;
    bool reported = false;
    bool sawDead = false;
    bool haveBaseline = false;
    bool finished = false;
    uint32_t frames = 0;
    uint32_t deadFrames = 0;
    net::EnemyId target = net::kInvalidEnemyId;
    std::vector<DirectX::XMFLOAT4X4> baseline;
};

inline State& GetState() {
    static State state;
    return state;
}

inline bool Enabled() {
    char value[4] = {};
    return GetEnvironmentVariableA("SGE_ENEMY_DEATH_TEST", value,
                                   static_cast<DWORD>(sizeof(value))) > 0 &&
           value[0] == '1';
}

inline void Log(bool pass, const char* detail) {
    SGE_LOG("LogGameplay", pass ? EngineLog::Level::Display
                                 : EngineLog::Level::Error,
            std::string("Enemy death smoke ") + (pass ? "PASS: " : "FAIL: ") +
            detail);
}

inline SkinnedEnemy* FindTarget(State& state) {
    if (state.target != net::kInvalidEnemyId) {
        for (const auto& actor : g_bandits) {
            if (actor && !actor->networkControlled &&
                actor->netEnemyId == state.target)
                return actor.get();
        }
    }
    for (const auto& actor : g_bandits) {
        if (actor && !actor->networkControlled &&
            actor->netEnemyId != net::kInvalidEnemyId)
            return actor.get();
    }
    return nullptr;
}

inline void PointCamera(const SkinnedEnemy& enemy) {
    using namespace DirectX;
    const XMFLOAT3 eye{enemy.position.x, enemy.position.y + 1.5f,
                       enemy.position.z + 4.0f};
    const XMFLOAT3 delta{enemy.position.x - eye.x, enemy.position.y + 1.0f - eye.y,
                         enemy.position.z - eye.z};
    scene.camera.Position = eye;
    scene.camera.SetViewAngles(
        XMConvertToDegrees(std::atan2(delta.z, delta.x)),
        XMConvertToDegrees(std::atan2(
            delta.y, std::sqrt(delta.x * delta.x + delta.z * delta.z))));
}

inline bool PaletteFiniteAndChanged(const std::vector<DirectX::XMFLOAT4X4>& a,
                                    const std::vector<DirectX::XMFLOAT4X4>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    bool changed = false;
    const float* ap = reinterpret_cast<const float*>(a.data());
    const float* bp = reinterpret_cast<const float*>(b.data());
    const size_t count = a.size() * 16;
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(ap[i]) || !std::isfinite(bp[i])) return false;
        changed |= std::abs(ap[i] - bp[i]) > 1e-4f;
    }
    return changed;
}

inline void StartEnemyDeathSmoke(HWND hwnd) {
    State& state = GetState();
    if (state.active || !Enabled()) return;
    state = State{};
    state.active = true;
    StartLevelOne(hwnd, true, false, true, nullptr, true);
    Log(true, "started");
}

inline void UpdateEnemyDeathSmoke() {
    State& state = GetState();
    if (!state.active || state.finished) return;
    ++state.frames;
    if (state.frames > 1800) {
        state.finished = true;
        Log(false, "timeout waiting for connected players or ragdoll");
        PostQuitMessage(1);
        return;
    }

    std::vector<net::RemotePlayer> players;
    g_netSession.GetRemotePlayers(players);
    if (players.empty()) return;
    SkinnedEnemy* enemy = FindTarget(state);
    if (!enemy) return;
    PointCamera(*enemy);
    if (!state.reported && g_netSession.CurrentRole() == net::Role::Client) {
        state.target = enemy->netEnemyId;
        g_netSession.ReportEnemyHit(state.target, 100.0f, true,
                                    0.0f, 0.0f, 1.0f,
                                    enemy->position.x, enemy->position.y + 1.0f,
                                    enemy->position.z);
        state.reported = true;
        Log(true, "client reported lethal headshot");
        return;
    }
    if (!enemy->Dead() || enemy->RagdollId() == UINT32_MAX) return;
    if (!state.sawDead) {
        state.sawDead = true;
        state.deadFrames = 0;
        state.baseline = enemy->Palette();
        state.haveBaseline = true;
        return;
    }
    ++state.deadFrames;
    if (state.deadFrames == 120) {
        const bool moved = state.haveBaseline &&
            PaletteFiniteAndChanged(state.baseline, enemy->Palette());
        if (!moved) {
            state.finished = true;
            Log(false, "dead enemy palette did not change or became non-finite");
            PostQuitMessage(1);
            return;
        }
        Log(true, "dead enemy ragdoll palette moved for 120 frames");
    }
    if (state.deadFrames >= 240) {
        state.finished = true;
        Log(true, g_netSession.CurrentRole() == net::Role::Host
                       ? "host observed replicated ragdoll"
                       : "client observed replicated ragdoll");
        PostQuitMessage(0);
    }
}

} // namespace EnemyDeathSmoke

using EnemyDeathSmoke::StartEnemyDeathSmoke;
using EnemyDeathSmoke::UpdateEnemyDeathSmoke;
