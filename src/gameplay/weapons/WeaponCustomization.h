#ifndef WEAPON_CUSTOMIZATION_H
#define WEAPON_CUSTOMIZATION_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace SGE {

enum class AttachmentSlot : uint8_t {
    Muzzle = 0,
    Optic,
    SideRail,
    Count
};

struct WeaponDefinition {
    std::string id;
    std::string displayName;
    int legacyWeaponId = -1;

    int magazineCapacity = 0;
    int initialReserve = 0;
    int maximumReserve = 0;
    float reloadSeconds = 0.0f;
    float fireIntervalSeconds = 0.1f;
    float damageMultiplier = 1.0f;
    float projectileSpeed = 0.0f;
    float recoilPitchDegrees = 0.0f;
    float recoilYawDegrees = 0.0f;
    float hipSpreadMultiplier = 1.0f;
    float adsSpreadMultiplier = 1.0f;
    float noiseRadiusMultiplier = 1.0f;
    float muzzleFlashDurationMultiplier = 1.0f;
    float muzzleFlashSizeMultiplier = 1.0f;
    float smokeMultiplier = 1.0f;
    float adsFovDegrees = 42.0f;
    // How much material a round can punch through before it stops, in arbitrary
    // budget units spent per surface. 0 stops at the first thing hit, which is
    // what every weapon did before this existed -- so a weapon left at the
    // default keeps its old behaviour exactly.
    float penetrationPower = 0.0f;
    bool suppressed = false;

    std::string viewModelAsset;
    std::string hudIconAsset;
};

struct AttachmentModifier {
    float recoilMultiplier = 1.0f;
    float hipSpreadMultiplier = 1.0f;
    float adsSpreadMultiplier = 1.0f;
    float noiseRadiusMultiplier = 1.0f;
    float muzzleFlashDurationMultiplier = 1.0f;
    float muzzleFlashSizeMultiplier = 1.0f;
    float smokeMultiplier = 1.0f;
    float adsFovOffsetDegrees = 0.0f;
};

struct AttachmentDefinition {
    std::string id;
    std::string displayName;
    AttachmentSlot slot = AttachmentSlot::Muzzle;
    int modifierOrder = 0;
    uint32_t compatibleWeaponMask = 0;
    AttachmentModifier modifier;
    bool suppressesWeapon = false;
    bool providesRedDot = false;
    bool providesLaser = false;
    std::string viewModelAsset;

    bool CompatibleWith(int legacyWeaponId) const {
        return legacyWeaponId >= 0 && legacyWeaponId < 32 &&
            (compatibleWeaponMask &
             (1u << static_cast<uint32_t>(legacyWeaponId))) != 0;
    }
};

struct WeaponInstance {
    std::string weaponId;
    int legacyWeaponId = -1;
    int magazine = 0;
    int reserve = 0;
    std::array<std::string,
        static_cast<size_t>(AttachmentSlot::Count)> installedAttachmentIds{};
};

struct ResolvedWeaponStats {
    int magazineCapacity = 0;
    int maximumReserve = 0;
    float reloadSeconds = 0.0f;
    float fireIntervalSeconds = 0.1f;
    float damageMultiplier = 1.0f;
    float projectileSpeed = 0.0f;
    float recoilPitchDegrees = 0.0f;
    float recoilYawDegrees = 0.0f;
    float hipSpreadMultiplier = 1.0f;
    float adsSpreadMultiplier = 1.0f;
    float noiseRadiusMultiplier = 1.0f;
    float muzzleFlashDurationMultiplier = 1.0f;
    float muzzleFlashSizeMultiplier = 1.0f;
    float smokeMultiplier = 1.0f;
    float adsFovDegrees = 42.0f;
    float penetrationPower = 0.0f;
    bool suppressed = false;
    bool redDotSight = false;
    bool laserSight = false;
};

