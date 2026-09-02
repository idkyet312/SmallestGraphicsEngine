#ifndef PLAYER_STATE_H
#define PLAYER_STATE_H

#include "WeaponCustomization.h"

struct PlayerState {
    float maxHealth = 100.0f;
    float health = 100.0f;
    float damageFlash = 0.0f;
    // How hard the last hit landed, 0..1, relative to a full-health kill. Drives
    // the strength of every hit reaction rather than each one re-deriving it, so
    // a rifle graze and a point-blank grenade read as different events instead
    // of firing the same canned flash.
    float damageFlashSeverity = 0.0f;
    // Where the hit came from, in world space, normalised and pointing from the
    // player toward the attacker. Zero when the source is unknown (burning,
    // falling), which the indicator treats as "no direction to show".
    float lastHitDirX = 0.0f;
    float lastHitDirZ = 0.0f;
    // Separate, slower timer than damageFlash: the directional wedge has to
    // outlive the screen flash to be readable, or it is gone before the eye
    // finds it.
    float hitIndicator = 0.0f;
    // Low-health state. Ramps in below a threshold and drives the vignette and
    // heartbeat, so being nearly dead is legible without reading the HP bar.
    float lowHealthPulse = 0.0f;
    // Off by default: a run is meant to be survivable, not unlosable, and the
    // ammo/reload systems below only do anything when it is off (see
    // AmmoEnforced). The deployment screen offers it as an explicit choice per
    // run, StartLevelOne still takes it as a parameter, and the debug UI
    // checkbox toggles it live.
    bool godMode = false;
    bool healthRegen = true;
    float regenDelay = 5.0f;
    float regenDuration = 2.0f;
    float regenTimer = 0.0f;

    float HealthRegenPerSecond() const {
        return regenDuration > 0.0f ? maxHealth / regenDuration : maxHealth;
    }

    static constexpr int kWeaponSlots = SGE::WeaponCustomizationSystem::kWeaponCount;
    // The instance is now the one source of truth for both ammo and installed
    // parts. A dropped weapon can therefore carry its exact configuration,
    // instead of rebuilding attachments from the selected weapon ID.
    SGE::WeaponCustomizationSystem weapons;
    float reloadTimer = 0.0f;
    int reloadingSlot = -1;

    bool AmmoEnforced() const { return !godMode; }
    bool Reloading() const { return reloadTimer > 0.0f; }

    SGE::WeaponInstance* Weapon(int slot) { return weapons.Instance(slot); }
    const SGE::WeaponInstance* Weapon(int slot) const {
        return weapons.Instance(slot);
    }
    SGE::ResolvedWeaponStats ResolveWeaponStats(int slot) const {
        return weapons.Resolve(slot);
    }
    int Magazine(int slot) const {
        const SGE::WeaponInstance* instance = Weapon(slot);
        return instance ? instance->magazine : 0;
    }
    int Reserve(int slot) const {
        const SGE::WeaponInstance* instance = Weapon(slot);
        return instance ? instance->reserve : 0;
    }
    int MagazineSize(int slot) const {
        return ResolveWeaponStats(slot).magazineCapacity;
    }
    int MaxReserve(int slot) const {
        return ResolveWeaponStats(slot).maximumReserve;
    }
    float ReloadTime(int slot) const {
        return ResolveWeaponStats(slot).reloadSeconds;
    }
    bool SetAmmo(int slot, int magazine, int reserve) {
        SGE::WeaponInstance* instance = Weapon(slot);
        if (!instance) return false;
        const SGE::ResolvedWeaponStats stats = ResolveWeaponStats(slot);
        instance->magazine = std::clamp(magazine, 0, stats.magazineCapacity);
        instance->reserve = std::clamp(reserve, 0, stats.maximumReserve);
        return true;
    }

    void RestoreAmmo() {
        weapons.RestoreAmmo();
        reloadTimer = 0.0f;
        reloadingSlot = -1;
    }

    void HalveAmmo() {
        weapons.HalveAmmo();
        reloadTimer = 0.0f;
        reloadingSlot = -1;
    }

    bool BeginReload(int slot) {
        if (!AmmoEnforced() || Reloading()) return false;
        SGE::WeaponInstance* instance = Weapon(slot);
        if (!instance) return false;
        const SGE::ResolvedWeaponStats stats = ResolveWeaponStats(slot);
        if (instance->magazine >= stats.magazineCapacity ||
            instance->reserve <= 0)
            return false;
        reloadingSlot = slot;
        reloadTimer = stats.reloadSeconds;
        return true;
    }

    void UpdateReload(float dt) {
        if (!Reloading()) return;
        reloadTimer -= dt;
        if (reloadTimer > 0.0f) return;
        reloadTimer = 0.0f;
        const int slot = reloadingSlot;
        reloadingSlot = -1;
        SGE::WeaponInstance* instance = Weapon(slot);
        if (!instance) return;
        const int needed = ResolveWeaponStats(slot).magazineCapacity -
                           instance->magazine;
        const int moved = (needed < instance->reserve) ?
            needed : instance->reserve;
        instance->magazine += moved;
        instance->reserve -= moved;
    }

    bool ConsumeAmmo(int slot) {
        if (!AmmoEnforced()) return true;
        SGE::WeaponInstance* instance = Weapon(slot);
        // Preserve the old invalid-slot behaviour: non-ammo utility actions
        // outside the table are not blocked by the ammo system.
        if (!instance) return true;
        if (Reloading() || instance->magazine <= 0) return false;
        --instance->magazine;
        return true;
    }
};

#endif
