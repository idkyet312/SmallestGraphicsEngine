#ifndef ARMORY_CATALOG_H
#define ARMORY_CATALOG_H

// Storefront data for the deployment screen's armory. The deploy panel used to
// hand out every weapon, grenade and gear item for free through three combo
// boxes; the wallet in MoneySystem had nothing to spend on. This table is the
// other half of that system -- it puts a price on every pick so the career
// balance is a budget rather than a scoreboard.
//
// Prices, not purchases: the catalog is a pure lookup. Whether an item is owned,
// what a loadout costs and what gets deducted are all decided by the caller
// (the deploy screen), which keeps this header free of game state and testable
// on its own.
//
// Ordering the tiers: every firearm is bought, including the AK-74 that used to
// be handed out free as "standard issue". The starting balance
// (MoneySystem::kStartingBalance, 2000) is the first loadout's budget rather
// than pocket money on top of a free rifle, so the opening choice is a real one
// -- one good weapon, or two cheap ones. Prices are set against the kill reward
// (MoneySystem::kEnemyKillReward, 120): a mid-tier weapon is roughly a dozen
// kills, the RPG is a comm tower.
//
// The remote charge (slot 5) stays at zero and stays the exception. It is
// demolition kit issued on every mission rather than a weapon choice -- see
// GunModel::kRemoteChargeWeapon -- and pricing it would make a demolition
// objective unreachable for a player who spent their balance on rifles.

#include <cstdint>

#include "MissionSystem.h"

struct ArmoryCatalog {
    // Weapon prices indexed by the same weapon ids MissionLoadout::weapons
    // stores, so a price lookup never needs a name match. Slots that are hidden
    // from selection (the retired AK-47 at 0, the retired suppressed R700 at 8)
    // carry a price anyway rather than a sentinel: an unreachable hole in the
    // table would be one more thing to keep in sync with GunModel's gating.
    static constexpr int kWeaponPrices[MissionLoadout::kWeaponCount] = {
        0,      // 0  AK47 (retired, never offered)
        900,    // 1  Remington 870
        4200,   // 2  RPG-7
        2600,   // 3  R700 Sniper
        3400,   // 4  ARC Laser Cutter (debug)
        0,      // 5  Remote C4 (always issued, never bought)
        3800,   // 6  M2 Flamethrower (debug)
        3100,   // 7  Mako Harpoon Gun (debug)
        3000,   // 8  R700 Suppressed (retired)
        1500,   // 9  M4A1
        800,    // 10 AK-74
        700,    // 11 M9
        1800,   // 12 Kriss Vector
    };

    // One line of shop copy per weapon. The combo box gave a bare name and left
    // the player to find out what a pick did in the field; a storefront row has
    // the space to say it up front.
    static constexpr const char* kWeaponBlurbs[MissionLoadout::kWeaponCount] = {
        "Retired from service.",
        "Pump shotgun. Devastating inside a room, useless past one.",
        "Rocket launcher. One shot answers a vehicle or a bunkered squad.",
        "Bolt-action rifle. Scoped, one hit, and a laser the enemy can see.",
        "Cutting torch. Development hardware.",
        "Demolition charge. Issued on every mission.",
        "Flamethrower. Development hardware.",
        "Harpoon launcher. Development hardware.",
        "Retired from service.",
        "Carbine. Flatter recoil than the AK and faster to aim.",
        "Battle rifle. Heavy round, steady cadence, takes every rail.",
        "Sidearm. Draws and reloads faster than any rifle, and hits softer.",
        "SMG. Empties a magazine faster than anything else, but hits softest at range.",
    };

    static constexpr int kGrenadeCount = 3;
    // Frag stays free, on the same terms as the remote charge: it is the
    // issued grenade rather than a purchase, so a player who spent everything
    // on a rifle still has something to throw.
    static constexpr int kGrenadePrices[kGrenadeCount] = { 0, 450, 1200 };
    static constexpr const char* kGrenadeNames[kGrenadeCount] = {
        "Frag Grenade", "Molotov Cocktail", "Vortex Grenade"
    };
    static constexpr const char* kGrenadeBlurbs[kGrenadeCount] = {
        "Issued fragmentation grenade. Wide lethal radius.",
        "Improvised incendiary. Denies ground long after the throw.",
        "Experimental singularity charge. Pulls a group into one point.",
    };

    static constexpr int kGearCount = 3;
    // None is the empty slot rather than a product, so it is priced at zero and
    // rendered as "carry nothing" instead of a purchase.
    static constexpr int kGearPrices[kGearCount] = { 0, 1800, 600 };
    static constexpr const char* kGearNames[kGearCount] = {
        "No Gear", "Night Vision Goggles", "Weapon Flashlight"
    };
    static constexpr const char* kGearBlurbs[kGearCount] = {
        "Leave the slot empty.",
        "Amplifies starlight. Turns a night insertion into a daylight one.",
        "Barrel-mounted lamp. Lights what you aim at -- and marks where you are.",
    };

    static int WeaponPrice(int weapon) {
        if (weapon < 0 || weapon >= MissionLoadout::kWeaponCount) return 0;
        return kWeaponPrices[weapon];
    }

    static const char* WeaponBlurb(int weapon) {
        if (weapon < 0 || weapon >= MissionLoadout::kWeaponCount) return "";
        return kWeaponBlurbs[weapon];
    }

    static int GrenadePrice(GrenadeType grenade) {
        const int index = static_cast<int>(grenade);
        if (index < 0 || index >= kGrenadeCount) return 0;
        return kGrenadePrices[index];
    }

    static int GearPrice(GearType gear) {
        const int index = static_cast<int>(gear);
        if (index < 0 || index >= kGearCount) return 0;
        return kGearPrices[index];
    }

    // Attachments are keyed by string id rather than an index, because
    // WeaponCustomization loads them from data and their order is not fixed.
    // Matched on the id's meaning-bearing substring so a renamed variant
    // ("acog_scope" vs "scope_acog") still lands on the right tier instead of
    // silently falling through to free.
    static int AttachmentPrice(bool suppresses, bool redDot, bool laser) {
        if (suppresses) return 1400;
        if (redDot) return 800;
        if (laser) return 500;
        return 350;
    }
};

#endif