class WeaponCustomizationSystem {
public:
    static constexpr int kWeaponCount = 11;
    static constexpr size_t kAttachmentCount = 3;
    // Multiplies every weapon's authored aim kick. 1.0 is as-authored; 2.0 is
    // twice the climb per shot. Scales the camera recoil -- the part that
    // actually moves where the shots land -- so it is a difficulty knob, not a
    // cosmetic one. The viewmodel's own punch is driven separately in Scene.h
    // and follows this through the same recoilScale.
    static constexpr float kGlobalRecoilScale = 2.0f;

    WeaponCustomizationSystem() {
        // These values mirror the existing weapon tables and firing cadence.
        // Keeping the legacy integer beside the stable string ID lets old level
        // and UI data continue to work while saves can persist names.
        definitions_ = {{
            { "ak47", "AK47", 0, 30, 120, 240, 1.55f, 0.10f, 1.0f,
              300.0f, 0.55f, 0.22f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
              1.0f, 42.0f, 2.0f, false, "Content/Models/ak47/AK47.FBX", "" },
            { "mossberg_590a1", "Remington 870", 1, 8, 32, 64, 2.40f,
              0.80f, 1.0f, 300.0f, 1.54f, 0.484f, 1.0f, 1.0f, 1.0f,
              1.0f, 1.0f, 1.0f, 42.0f, 0.0f, false,
              "Content/Models/MainPlayer/Guns/Shotgun/remington870.glb", "" },
            { "rpg7", "RPG-7", 2, 1, 4, 8, 2.80f, 1.70f, 1.0f,
              70.0f, 2.20f, 0.0f, 1.0f, 1.0f, 1.0f, 1.8f, 1.75f,
              2.4f, 42.0f, 0.0f, false, "Content/Models/RPG7/RPG72.fbx", "" },
            { "svd", "R700 Sniper", 3, 10, 40, 80, 1.75f, 1.05f, 5.0f,
              650.0f, 2.31f, 0.0f, 1.0f, 1.0f, 1.0f, 1.35f, 1.35f,
              1.5f, 42.0f, 4.0f, false,
              "Content/Models/MainPlayer/Guns/R700/Remington_700_Sps_Tactical.glb", "" },
            { "arc_laser_cutter", "ARC Laser Cutter", 4, 120, 360, 600,
              2.10f, 0.055f, 6.0f, 1800.0f, 0.55f, 0.22f, 1.0f, 1.0f,
              1.0f, 1.0f, 1.0f, 1.0f, 42.0f, 0.0f, false,
              "procedural/weapons/arc_laser_cutter", "" },
            { "remote_c4", "Remote C4", 5, 6, 12, 24, 1.80f, 0.48f,
              1.0f, 0.0f, 0.55f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
              1.0f, 42.0f, 0.0f, false,
              "Content/Models/C4/C4_bomb/source/c4.glb", "" },
            { "m2_flamethrower", "M2 Flamethrower", 6, 100, 300, 500,
              2.60f, 0.075f, 1.0f, 0.0f, 0.55f, 0.22f, 1.0f, 1.0f,
              1.0f, 1.0f, 1.0f, 1.0f, 42.0f, 0.0f, false,
              "procedural/weapons/m2_flamethrower", "" },
            { "mako_harpoon", "Mako Harpoon Gun", 7, 1, 12, 24, 1.35f,
              1.25f, 1.0f, 0.0f, 0.55f, 0.0f, 1.0f, 1.0f, 1.0f,
              1.0f, 1.0f, 1.0f, 42.0f, 0.0f, false,
              "Content/Models/HarpoonGun/HarpoonGun.glb", "" },
            { "svd_suppressed", "R700 Suppressed", 8, 5, 20, 40, 2.05f,
              1.05f, 5.0f, 650.0f, 1.98f, 0.0f, 1.0f, 1.0f, 0.25f,
              0.55f, 0.35f, 0.7f, 42.0f, 4.0f, true,
              "Content/Models/MainPlayer/Guns/R700/Remington_700_Sps_Tactical.glb", "" },
            // The carbine against the AK's battle rifle: it cycles faster
            // (0.075s vs 0.10) and climbs less (0.42/0.18 vs 0.55/0.22) for
            // slightly less damage per shot, so the two are a real choice at
            // range rather than one being strictly better.
            { "m4a1", "M4A1", 9, 30, 120, 240, 1.45f, 0.075f, 0.9f,
              320.0f, 0.42f, 0.18f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
              1.0f, 42.0f, 1.8f, false,
              "Content/Models/MainPlayer/Guns/m4/m4A1.glb", "" },
            // The AK47 stats verbatim -- same cartridge class, same cadence,
            // same handling. This is a second model of the same rifle, so the
            // two are interchangeable in everything but appearance.
            { "ak74", "AK-74", 10, 30, 120, 240, 1.55f, 0.10f, 1.0f,
              300.0f, 0.55f, 0.22f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
              1.0f, 42.0f, 2.0f, false,
              "Content/Models/MainPlayer/Guns/Ak74/ak74.glb", "" },
        }};

        const uint32_t ak = 1u << 0;
        const uint32_t shotgun = 1u << 1;
        const uint32_t svd = 1u << 3;
        const uint32_t m4 = 1u << 9;
        attachments_ = {{
            { "silencer", "Silencer", AttachmentSlot::Muzzle, 10,
              ak | svd | m4,
              { 0.86f, 1.0f, 1.0f, 0.25f, 0.55f, 0.35f, 0.60f, 0.0f },
              true, false, false, "procedural/attachments/silencer" },
            { "red_dot", "Red Dot Sight", AttachmentSlot::Optic, 20,
              ak | shotgun | m4,
              { 1.0f, 1.0f, 0.55f, 1.0f, 1.0f, 1.0f, 1.0f, -6.0f },
              false, true, false, "procedural/attachments/red_dot" },
            { "laser", "Visible Laser", AttachmentSlot::SideRail, 30,
              ak | shotgun | svd | m4,
              { 1.0f, 0.72f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f },
              false, false, true, "procedural/attachments/visible_laser" },
        }};

        for (const WeaponDefinition& definition : definitions_) {
            WeaponInstance& instance =
                instances_[static_cast<size_t>(definition.legacyWeaponId)];
            instance.weaponId = definition.id;
            instance.legacyWeaponId = definition.legacyWeaponId;
            instance.magazine = definition.magazineCapacity;
            instance.reserve = definition.initialReserve;
            ApplyDefaultAttachments(instance);
        }
    }

