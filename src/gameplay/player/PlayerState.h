#ifndef PLAYER_STATE_H
#define PLAYER_STATE_H

struct PlayerState {
    float maxHealth = 100.0f;
    float health = 100.0f;
    float damageFlash = 0.0f;
    bool godMode = false;
    bool healthRegen = true;
    float regenDelay = 5.0f;
    float regenRate = 12.0f;
    float regenTimer = 0.0f;

    static constexpr int kWeaponSlots = 7;
    int magazineSize[kWeaponSlots] = { 30, 8, 1, 10, 120, 6, 100 };
    int maxReserve[kWeaponSlots] = { 240, 64, 8, 80, 600, 24, 500 };
    float reloadTime[kWeaponSlots] = { 1.55f, 2.4f, 2.8f, 1.75f, 2.1f, 1.8f, 2.6f };
    int magazine[kWeaponSlots] = { 30, 8, 1, 10, 120, 6, 100 };
    int reserve[kWeaponSlots] = { 120, 32, 4, 40, 360, 12, 300 };
    float reloadTimer = 0.0f;
    int reloadingSlot = -1;

    bool AmmoEnforced() const { return !godMode; }
    bool Reloading() const { return reloadTimer > 0.0f; }

    void RestoreAmmo() {
        for (int i = 0; i < kWeaponSlots; ++i) {
            magazine[i] = magazineSize[i];
            reserve[i] = maxReserve[i];
        }
        reloadTimer = 0.0f;
        reloadingSlot = -1;
    }

    bool BeginReload(int slot) {
        if (!AmmoEnforced() || Reloading()) return false;
        if (slot < 0 || slot >= kWeaponSlots) return false;
        if (magazine[slot] >= magazineSize[slot] || reserve[slot] <= 0)
            return false;
        reloadingSlot = slot;
        reloadTimer = reloadTime[slot];
        return true;
    }

    void UpdateReload(float dt) {
        if (!Reloading()) return;
        reloadTimer -= dt;
        if (reloadTimer > 0.0f) return;
        reloadTimer = 0.0f;
        const int slot = reloadingSlot;
        reloadingSlot = -1;
        if (slot < 0 || slot >= kWeaponSlots) return;
        const int needed = magazineSize[slot] - magazine[slot];
        const int moved = (needed < reserve[slot]) ? needed : reserve[slot];
        magazine[slot] += moved;
        reserve[slot] -= moved;
    }

    bool ConsumeAmmo(int slot) {
        if (!AmmoEnforced()) return true;
        if (slot < 0 || slot >= kWeaponSlots) return true;
        if (Reloading() || magazine[slot] <= 0) return false;
        --magazine[slot];
        return true;
    }
};

#endif
