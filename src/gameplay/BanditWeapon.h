#pragma once
// The enemy loadout class, split out of SkinnedEnemy.h so it can be included
// without dragging in DX12, the FBX importer and the navigation system.
//
// Three consumers need this and only this: the runtime (which applies the
// loadout at spawn), the level format (which serialises the authored choice as
// a string), and the tests that pin those two together. The editor deliberately
// still works in raw strings -- it has no business including gameplay headers --
// which is exactly why the round-trip test matters.
#include <string>

// Loadout class. Rifle is the original bandit behaviour; the other two change
// engagement range, damage, and the shape of a shot rather than the model.
enum class BanditWeapon {
    Rifle,
    Shotgun,
    Sniper,
};

// Stable strings for the level format and the editor. Kept beside the enum so a
// new class cannot be added without a name to serialize it under.
inline const char* BanditWeaponName(BanditWeapon weapon) {
    switch (weapon) {
    case BanditWeapon::Shotgun: return "shotgun";
    case BanditWeapon::Sniper:  return "sniper";
    case BanditWeapon::Rifle:   break;
    }
    return "rifle";
}

inline bool ParseBanditWeapon(const std::string& text, BanditWeapon& weapon) {
    if (text == "rifle")   { weapon = BanditWeapon::Rifle;   return true; }
    if (text == "shotgun") { weapon = BanditWeapon::Shotgun; return true; }
    if (text == "sniper")  { weapon = BanditWeapon::Sniper;  return true; }
    return false;
}