    const std::array<WeaponDefinition, kWeaponCount>& Definitions() const {
        return definitions_;
    }
    const std::array<AttachmentDefinition, kAttachmentCount>& Attachments() const {
        return attachments_;
    }

    const WeaponDefinition* FindWeapon(int legacyWeaponId) const {
        if (legacyWeaponId < 0 || legacyWeaponId >= kWeaponCount) return nullptr;
        return &definitions_[static_cast<size_t>(legacyWeaponId)];
    }
    const WeaponDefinition* FindWeapon(const std::string& id) const {
        const auto found = std::find_if(definitions_.begin(), definitions_.end(),
            [&](const WeaponDefinition& definition) {
                return definition.id == id;
            });
        return found == definitions_.end() ? nullptr : &*found;
    }
    const AttachmentDefinition* FindAttachment(const std::string& id) const {
        const auto found = std::find_if(attachments_.begin(), attachments_.end(),
            [&](const AttachmentDefinition& attachment) {
                return attachment.id == id;
            });
        return found == attachments_.end() ? nullptr : &*found;
    }

    WeaponInstance* Instance(int legacyWeaponId) {
        if (legacyWeaponId < 0 || legacyWeaponId >= kWeaponCount) return nullptr;
        return &instances_[static_cast<size_t>(legacyWeaponId)];
    }
    const WeaponInstance* Instance(int legacyWeaponId) const {
        if (legacyWeaponId < 0 || legacyWeaponId >= kWeaponCount) return nullptr;
        return &instances_[static_cast<size_t>(legacyWeaponId)];
    }

    WeaponInstance CreateInstance(int legacyWeaponId, int magazine = -1,
                                  int reserve = -1) const {
        WeaponInstance instance;
        const WeaponDefinition* definition = FindWeapon(legacyWeaponId);
        if (!definition) return instance;
        instance.weaponId = definition->id;
        instance.legacyWeaponId = legacyWeaponId;
        instance.magazine = std::clamp(
            magazine < 0 ? definition->magazineCapacity : magazine,
            0, definition->magazineCapacity);
        instance.reserve = std::clamp(
            reserve < 0 ? definition->initialReserve : reserve,
            0, definition->maximumReserve);
        ApplyDefaultAttachments(instance);
        return instance;
    }

    bool SetInstance(const WeaponInstance& instance) {
        const WeaponDefinition* definition = FindWeapon(instance.weaponId);
        if (!definition || definition->legacyWeaponId != instance.legacyWeaponId)
            return false;
        WeaponInstance value = instance;
        const ResolvedWeaponStats resolved = Resolve(value);
        value.magazine = std::clamp(value.magazine, 0, resolved.magazineCapacity);
        value.reserve = std::clamp(value.reserve, 0, resolved.maximumReserve);
        instances_[static_cast<size_t>(value.legacyWeaponId)] = std::move(value);
        return true;
    }

    bool EquipAttachment(int legacyWeaponId, const std::string& attachmentId) {
        WeaponInstance* instance = Instance(legacyWeaponId);
        const AttachmentDefinition* attachment = FindAttachment(attachmentId);
        if (!instance || !attachment ||
            !attachment->CompatibleWith(legacyWeaponId)) return false;
        instance->installedAttachmentIds[
            static_cast<size_t>(attachment->slot)] = attachment->id;
        return true;
    }

    bool RemoveAttachment(int legacyWeaponId, AttachmentSlot slot) {
        WeaponInstance* instance = Instance(legacyWeaponId);
        if (!instance) return false;
        std::string& id = instance->installedAttachmentIds[static_cast<size_t>(slot)];
        if (id.empty()) return false;
        id.clear();
        return true;
    }

    bool AttachmentInstalled(int legacyWeaponId,
                             const std::string& attachmentId) const {
        const WeaponInstance* instance = Instance(legacyWeaponId);
        if (!instance) return false;
        return std::find(instance->installedAttachmentIds.begin(),
                         instance->installedAttachmentIds.end(), attachmentId) !=
               instance->installedAttachmentIds.end();
    }

    ResolvedWeaponStats Resolve(int legacyWeaponId) const {
        const WeaponInstance* instance = Instance(legacyWeaponId);
        return instance ? Resolve(*instance) : ResolvedWeaponStats{};
    }

    ResolvedWeaponStats Resolve(const WeaponInstance& instance) const {
        ResolvedWeaponStats result;
        const WeaponDefinition* definition = FindWeapon(instance.weaponId);
        if (!definition || definition->legacyWeaponId != instance.legacyWeaponId)
            return result;

        result.magazineCapacity = definition->magazineCapacity;
        result.maximumReserve = definition->maximumReserve;
        result.reloadSeconds = definition->reloadSeconds;
        result.fireIntervalSeconds = definition->fireIntervalSeconds;
        result.damageMultiplier = definition->damageMultiplier;
        result.projectileSpeed = definition->projectileSpeed;
        result.recoilPitchDegrees = definition->recoilPitchDegrees;
        result.recoilYawDegrees = definition->recoilYawDegrees;
        result.hipSpreadMultiplier = definition->hipSpreadMultiplier;
        result.adsSpreadMultiplier = definition->adsSpreadMultiplier;
        result.noiseRadiusMultiplier = definition->noiseRadiusMultiplier;
        result.muzzleFlashDurationMultiplier =
            definition->muzzleFlashDurationMultiplier;
        result.muzzleFlashSizeMultiplier = definition->muzzleFlashSizeMultiplier;
        result.smokeMultiplier = definition->smokeMultiplier;
        result.adsFovDegrees = definition->adsFovDegrees;
        result.penetrationPower = definition->penetrationPower;
        result.suppressed = definition->suppressed;

        std::array<const AttachmentDefinition*,
            static_cast<size_t>(AttachmentSlot::Count)> ordered{};
        size_t count = 0;
        for (const std::string& id : instance.installedAttachmentIds) {
            if (id.empty()) continue;
            const AttachmentDefinition* attachment = FindAttachment(id);
            if (attachment && attachment->CompatibleWith(instance.legacyWeaponId))
                ordered[count++] = attachment;
        }
        std::sort(ordered.begin(), ordered.begin() + count,
            [](const AttachmentDefinition* a, const AttachmentDefinition* b) {
                if (a->modifierOrder != b->modifierOrder)
                    return a->modifierOrder < b->modifierOrder;
                return a->id < b->id;
            });

        for (size_t i = 0; i < count; ++i) {
            const AttachmentDefinition& attachment = *ordered[i];
            result.recoilPitchDegrees *= attachment.modifier.recoilMultiplier;
            result.recoilYawDegrees *= attachment.modifier.recoilMultiplier;
            result.hipSpreadMultiplier *=
                attachment.modifier.hipSpreadMultiplier;
            result.adsSpreadMultiplier *=
                attachment.modifier.adsSpreadMultiplier;
            result.noiseRadiusMultiplier *=
                attachment.modifier.noiseRadiusMultiplier;
            result.muzzleFlashDurationMultiplier *=
                attachment.modifier.muzzleFlashDurationMultiplier;
            result.muzzleFlashSizeMultiplier *=
                attachment.modifier.muzzleFlashSizeMultiplier;
            result.smokeMultiplier *= attachment.modifier.smokeMultiplier;
            result.adsFovDegrees += attachment.modifier.adsFovOffsetDegrees;
            result.suppressed = result.suppressed || attachment.suppressesWeapon;
            result.redDotSight = result.redDotSight || attachment.providesRedDot;
            result.laserSight = result.laserSight || attachment.providesLaser;
        }
        result.adsFovDegrees = std::clamp(result.adsFovDegrees, 15.0f, 60.0f);
        // Global recoil scale, applied last so it multiplies the authored value
        // *and* whatever the attachments did to it -- a compensator still cuts
        // the same fraction, it just cuts a larger number. Applying it before
        // the attachment loop would let a recoilMultiplier below 1 quietly
        // cancel it out.
        //
        // One knob here rather than an edit per weapon: every gun resolves its
        // stats through this function, so the scale reaches all of them,
        // including any added later.
        result.recoilPitchDegrees *= kGlobalRecoilScale;
        result.recoilYawDegrees *= kGlobalRecoilScale;
        return result;
    }

    void RestoreAmmo() {
        for (WeaponInstance& instance : instances_) {
            const WeaponDefinition* definition = FindWeapon(instance.legacyWeaponId);
            if (!definition) continue;
            instance.magazine = Resolve(instance).magazineCapacity;
            instance.reserve = definition->maximumReserve;
        }
    }

    void HalveAmmo() {
        for (WeaponInstance& instance : instances_) {
            instance.magazine /= 2;
            instance.reserve /= 2;
        }
    }

private:
    static void ApplyDefaultAttachments(WeaponInstance& instance) {
        // The Remington ships with its receiver-mounted red dot. Leave the side
        // rail empty so the visible laser remains an opt-in attachment.
        if (instance.legacyWeaponId == 1) {
            instance.installedAttachmentIds[
                static_cast<size_t>(AttachmentSlot::Optic)] = "red_dot";
        }
    }

    std::array<WeaponDefinition, kWeaponCount> definitions_{};
    std::array<AttachmentDefinition, kAttachmentCount> attachments_{};
    std::array<WeaponInstance, kWeaponCount> instances_{};
};

} // namespace SGE

#endif
