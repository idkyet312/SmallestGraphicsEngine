#pragma once

// The AK-47 view model: the real FBX, drawn where the boxed carbine used to be.
//
// The renderer already has a place to put a gun -- Scene::GetGunBaseMatrix()
// hands back an orthonormal frame sitting in front of the camera with +X right,
// +Y up, +Z down the barrel, and the old M4 was a pile of boxes laid out in that
// space. This file swaps those boxes for geometry, and nothing else about how
// the weapon is positioned or aimed changes.
//
// Two things the raw asset will not give us, so we do them here:
//
//   * ORIENTATION AND SCALE. An FBX arrives in whatever units and axis
//     convention its author used -- this one is Z-up and hundreds of units long.
//     Rather than hand-tune a magic matrix, we measure the mesh's bounding box,
//     take its longest axis to BE the barrel, and build the rotation that lands
//     that axis on +Z. Then we scale the whole thing to a fixed barrel length so
//     the gun fills the same screen space the boxed M4 did.
//
//   * MATERIALS. The asset ships metalness and roughness as separate greyscale
//     TGAs, but the shader wants them packed in one map (G = roughness,
//     B = metal), the glTF layout. So we decode both (plus AO) and interleave
//     them into a single texture.

#include "DX12Core.h"
#include "FBXImporter.h"
#include "GLBImporter.h"
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace DirectX;

class GunModel {
public:
    // The whole weapon as one mesh, already in gun-local space: barrel down +Z,
    // origin at the grip end, sized to kBarrelLength. Null until Load() succeeds.
    static std::shared_ptr<SceneMesh>& Mesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }
    static bool Loaded() { return Mesh() != nullptr; }

    static std::shared_ptr<SceneMesh>& ShotgunMesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }
    static bool ShotgunLoaded() { return ShotgunMesh() != nullptr; }

    static std::shared_ptr<SceneMesh>& RPGMesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }
    static bool RPGLoaded() { return RPGMesh() != nullptr; }
    static std::shared_ptr<SceneMesh>& RPGRocketMesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }

    static std::shared_ptr<SceneMesh>& SVDMesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }
    static bool SVDLoaded() { return SVDMesh() != nullptr; }

    static std::shared_ptr<SceneMesh>& HarpoonGunMesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }
    static bool HarpoonGunLoaded() { return HarpoonGunMesh() != nullptr; }

    static std::shared_ptr<SceneMesh>& M4Mesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }
    static bool M4Loaded() { return M4Mesh() != nullptr; }

    // AK-74: a second Kalashnikov sharing the AK47 stats, offsets and
    // material. A variant of the same rifle rather than a new weapon class, so
    // nothing about its handling is authored separately.
    static std::shared_ptr<SceneMesh>& AK74Mesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }
    static bool AK74Loaded() { return AK74Mesh() != nullptr; }

    // The M4's iron sights, split from the body at load. Drawn only when no
    // optic is fitted: a red dot mounts directly over them, and the rear leaf
    // would otherwise stand up through the sight body.
    static std::shared_ptr<SceneMesh>& M4IronSightMesh() {
        static std::shared_ptr<SceneMesh> mesh;
        return mesh;
    }

    static std::shared_ptr<SceneNode>& HarpoonSpearModel() {
        static std::shared_ptr<SceneNode> model;
        return model;
    }

    static std::shared_ptr<SceneNode>& RedDotSightModel() {
        static std::shared_ptr<SceneNode> model;
        return model;
    }

    // Highest valid weapon id. Several parallel tables are sized to this, so
    // adding a weapon means extending every one of them.
    static constexpr int kMaxWeapon = 10;

    static int& SelectedWeapon() {
        // 0 AK, 1 shotgun, 2 RPG, 3 SVD, 4 laser, 5 C4, 6 flame, 7 harpoon,
        // 8 suppressed SVD, 9 M4A1, 10 AK-74
        static int weapon = 0;
        return weapon;
    }
    static const char* WeaponName(int weapon) {
        static constexpr const char* names[kMaxWeapon + 1] = {
            "AK47", "Remington 870", "RPG-7", "R700 Sniper",
            "ARC Laser Cutter", "Remote C4", "M2 Flamethrower",
            "Mako Harpoon Gun", "R700 Suppressed", "M4A1", "AK-74"
        };
        return names[(std::max)(0, (std::min)(weapon, kMaxWeapon))];
    }
    static bool ShotgunSelected() { return SelectedWeapon() == 1 && ShotgunLoaded(); }
    static bool RPGSelected() { return SelectedWeapon() == 2 && RPGLoaded(); }
    // Both SVD variants share one imported mesh: the suppressor is drawn as an
    // extra tube rather than a separate model, so anything asking "is the
    // player holding an SVD" -- viewmodel, scope, offsets -- must accept both.
    static bool SVDSuppressedSelected() {
        return SelectedWeapon() == 8 && SVDLoaded();
    }
    static bool SVDSelected() {
        return (SelectedWeapon() == 3 || SelectedWeapon() == 8) && SVDLoaded();
    }
    static bool LaserSelected() { return SelectedWeapon() == 4; }
    static bool C4Selected() { return SelectedWeapon() == 5; }
    static bool FlamethrowerSelected() { return SelectedWeapon() == 6; }
    static bool HarpoonSelected() { return SelectedWeapon() == 7; }
    static bool M4Selected() { return SelectedWeapon() == 9 && M4Loaded(); }
    static bool AK74Selected() { return SelectedWeapon() == 10 && AK74Loaded(); }
    static const char* SelectedWeaponName() {
        return WeaponName(SelectedWeapon());
    }
    static std::shared_ptr<SceneMesh>& PlayerMesh() {
        if (HarpoonSelected())
            return HarpoonGunLoaded() ? HarpoonGunMesh() :
                (ShotgunLoaded() ? ShotgunMesh() : Mesh());
        if (FlamethrowerSelected())
            return ShotgunLoaded() ? ShotgunMesh() : Mesh();
        if (M4Selected()) return M4Mesh();
        if (AK74Selected()) return AK74Mesh();
        if (SVDSelected()) return SVDMesh();
        if (RPGSelected()) return RPGMesh();
        return ShotgunSelected() ? ShotgunMesh() : Mesh();
    }
    // Per-weapon placement in gun-local space. Each imported mesh has a
    // different distance from its rear bound to its support-hand grip, so one
    // shared offset cannot keep every weapon inside the same animated hands.
    static XMFLOAT3& WeaponOffset(int weapon) {
        static std::array<XMFLOAT3, kMaxWeapon + 1> offsets = {{
            { 0.000f, -0.100f, -0.440f }, // AK47 handguard
            // Tuned in game against the Remington 870 mesh, which is shorter
            // and sits lower in the hands than the Mossberg it replaced.
            { 0.009f, -0.050f, -0.410f }, // Remington 870 pump
            { 0.020f, -0.030f, -0.330f }, // RPG forward grip
            { 0.030f, -0.060f, -0.490f }, // SVD handguard
            { 0.015f, -0.080f, -0.390f }, // laser emitter
            {-0.035f,  0.055f,  0.215f }, // C4 pack (authored brick)
            { 0.030f, -0.095f, -0.330f }, // flamethrower nozzle
            { 0.020f, -0.085f, -0.305f }, // harpoon barrel
            // Same mesh and so the same grip as the standard SVD. The
            // suppressor hangs off the muzzle, forward of the support hand,
            // and does not move where the rifle is held.
            { 0.030f, -0.060f, -0.490f }, // SVD suppressed handguard
            // Sits further back and lower than the AK: Orient normalises both
            // to the same barrel length, but the M4's rear bound is closer to
            // its grip, so the same pocket would push it through the hands.
            { 0.005f, -0.130f, -0.250f }, // M4A1 handguard
            // Same values as the AK47 above: Orient normalises both rifles to
            // the same barrel length, and the AK-74 shares the AK pattern, so
            // the support hand lands in the same place on the handguard.
            { 0.000f, -0.100f, -0.440f }, // AK-74 handguard
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxWeapon));
        return offsets[static_cast<size_t>(slot)];
    }
    static XMFLOAT3& PlayerOffset() {
        return WeaponOffset(SelectedWeapon());
    }
    // Per-weapon fit rotation, in degrees (pitch/yaw/roll). The offset above
    // only slides a mesh along the hands; an imported model whose authored
    // forward axis differs from the engine's also has to be turned before it
    // sits in them, which is what this carries. Zero for the weapons whose
    // meshes already arrive correctly oriented.
    static XMFLOAT3& WeaponFitRotation(int weapon) {
        static std::array<XMFLOAT3, kMaxWeapon + 1> rotations = {{
            { 0.0f, 0.0f, 0.0f },   // AK47
            // Slight pitch: the export is not perfectly square to its own
            // bounding box, so Orient leaves it a touch off-axis.
            { 2.0f, 0.0f, 0.0f },   // Remington 870
            { 0.0f, 0.0f, 0.0f },   // RPG
            { 0.0f, 0.0f, 0.0f },   // SVD
            { 0.0f, 0.0f, 0.0f },   // laser
            { 7.0f, 247.0f, 63.0f },// C4: turns the brick into the held pose
            { 0.0f, 0.0f, 0.0f },   // flamethrower
            { 0.0f, 0.0f, 0.0f },   // harpoon
            { 0.0f, 0.0f, 0.0f },   // SVD suppressed
            { 0.0f, 0.0f, 0.0f },   // M4A1
            { 0.0f, 0.0f, 0.0f },   // AK-74
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxWeapon));
        return rotations[static_cast<size_t>(slot)];
    }
    static XMFLOAT3& PlayerFitRotation() {
        return WeaponFitRotation(SelectedWeapon());
    }

    // Per-weapon size multiplier on top of the shared viewmodel scale. Orient
    // normalises every imported gun to the same barrel length, which makes one
    // set of hand offsets work everywhere but also throws away how big the
    // weapon really is -- a carbine and a full-length rifle both come out
    // 1.25 units long. This is the correction for that, kept separate from the
    // global Scale so tuning one weapon cannot resize the whole rack.
    static float& WeaponFitScale(int weapon) {
        static std::array<float, kMaxWeapon + 1> scales = {{
            1.00f, // AK47
            // Orient normalises every gun to one barrel length, which leaves
            // this pump-action reading small beside the rifles. Sized up to
            // match them in hand.
            1.35f, // Remington 870
            1.00f, // RPG
            1.00f, // SVD
            1.00f, // laser
            1.00f, // C4
            1.00f, // flamethrower
            1.00f, // harpoon
            1.00f, // SVD suppressed
            1.00f, // M4A1
            1.00f, // AK-74
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxWeapon));
        return scales[static_cast<size_t>(slot)];
    }
    static float& PlayerFitScale() {
        return WeaponFitScale(SelectedWeapon());
    }

    // Optic mount, per weapon and in gun-local space. The sight rides the
    // weapon transform, so these are relative to the rifle rather than the
    // camera: they slide it along the receiver rail, not across the screen.
    // Each host mesh carries its rail at a different height and station, which
    // is why one shared mount cannot serve every weapon.
    static XMFLOAT3& WeaponOpticOffset(int weapon) {
        static std::array<XMFLOAT3, kMaxWeapon + 1> offsets = {{
            { 0.012f, 0.154f, 0.376f }, // AK47 rear receiver rail
            { 0.003f, 0.099f, 0.480f }, // Remington 870 receiver rail
            // Only the AK and the shotgun accept an optic today (the red dot's
            // compatibility mask in WeaponCustomization.h). The rest carry the
            // same starting pose rather than a stale one, so widening that mask
            // gives a sight roughly on the rail instead of floating in space.
            { 0.012f, 0.154f, 0.376f }, // RPG
            { 0.012f, 0.154f, 0.376f }, // SVD
            { 0.012f, 0.154f, 0.376f }, // laser
            { 0.012f, 0.154f, 0.376f }, // C4
            { 0.012f, 0.154f, 0.376f }, // flamethrower
            { 0.012f, 0.154f, 0.376f }, // harpoon
            { 0.012f, 0.154f, 0.376f }, // SVD suppressed
            // Further forward and lower than the AK: the M4's flat-top rail
            // runs the length of the receiver, so the optic sits ahead of where
            // the AK's dust-cover mount puts it. Paired with the 3.07 scale
            // below -- this rifle imports smaller against the shared viewmodel
            // length, so both the position and the size differ.
            { 0.009f, 0.114f, 0.470f }, // M4A1 flat-top rail
            { 0.012f, 0.154f, 0.376f }, // AK-74 rear receiver rail
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxWeapon));
        return offsets[static_cast<size_t>(slot)];
    }
    static XMFLOAT3& PlayerOpticOffset() {
        return WeaponOpticOffset(SelectedWeapon());
    }
    // Degrees, applied in the sight's own space before it is moved onto the
    // rail, so it turns in place rather than swinging around the receiver. The
    // asset is authored long on +X, and the renderer's own quarter turn down
    // the barrel is separate from this -- zero here means "as mounted".
    static XMFLOAT3& WeaponOpticRotation(int weapon) {
        static std::array<XMFLOAT3, kMaxWeapon + 1> rotations = {{
            { 0.0f,  0.0f, 0.0f }, // AK47
            { 0.0f,  0.0f, 0.0f }, // Remington 870
            { 0.0f,  0.0f, 0.0f }, // RPG
            { 0.0f,  0.0f, 0.0f }, // SVD
            { 0.0f,  0.0f, 0.0f }, // laser
            { 0.0f,  0.0f, 0.0f }, // C4
            { 0.0f,  0.0f, 0.0f }, // flamethrower
            { 0.0f,  0.0f, 0.0f }, // harpoon
            { 0.0f,  0.0f, 0.0f }, // SVD suppressed
            // Small correction for the rail's own pitch and cant.
            {-5.0f, -1.0f, 0.0f }, // M4A1
            { 0.0f,  0.0f, 0.0f }, // AK-74
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxWeapon));
        return rotations[static_cast<size_t>(slot)];
    }
    static XMFLOAT3& PlayerOpticRotation() {
        return WeaponOpticRotation(SelectedWeapon());
    }
    // Preserves the sight's real-world size against the normalised 1.25-unit
    // rifle, so it is per weapon like the offset above it.
    static float& WeaponOpticScale(int weapon) {
        static std::array<float, kMaxWeapon + 1> scales = {{
            1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f,
            // The M4 imports smaller relative to the shared viewmodel length
            // than the AK does, so the same sight needs scaling up to stay a
            // believable size on its rail.
            3.07f, // M4A1
            1.00f  // AK-74: imports at the AK47 proportions
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxWeapon));
        return scales[static_cast<size_t>(slot)];
    }
    static float& PlayerOpticScale() {
        return WeaponOpticScale(SelectedWeapon());
    }
    // The collimated reticle is a separate emissive point from the sight body,
    // so it carries its own placement: it sits behind and below the tube's
    // centre, and it has to stay on the crosshair when the body is nudged.
    static XMFLOAT3& WeaponReticleOffset(int weapon) {
        static std::array<XMFLOAT3, kMaxWeapon + 1> offsets = {{
            { 0.000f, -1.000f, -0.600f }, // AK47
            { 0.000f, -1.000f, -0.600f }, // Remington 870
            { 0.000f, -1.000f, -0.600f }, // RPG
            { 0.000f, -1.000f, -0.600f }, // SVD
            { 0.000f, -1.000f, -0.600f }, // laser
            { 0.000f, -1.000f, -0.600f }, // C4
            { 0.000f, -1.000f, -0.600f }, // flamethrower
            { 0.000f, -1.000f, -0.600f }, // harpoon
            { 0.000f, -1.000f, -0.600f }, // SVD suppressed
            // Unlike the others, this one is tuned rather than nominal: the
            // M4 is the only weapon that actually mounts the sight, so its dot
            // is centred in real glass. The sign flip against the rest follows
            // from the mount sitting forward and high on the flat-top rail
            // instead of back and low on a dust cover.
            {-0.000f,  0.164f,  0.789f }, // M4A1
            { 0.000f, -1.000f, -0.600f }, // AK-74
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxWeapon));
        return offsets[static_cast<size_t>(slot)];
    }
    static XMFLOAT3& PlayerReticleOffset() {
        return WeaponReticleOffset(SelectedWeapon());
    }
    // One dot size for every weapon: it is the apparent size of the reticle,
    // which does not depend on which rifle is underneath it. Tuned against the
    // M4A1, the only weapon whose compatibility mask accepts the red dot today.
    static float& ReticleSize() {
        static float size = 0.0163f;
        return size;
    }

    // Authored optic defaults, for the debug panel's Reset Sight.
    //
    // The accessors above hand out mutable references so the sliders can drive
    // the live pose, which means the tuned starting values are gone the moment
    // anything is dragged. These return the authored numbers by value so a
    // reset restores the weapon actually in hand -- the reset used to hardcode
    // the AK's mount and would overwrite the M4's with it.
    static XMFLOAT3 DefaultOpticOffset(int weapon) {
        if (weapon == 1) return XMFLOAT3(0.003f, 0.099f, 0.480f);
        // The M4's flat-top rail sits further forward and lower than the AK's
        // dust-cover mount; remaining weapons share the AK's starting pose.
        if (weapon == 9) return XMFLOAT3(0.009f, 0.114f, 0.470f);
        return XMFLOAT3(0.012f, 0.154f, 0.376f);
    }
    static XMFLOAT3 DefaultOpticRotation(int weapon) {
        // Small correction for the M4 rail's own pitch and cant.
        if (weapon == 9) return XMFLOAT3(-5.0f, -1.0f, 0.0f);
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    static float DefaultOpticScale(int weapon) {
        // The M4 imports smaller against the shared viewmodel length, so the
        // same sight needs scaling up to stay a believable size on its rail.
        return weapon == 9 ? 3.07f : 1.00f;
    }
    static XMFLOAT3 DefaultReticleOffset(int weapon) {
        if (weapon == 9) return XMFLOAT3(-0.000f, 0.164f, 0.789f);
        return XMFLOAT3(0.000f, -1.000f, -0.600f);
    }
    static float DefaultReticleSize() { return 0.0163f; }

    static bool PlayerLoaded() {
        return C4Selected() || FlamethrowerSelected() || HarpoonSelected() ||
            PlayerMesh() != nullptr;
    }
    static bool WeaponLoaded(int weapon) {
        switch (weapon) {
        case 0: return Loaded();
        case 1: return ShotgunLoaded();
        case 2: return RPGLoaded();
        case 3: return SVDLoaded();
        case 4: return true; // procedural viewmodel; no asset load required
        case 5: return true;
        case 6: return true;
        case 7: return true;
        case 8: return SVDLoaded(); // suppressed variant of the same rifle
        case 9: return M4Loaded();
        case 10: return AK74Loaded();
        default: return false;
        }
    }
    // Remote C4 (slot 5) is demolition kit, not a weapon choice: it rides along
    // with every loadout so the player always has the means to bring down a
    // demolition objective, whatever two weapons they picked. Carrying it costs
    // neither of the two slots.
    static constexpr int kRemoteChargeWeapon = 5;

    // The two chosen weapons. C4 is carried on top of these -- see
    // LoadoutAllows/CycleWeapon, which treat it as always available.
    static std::array<int, 2>& LoadoutWeapons() {
        static std::array<int, 2> weapons{{ 0, 1 }};
        return weapons;
    }
    static bool& LoadoutRestricted() {
        static bool restricted = false;
        return restricted;
    }
    // True when `weapon` may be selected under the current restriction.
    static bool LoadoutAllows(int weapon) {
        if (!LoadoutRestricted()) return WeaponLoaded(weapon);
        if (weapon == kRemoteChargeWeapon) return true;
        const auto& weapons = LoadoutWeapons();
        return weapon == weapons[0] || weapon == weapons[1];
    }
    static bool ConfigureLoadout(int primary, int secondary) {
        if (primary == secondary || !WeaponLoaded(primary) ||
            !WeaponLoaded(secondary))
            return false;
        LoadoutWeapons() = {{ primary, secondary }};
        LoadoutRestricted() = true;
        SelectedWeapon() = primary;
        return true;
    }
    static void DisableLoadoutRestriction() { LoadoutRestricted() = false; }
    static void CycleWeapon(int direction) {
        if (LoadoutRestricted()) {
            // Cycle the two chosen weapons plus the always-carried charge, in a
            // stable order so the control feels the same every mission.
            const auto& weapons = LoadoutWeapons();
            std::array<int, 3> carried{{ weapons[0], weapons[1],
                                         kRemoteChargeWeapon }};
            int count = 2;
            if (weapons[0] != kRemoteChargeWeapon &&
                weapons[1] != kRemoteChargeWeapon)
                count = 3;
            int index = 0;
            for (int i = 0; i < count; ++i)
                if (carried[static_cast<size_t>(i)] == SelectedWeapon())
                    index = i;
            const int step = direction < 0 ? -1 : 1;
            index = (index + step + count) % count;
            SelectedWeapon() = carried[static_cast<size_t>(index)];
            return;
        }
        const int step = direction < 0 ? -1 : 1;
        // Bounded by the table, not a literal: this was hardcoded to 8 while
        // nine weapons existed, so the suppressed SVD in slot 8 could never be
        // reached by scrolling. Any weapon added past the end was equally
        // invisible.
        constexpr int kWeaponSlots = kMaxWeapon + 1;
        int candidate = SelectedWeapon();
        for (int attempt = 0; attempt < kWeaponSlots; ++attempt) {
            candidate = (candidate + step + kWeaponSlots) % kWeaponSlots;
            if (WeaponLoaded(candidate)) {
                SelectedWeapon() = candidate;
                return;
            }
        }
    }

    // Load and normalise the AK. Safe to call repeatedly; only the first call
    // does anything. Must run inside the model-loading command-list window --
    // it records texture uploads.
    static void Load() {
        static bool attempted = false;
        if (attempted) return;
        attempted = true;

        LoadHarpoonGun();
        LoadHarpoonSpear();
        LoadRedDotSight();
        LoadM4();

        const std::string path = Resolve("Content/Models/ak47/AK47.FBX");
        std::cout << "Loading AK47 " << path << "...\n";
        // Scale 1 and no FBX-side textures: we normalise the size ourselves below
        // and assign the material by hand, so letting the importer resolve
        // textures would just record uploads for resources we then discard --
        // which invalidates the open command list.
        auto root = FBXImporter::Load(path, g_dx12.device, g_dx12.commandList,
                                      1.0f, false, false);
        if (!root) {
            std::cerr << "AK47 FBX unavailable; gun falls back to boxes\n";
            LoadShotgun();
            LoadRPG();
            LoadSVD();
            return;
        }

        std::vector<MeshPrimitive> prims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims);
        if (prims.empty()) {
            std::cerr << "AK47 FBX had no geometry; gun falls back to boxes\n";
            LoadShotgun();
            LoadRPG();
            LoadSVD();
            return;
        }

        FlipV(prims);
        Orient(prims);
        AssignMaterial(prims);

        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(prims);
        for (MeshPrimitive& p : mesh->primitives)
            GLBImporter::BuildMeshletData(p, g_dx12.device.Get());
        Mesh() = mesh;

        std::cout << "AK47 loaded: " << mesh->primitives.size() << " primitive(s)\n";

        // Ground truth: stdout stays buffered while the app runs, so record what
        // actually loaded to a file the integration can be checked against.
        if (FILE* f = std::fopen("gun_load.log", "w")) {
            size_t tris = 0, verts = 0;
            for (const MeshPrimitive& p : mesh->primitives) {
                tris += p.indices.size() / 3;
                verts += p.vertices.size() / 12;
            }
            auto& mat = Material();
            std::fprintf(f,
                "loaded=1 prims=%zu verts=%zu tris=%zu albedo=%d normal=%d packedMR=%d\n",
                mesh->primitives.size(), verts, tris,
                mat && mat->baseColorTexture ? 1 : 0,
                mat && mat->normalTexture ? 1 : 0,
                mat && mat->metallicRoughnessTexture ? 1 : 0);
            std::fprintf(f, "bounds x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]\n",
                         s_lo.x, s_hi.x, s_lo.y, s_hi.y, s_lo.z, s_hi.z);
            std::fclose(f);
        }
        LoadShotgun();
        LoadRPG();
        LoadSVD();
        // After the AK47 above, whose material this reuses.
        LoadAK74();
    }

    // Keep the material alive: its texture uploads stay referenced by the open
    // command list until the caller flushes model loading.
    static std::shared_ptr<SceneMaterial>& Material() {
        static std::shared_ptr<SceneMaterial> mat;
        return mat;
    }

private:
    // Length of the normalised weapon along the barrel, in gun-local units. The
    // boxed M4 ran from about z = -0.44 to z = +0.81, so this matches its reach
    // and the view model keeps the size it always had on screen.
    static constexpr float kBarrelLength = 1.25f;

    static std::string Resolve(const std::string& rel) {
        for (const std::string& c : { rel, "build/" + rel, "../" + rel, "../../build/" + rel })
            if (std::filesystem::exists(c)) return c;
        return rel;
    }

    static void LoadRedDotSight() {
        const std::string path = Resolve(
            "Content/Models/MainPlayer/Guns/Attachment/Red+Dot+Sight.glb");
        auto root = GLBImporter::LoadGLB(
            path, g_dx12.device, g_dx12.commandList);
        if (!root) {
            std::cerr << "Red dot sight GLB unavailable; using procedural fallback\n";
            return;
        }

        // The downloaded scene includes a large black presentation backdrop.
        // It is not part of the attachment and would cover most of the viewmodel.
        const auto prepare = [&](const auto& self,
                                 const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            node->children.erase(
                std::remove_if(node->children.begin(), node->children.end(),
                    [](const std::shared_ptr<SceneNode>& child) {
                        return child && child->name == "BG";
                    }),
                node->children.end());
            if (node->mesh) {
                // Material names here describe the product, not the parts:
                // "RedDot" is the sight HOUSING (1905 of the asset's 2291
                // triangles), not the reticle. Dropping it deleted the whole
                // optic and left the ring behind -- the exact opposite of what
                // was wanted. The painted ring lives on "Glass" instead, and is
                // suppressed below by its texture rather than by deleting
                // geometry the lens itself needs.
                for (MeshPrimitive& primitive : node->mesh->primitives) {
                    if (!primitive.material) continue;
                    primitive.material->disableOcclusionCulling = true;
                    // The asset also carries an authored emissive map, which
                    // drew the glowing green ring. Nothing here wants it: the
                    // only mark that should light up is the engine dot.
                    primitive.material->emissiveTexture.Reset();
                    primitive.material->emissiveFactor = { 0.0f, 0.0f, 0.0f };
                    if (primitive.material->name != "Glass") continue;
                    // The ring is authored twice: once as the emissive glow
                    // cleared above, and again in the lens base-colour texture,
                    // which is what stayed visible as a faint grey circle after
                    // the emissive went. Drop that texture as well so the glass
                    // is a clean plate.
                    //
                    // It also carried the lens coverage, so the alpha has to be
                    // supplied explicitly now -- without this the factor's 1.0
                    // would leave an opaque disc where the glass used to be.
                    primitive.material->baseColorTexture.Reset();
                    primitive.material->baseColorFactor.w = 0.30f;
                    primitive.material->alphaBlend = true;
                    // Tinted coating rather than clear plate: real reflex glass
                    // passes most of the scene while throwing a faint cool cast,
                    // and near-black metal here would sink the lens back to the
                    // opaque look this is fixing.
                    primitive.material->baseColorFactor.x = 0.58f;
                    primitive.material->baseColorFactor.y = 0.70f;
                    primitive.material->baseColorFactor.z = 0.78f;
                    // Polished dielectric: sharp specular for the sky glint
                    // across the lens, no metallic darkening underneath it.
                    primitive.material->metallicFactor = 0.0f;
                    primitive.material->roughnessFactor = 0.06f;
                    // Selects the Fresnel glass path in the pixel shader, so
                    // the lens clears looking straight through and picks up sky
                    // toward grazing angles. A flat authored alpha cannot do
                    // that, which is what made it read as tinted film.
                    primitive.material->materialType = 8.0f;  // glass
                    // Both faces of a curved lens are visible through itself.
                    primitive.material->doubleSided = true;
                }
            }
            for (const auto& child : node->children) self(self, child);
        };
        prepare(prepare, root);
        RedDotSightModel() = std::move(root);
        std::cout << "Red dot sight GLB ready\n";
    }

    static void LoadHarpoonSpear() {
        const std::string path = Resolve(
            "Content/Models/HarpoonSpear/Spear.glb");
        HarpoonSpearModel() = GLBImporter::LoadGLB(
            path, g_dx12.device, g_dx12.commandList);
        if (!HarpoonSpearModel()) {
            std::cerr << "Harpoon spear GLB unavailable; using procedural spear\n";
            return;
        }
        const auto disableMovingOcclusion = [&](const auto& self,
                                                const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            if (node->mesh) for (MeshPrimitive& primitive : node->mesh->primitives)
                if (primitive.material)
                    primitive.material->disableOcclusionCulling = true;
            for (const auto& child : node->children) self(self, child);
        };
        disableMovingOcclusion(disableMovingOcclusion, HarpoonSpearModel());
        std::cout << "Harpoon spear GLB ready\n";
    }

    static void LoadHarpoonGun() {
        const std::string path = Resolve(
            "Content/Models/HarpoonGun/HarpoonGun.glb");
        auto root = GLBImporter::LoadGLB(
            path, g_dx12.device, g_dx12.commandList);
        if (!root) {
            std::cerr << "Harpoon gun GLB unavailable; using shotgun fallback\n";
            return;
        }

        std::vector<MeshPrimitive> primitives;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, primitives);
        if (primitives.empty()) {
            std::cerr << "Harpoon gun GLB had no geometry\n";
            return;
        }

        // Rebase into the same grip-local frame as every other weapon: rear at
        // z=0, barrel down +Z, normalized to the standard viewmodel length.
        Orient(primitives);
        FlipHarpoonGun180(primitives);
        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(primitives);
        for (MeshPrimitive& primitive : mesh->primitives) {
            if (primitive.material)
                primitive.material->disableOcclusionCulling = true;
            GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
        }
        HarpoonGunMesh() = mesh;
        std::cout << "Harpoon gun GLB ready: " << mesh->primitives.size()
                  << " primitive(s)\n";
    }

    // Second Kalashnikov, and a GLB rather than the binary FBX it used to
    // be: the re-export embeds the AK-74 texture set the FBX was missing,
    // so this no longer has to borrow the AK47's material and no longer
    // has to run after the AK47 load.
    static void LoadAK74() {
        const std::string path = Resolve(
            "Content/Models/MainPlayer/Guns/Ak74/ak74.glb");
        auto root = GLBImporter::LoadGLB(
            path, g_dx12.device, g_dx12.commandList);
        if (!root) {
            std::cerr << "AK-74 GLB unavailable; weapon slot stays empty\n";
            return;
        }

        std::vector<MeshPrimitive> prims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims);
        if (prims.empty()) {
            std::cerr << "AK-74 GLB had no geometry\n";
            return;
        }

        // No FlipV here, unlike the FBX this replaced: glTF already uses our
        // UV convention. Normalises to the shared 1.25-unit barrel length,
        // which is what makes the AK47's authored hand and optic offsets
        // correct for this mesh too.
        Orient(prims);

        // Keeps its own material rather than borrowing the AK47's. The FBX
        // this replaced shipped with no textures at all, which is the only
        // reason it had to share; this export embeds the same AK-74 base
        // colour, packed metallic-roughness and normal set directly.
        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(prims);
        for (MeshPrimitive& primitive : mesh->primitives) {
            if (primitive.material)
                primitive.material->disableOcclusionCulling = true;
            GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
        }
        AK74Mesh() = mesh;
        std::cout << "AK-74 loaded: " << mesh->primitives.size()
                  << " primitive(s)\n";
        if (FILE* file = std::fopen("gun_load.log", "a")) {
            size_t triangles = 0, vertices = 0;
            for (const MeshPrimitive& primitive : mesh->primitives) {
                triangles += primitive.indices.size() / 3;
                vertices += primitive.vertices.size() / 12;
            }
            const auto& first = mesh->primitives.front().material;
            std::fprintf(file,
                "ak74_loaded=1 prims=%zu verts=%zu tris=%zu"
                " albedo=%d normal=%d packedMR=%d\n",
                mesh->primitives.size(), vertices, triangles,
                first && first->baseColorTexture ? 1 : 0,
                first && first->normalTexture ? 1 : 0,
                first && first->metallicRoughnessTexture ? 1 : 0);
            std::fclose(file);
        }
    }

    static void LoadM4() {
        const std::string path = Resolve(
            "Content/Models/MainPlayer/Guns/m4/m4A1.glb");
        auto root = GLBImporter::LoadGLB(
            path, g_dx12.device, g_dx12.commandList);
        if (!root) {
            std::cerr << "M4A1 GLB unavailable; weapon slot stays empty\n";
            return;
        }

        // Authored as eighteen separate parts (body, barrel, magazine, iron
        // sights) at 0.01 node scale with per-part rotations. Flattening bakes
        // each node's transform into its vertices, so the hierarchy stops
        // mattering and Orient can treat the result as one rifle.
        //
        // The iron sights are kept in the same list through Orient rather than
        // flattened separately: Orient derives its rotation and scale from the
        // bounding box of whatever it is given, so sights measured on their own
        // would be normalised against their own extents and land at the wrong
        // size and place. Split afterwards, once the whole rifle shares one
        // frame. The count is recorded first so the split knows where they are.
        std::vector<MeshPrimitive> primitives;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        std::vector<size_t> ironSightIndices;
        FlattenTaggingIronSights(root, primitives, ironSightIndices);
        if (primitives.empty()) {
            std::cerr << "M4A1 GLB had no geometry\n";
            return;
        }

        // Same grip-local frame as every other weapon: rear at z=0, barrel down
        // +Z, normalised to the standard viewmodel length. Measured from the
        // bounds rather than hand-fitted, which is what makes the asset's own
        // scale and axis convention irrelevant.
        Orient(primitives);

        // Split the sights out now that everything shares the rifle's frame.
        // Walked back-to-front so each erase cannot shift an index still to be
        // removed.
        std::vector<MeshPrimitive> ironSights;
        for (size_t i = ironSightIndices.size(); i-- > 0;) {
            const size_t index = ironSightIndices[i];
            if (index >= primitives.size()) continue;
            ironSights.push_back(std::move(primitives[index]));
            primitives.erase(primitives.begin() +
                             static_cast<std::ptrdiff_t>(index));
        }

        const auto finish = [](std::vector<MeshPrimitive>&& prims) {
            auto built = std::make_shared<SceneMesh>();
            built->primitives = std::move(prims);
            for (MeshPrimitive& primitive : built->primitives) {
                if (primitive.material)
                    primitive.material->disableOcclusionCulling = true;
                GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
            }
            return built;
        };

        auto mesh = finish(std::move(primitives));
        M4Mesh() = mesh;
        if (!ironSights.empty())
            M4IronSightMesh() = finish(std::move(ironSights));
        std::cout << "M4A1 GLB ready: " << mesh->primitives.size()
                  << " primitive(s), " << ironSightIndices.size()
                  << " iron sight primitive(s) split\n";
    }

    // Flatten, recording which primitives came from the M4's iron sight nodes.
    // Those are drawn only when no optic is fitted -- a red dot sits directly
    // over them, and the rear leaf pokes through the sight body otherwise.
    static void FlattenTaggingIronSights(
        const std::shared_ptr<SceneNode>& node,
        std::vector<MeshPrimitive>& out,
        std::vector<size_t>& ironSightIndices,
        bool insideIronSight = false) {
        if (!node) return;
        std::string nodeName = node->name;
        std::transform(nodeName.begin(), nodeName.end(), nodeName.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        // Matches "Aiming Module back" and "Aiming module iron site" (the
        // asset's own spelling). Substring rather than exact so a re-export
        // that tidies the names still hits.
        insideIronSight = insideIronSight ||
            nodeName.find("aiming module") != std::string::npos ||
            nodeName.find("aiming modul") != std::string::npos;

        if (node->mesh) {
            const size_t before = out.size();
            // Reuse the shared flatten for the vertex work by handing it a
            // single-node view: it bakes globalTransform, which is already
            // resolved on this node.
            FlattenSingleNode(node, out);
            if (insideIronSight)
                for (size_t i = before; i < out.size(); ++i)
                    ironSightIndices.push_back(i);
        }
        for (const auto& child : node->children)
            FlattenTaggingIronSights(child, out, ironSightIndices,
                                     insideIronSight);
    }

    // Remington 870, the pump-action that replaced the Mossberg 590A1. A GLB
    // rather than an FBX, so the import skips FlipV -- the glTF UV convention
    // already matches ours -- and keeps the file's own PBR material set (two
    // materials, base colour + metallic-roughness + normal) instead of the flat
    // factors the Mossberg needed, which shipped without usable textures.
    //
    // Authored as six sibling roots (gun, mag, pump, kurok, slider, bullet)
    // laid out along X with per-node translations and, on two of them, a 90
    // degree rotation. Flatten bakes those transforms into the vertices, and
    // Orient then measures the combined bounds to rebase the whole thing into
    // the shared grip-local frame, so the asset's X-major layout needs no
    // hand-written axis fix here.
    static void LoadShotgun() {
        const std::string path = Resolve(
            "Content/Models/MainPlayer/Guns/Shotgun/remington870.glb");
        auto root = GLBImporter::LoadGLB(
            path, g_dx12.device, g_dx12.commandList);
        if (!root) {
            std::cerr << "Remington 870 GLB unavailable\n";
            return;
        }

        std::vector<MeshPrimitive> prims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims);
        if (prims.empty()) {
            std::cerr << "Remington 870 GLB had no geometry\n";
            return;
        }

        Orient(prims);

        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(prims);
        for (MeshPrimitive& primitive : mesh->primitives) {
            // The model is authored double-sided and is an open shell in
            // places, so occlusion culling against it is unreliable -- the same
            // exemption every other viewmodel GLB takes.
            if (primitive.material)
                primitive.material->disableOcclusionCulling = true;
            GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
        }
        ShotgunMesh() = mesh;
        if (!mesh->primitives.empty() && mesh->primitives.front().material)
            ShotgunMaterial() = mesh->primitives.front().material;
        std::cout << "Remington 870 loaded: " << mesh->primitives.size()
                  << " primitive(s)\n";
        if (FILE* file = std::fopen("gun_load.log", "a")) {
            size_t triangles = 0, vertices = 0;
            for (const MeshPrimitive& primitive : mesh->primitives) {
                triangles += primitive.indices.size() / 3;
                vertices += primitive.vertices.size() / 12;
            }
            // Same ground truth as the AK above: stdout stays buffered, so
            // record what the textured GLB actually produced.
            float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
            float hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (const MeshPrimitive& primitive : mesh->primitives)
                for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12)
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = (std::min)(lo[a], primitive.vertices[v + a]);
                        hi[a] = (std::max)(hi[a], primitive.vertices[v + a]);
                    }
            const auto& first = mesh->primitives.front().material;
            std::fprintf(file,
                "shotgun_loaded=1 prims=%zu verts=%zu tris=%zu"
                " albedo=%d normal=%d packedMR=%d\n",
                mesh->primitives.size(), vertices, triangles,
                first && first->baseColorTexture ? 1 : 0,
                first && first->normalTexture ? 1 : 0,
                first && first->metallicRoughnessTexture ? 1 : 0);
            std::fprintf(file,
                "shotgun_bounds x[%.3f..%.3f] y[%.3f..%.3f] z[%.3f..%.3f]\n",
                lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            std::fclose(file);
        }
    }

    static std::shared_ptr<SceneMaterial>& ShotgunMaterial() {
        static std::shared_ptr<SceneMaterial> material;
        return material;
    }

    static std::shared_ptr<SceneMaterial>& RPGMaterial() {
        static std::shared_ptr<SceneMaterial> material;
        return material;
    }
    static std::shared_ptr<SceneMaterial>& RPGRocketMaterial() {
        static std::shared_ptr<SceneMaterial> material;
        return material;
    }

    static std::vector<std::shared_ptr<SceneMaterial>>& SVDMaterials() {
        static std::vector<std::shared_ptr<SceneMaterial>> materials;
        return materials;
    }

    static void LoadRPG() {
        const std::string path = Resolve("Content/Models/RPG7/RPG72.fbx");
        std::cout << "Loading RPG-7 " << path << "...\n";
        auto root = FBXImporter::Load(path, g_dx12.device, g_dx12.commandList,
                                      1.0f, false, false);
        if (!root) {
            std::cerr << "RPG-7 FBX unavailable\n";
            return;
        }

        std::vector<MeshPrimitive> prims;
        std::vector<MeshPrimitive> rocketPrims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims, &rocketPrims);
        if (prims.empty()) {
            std::cerr << "RPG-7 FBX had no geometry\n";
            return;
        }

        FlipV(prims);
        Orient(prims, 1.48f);
        // RPG72 already exports upright after axis normalization. Older RPG7
        // needed an extra -90 degree roll, which turns this replacement sideways.
        AssignRPGMaterial(prims);

        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(prims);
        for (MeshPrimitive& primitive : mesh->primitives)
            GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
        RPGMesh() = mesh;
        std::cout << "RPG-7 loaded: " << mesh->primitives.size() << " primitive(s)\n";

        if (!rocketPrims.empty()) {
            FlipV(rocketPrims);
            Orient(rocketPrims, 0.52f);
            RollRPGUpright(rocketPrims);
            AssignRPGRocketMaterial(rocketPrims);
            auto rocketMesh = std::make_shared<SceneMesh>();
            rocketMesh->primitives = std::move(rocketPrims);
            for (MeshPrimitive& primitive : rocketMesh->primitives)
                GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
            RPGRocketMesh() = rocketMesh;
        }
    }

    // Remington 700 SPS Tactical, the bolt gun that replaced the SVD in the
    // marksman slot. A GLB, so no FlipV -- glTF already matches our UV
    // convention -- and it keeps the SVD's 1.55 barrel length rather than the
    // standard 1.25, because a marksman rifle is meant to read longer than the
    // assault rifles and the ADS and optic offsets were tuned against that.
    static void LoadSVD() {
        const std::string path = Resolve(
            "Content/Models/MainPlayer/Guns/R700/"
            "Remington_700_Sps_Tactical.glb");
        auto root = GLBImporter::LoadGLB(
            path, g_dx12.device, g_dx12.commandList);
        if (!root) {
            std::cerr << "Remington 700 GLB unavailable\n";
            return;
        }

        // Authored as three roots -- a loose cube, the magazine with its rounds,
        // and the body carrying barrel, bolt, scope and stock -- with per-node
        // scales and rotations. Flatten bakes those in so Orient can measure the
        // whole rifle as one object.
        std::vector<MeshPrimitive> prims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims);
        if (prims.empty()) {
            std::cerr << "Remington 700 GLB had no geometry\n";
            return;
        }

        Orient(prims, 1.55f);
        AssignR700Materials(prims);

        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(prims);
        for (MeshPrimitive& primitive : mesh->primitives) {
            if (primitive.material)
                primitive.material->disableOcclusionCulling = true;
            GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
        }
        SVDMesh() = mesh;
        std::cout << "Remington 700 loaded: " << mesh->primitives.size()
                  << " primitive(s)\n";
        if (FILE* file = std::fopen("gun_load.log", "a")) {
            size_t triangles = 0, vertices = 0;
            for (const MeshPrimitive& primitive : mesh->primitives) {
                triangles += primitive.indices.size() / 3;
                vertices += primitive.vertices.size() / 12;
            }
            std::fprintf(file, "r700_loaded=1 prims=%zu verts=%zu tris=%zu\n",
                         mesh->primitives.size(), vertices, triangles);
            std::fclose(file);
        }
    }

    // The R700 export carries eleven named materials but no usable values: no
    // images at all, and every factor left at the glTF default of white at
    // metallic 1.0 / roughness 1.0, which renders as a featureless white rifle.
    //
    // That is an export limit rather than a broken asset. In Blender the look is
    // a Mix Shader driven by Fresnel over a Diffuse BSDF and an Image Texture --
    // a node graph glTF has no way to express, so the exporter emitted bare
    // material slots. The names survived, though, so the look is rebuilt from
    // them the way the SVD's was: matched by name and given real factors, keyed
    // to how the rifle actually reads in Blender -- near-black parkerised metal,
    // a dark mottled synthetic stock, and a Fresnel-mixed objective lens.
    static void AssignR700Materials(std::vector<MeshPrimitive>& prims) {
        auto make = [](const char* name, XMFLOAT4 colour,
                       float metallic, float roughness) {
            auto material = std::make_shared<SceneMaterial>();
            material->name = name;
            material->baseColorFactor = colour;
            material->metallicFactor = metallic;
            material->roughnessFactor = roughness;
            return material;
        };

        auto metal  = make("r700_metal",  XMFLOAT4(0.075f, 0.078f, 0.082f, 1.0f), 0.82f, 0.35f);
        // The barrel and scope tube read smoother and darker than the receiver.
        auto barrel = make("r700_barrel", XMFLOAT4(0.048f, 0.050f, 0.054f, 1.0f), 0.86f, 0.24f);
        auto scope  = make("r700_scope",  XMFLOAT4(0.045f, 0.047f, 0.050f, 1.0f), 0.78f, 0.27f);
        // Synthetic stock: matte, barely metallic, a shade warmer than the steel.
        auto stock  = make("r700_stock",  XMFLOAT4(0.062f, 0.060f, 0.058f, 1.0f), 0.06f, 0.72f);
        auto wood   = make("r700_wood",   XMFLOAT4(0.070f, 0.062f, 0.056f, 1.0f), 0.06f, 0.70f);
        // Blender's "Wood" slot is the full lower stock (Cube.000), despite
        // the name. Rebuild its node graph with the authored black camo atlas,
        // roughness image and stock detail normal. The shared PBR shader already
        // performs the Fresnel-weighted diffuse/GGX mix shown in that graph.
        const std::string textureDir =
            "Content/Models/MainPlayer/Guns/R700/Textures/";
        wood->baseColorTexture = GLBImporter::LoadTextureFromFile(
            Resolve(textureDir + "Rifle_Camo_2.png"), g_dx12.device,
            g_dx12.commandList, wood->uploadHeaps);
        wood->normalTexture = GLBImporter::LoadTextureFromFile(
            Resolve(textureDir + "Rifle_Stock_Normal.png"), g_dx12.device,
            g_dx12.commandList, wood->uploadHeaps);
        wood->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            Resolve(textureDir + "Metal_Roughness.jpg"), g_dx12.device,
            g_dx12.commandList, wood->uploadHeaps);
        if (wood->baseColorTexture)
            wood->baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        // The source image is a standalone roughness field, not glTF's packed
        // G/B metallic-roughness layout. Mode 2 reads its green channel as the
        // final roughness while the polymer stock remains dielectric.
        wood->roughnessOnlyTexture = true;
        wood->metallicFactor = 0.0f;
        // The Blender material mixes pale-pink Glass (IOR 1.30, roughness 0)
        // with white GGX Glossy (roughness 0.10), driven by Fresnel at IOR 1.45.
        // Raster transparency stands in for the transmitted Glass lobe; type 9
        // supplies that exact Fresnel F0 without inheriting the reflex sight's
        // blue coating or its asset-specific UV edge treatment.
        auto glass  = make("r700_glass",  XMFLOAT4(1.000f, 0.620f, 0.560f, 0.30f), 0.0f, 0.10f);
        glass->alphaBlend = true;
        glass->doubleSided = true;
        glass->materialType = 9.0f;  // sniper glass
        auto brass  = make("r700_brass",  XMFLOAT4(0.430f, 0.290f, 0.090f, 1.0f), 0.88f, 0.26f);
        auto lead   = make("r700_bullet", XMFLOAT4(0.215f, 0.180f, 0.120f, 1.0f), 0.80f, 0.34f);
        SVDMaterials() = { metal, barrel, scope, stock, wood, glass, brass, lead };

        // Order matters: several names contain a shorter key ("Scope Glass" and
        // "Scope Knobs" both hold "scope", "Bullet Primer" holds "bullet"), so
        // the most specific test has to come first or the lens ends up in plain
        // scope steel.
        for (MeshPrimitive& primitive : prims) {
            std::string name = primitive.material ? primitive.material->name : "";
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            const auto has = [&](const char* key) {
                return name.find(key) != std::string::npos;
            };
            // "Buillet Case" is the asset's own spelling, so the case test keys
            // on "case" rather than on the misspelled word in front of it.
            primitive.material =
                  has("glass")  ? glass
                : has("primer") ? brass
                : has("case")   ? brass
                : has("bullet") ? lead
                : has("scope")  ? scope
                : has("barrel") ? barrel
                : has("wood")   ? wood
                : has("stock")  ? stock
                : metal;
        }
    }

    // Collapse the node tree into world-space primitives (same as PalmModel).
    // Bake one node's mesh into world space and append it. Split out of Flatten
    // so a caller that needs to know which node each primitive came from can
    // walk the tree itself without duplicating the vertex transform.
    static void FlattenSingleNode(const std::shared_ptr<SceneNode>& node,
                                  std::vector<MeshPrimitive>& target) {
        if (!node || !node->mesh) return;
        const XMMATRIX world = XMLoadFloat4x4(&node->globalTransform);
        const XMMATRIX nrm = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        for (const MeshPrimitive& src : node->mesh->primitives) {
            MeshPrimitive p = src;
            // Strip GPU handles: these are CPU-side working copies.
            p.vbv = {}; p.ibv = {};
            p.vertexBuffer.Reset(); p.indexBuffer.Reset();
            p.meshletDescBuffer.Reset(); p.meshletBoundsBuffer.Reset();
            p.meshletVertexIndexBuffer.Reset(); p.meshletTriangleBuffer.Reset();
            p.meshletCount = 0;

            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                XMVECTOR pos = XMVectorSet(p.vertices[v], p.vertices[v+1], p.vertices[v+2], 1);
                XMVECTOR n   = XMVectorSet(p.vertices[v+3], p.vertices[v+4], p.vertices[v+5], 0);
                XMVECTOR t   = XMVectorSet(p.vertices[v+8], p.vertices[v+9], p.vertices[v+10], 0);
                pos = XMVector3TransformCoord(pos, world);
                n   = XMVector3Normalize(XMVector3TransformNormal(n, nrm));
                t   = XMVector3Normalize(XMVector3TransformNormal(t, world));
                XMFLOAT3 pf, nf, tf;
                XMStoreFloat3(&pf, pos); XMStoreFloat3(&nf, n); XMStoreFloat3(&tf, t);
                p.vertices[v]=pf.x;   p.vertices[v+1]=pf.y;  p.vertices[v+2]=pf.z;
                p.vertices[v+3]=nf.x; p.vertices[v+4]=nf.y;  p.vertices[v+5]=nf.z;
                p.vertices[v+8]=tf.x; p.vertices[v+9]=tf.y;  p.vertices[v+10]=tf.z;
            }
            target.push_back(std::move(p));
        }
    }

    static void Flatten(const std::shared_ptr<SceneNode>& node,
                        std::vector<MeshPrimitive>& out,
                        std::vector<MeshPrimitive>* separatedRockets = nullptr,
                        bool insideRocket = false) {
        if (!node) return;
        std::string nodeName = node->name;
        std::transform(nodeName.begin(), nodeName.end(), nodeName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        insideRocket = insideRocket || nodeName.find("rocket") != std::string::npos;
        std::vector<MeshPrimitive>& target =
            separatedRockets && insideRocket ? *separatedRockets : out;
        FlattenSingleNode(node, target);
        for (const auto& child : node->children)
            Flatten(child, out, separatedRockets, insideRocket);
    }

    // Assimp hands back OpenGL-convention UVs (V = 0 at the BOTTOM of the image),
    // but D3D samples with V = 0 at the top, so V arrives inverted. Most of this
    // engine's models hide that -- their textures tile or are near-uniform, so
    // sampling the mirrored row looks the same. The AK's albedo is a tightly
    // packed atlas, where an inverted V lands in a completely different island
    // and paints the gun in garbage colours. Flip it back.
    //
    // Done here rather than with aiProcess_FlipUVs in FBXImporter because that
    // importer is shared, and every existing model was tuned against its current
    // behaviour.
    static void FlipV(std::vector<MeshPrimitive>& prims) {
        for (MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12)
                p.vertices[v + 7] = 1.0f - p.vertices[v + 7];   // offset 7 = V
    }

    static void RollRPGUpright(std::vector<MeshPrimitive>& prims) {
        // Generic orientation finds barrel axis but cannot infer roll. RPG grips
        // arrived on right side; rotate -90 degrees around local +Z so they hang.
        for (MeshPrimitive& primitive : prims) {
            for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                const float px = primitive.vertices[v];
                const float nx = primitive.vertices[v + 3];
                const float tx = primitive.vertices[v + 8];
                primitive.vertices[v] = primitive.vertices[v + 1];
                primitive.vertices[v + 1] = -px;
                primitive.vertices[v + 3] = primitive.vertices[v + 4];
                primitive.vertices[v + 4] = -nx;
                primitive.vertices[v + 8] = primitive.vertices[v + 9];
                primitive.vertices[v + 9] = -tx;
            }
        }
    }

    static void FlipHarpoonGun180(std::vector<MeshPrimitive>& prims) {
        // Rotate around local Y through the weapon centre. Keeping the same
        // 0..kBarrelLength frame preserves the viewmodel grip placement.
        for (MeshPrimitive& primitive : prims) {
            for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                primitive.vertices[v] = -primitive.vertices[v];
                primitive.vertices[v + 2] =
                    kBarrelLength - primitive.vertices[v + 2];
                primitive.vertices[v + 3] = -primitive.vertices[v + 3];
                primitive.vertices[v + 5] = -primitive.vertices[v + 5];
                primitive.vertices[v + 8] = -primitive.vertices[v + 8];
                primitive.vertices[v + 10] = -primitive.vertices[v + 10];
            }
        }
    }

    // Bake the asset's own axis convention and units away, so the mesh comes out
    // in the gun's local space: barrel along +Z, sights up +Z, sized to
    // kBarrelLength, origin at the rear of the weapon (roughly the grip).
    static void Orient(std::vector<MeshPrimitive>& prims,
                       float targetLength = kBarrelLength) {
        XMFLOAT3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                lo.x = std::min(lo.x, p.vertices[v]);     hi.x = std::max(hi.x, p.vertices[v]);
                lo.y = std::min(lo.y, p.vertices[v + 1]); hi.y = std::max(hi.y, p.vertices[v + 1]);
                lo.z = std::min(lo.z, p.vertices[v + 2]); hi.z = std::max(hi.z, p.vertices[v + 2]);
            }
        const float ext[3] = { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
        if (ext[0] + ext[1] + ext[2] < 1e-6f) return;

        // A rifle is far longer than it is tall or wide, so its longest bounding
        // axis IS the barrel -- no need to know the exporter's convention. The
        // shortest is the across-the-body axis (a gun is a thin slab), which
        // leaves the middle one running from grip to sights: up.
        int fwd = 0;
        for (int i = 1; i < 3; ++i) if (ext[i] > ext[fwd]) fwd = i;
        int side = 0;
        for (int i = 1; i < 3; ++i) if (ext[i] < ext[side]) side = i;
        if (side == fwd) side = (fwd + 1) % 3;          // degenerate; pick anything else
        const int up = 3 - fwd - side;                  // the remaining axis

        const float scale = targetLength / ext[fwd];
        // Centre across the body and on the barrel axis; sit the origin at the
        // rear so the model grows forward from the camera like the boxed M4 did.
        const float mid[3] = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
        const float loA[3] = { lo.x, lo.y, lo.z };

        auto pick = [](const float v[3], int i) { return v[i]; };

        for (MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                const float pos[3] = { p.vertices[v], p.vertices[v + 1], p.vertices[v + 2] };
                const float nml[3] = { p.vertices[v + 3], p.vertices[v + 4], p.vertices[v + 5] };
                const float tan[3] = { p.vertices[v + 8], p.vertices[v + 9], p.vertices[v + 10] };

                // Remap axes: source `fwd` -> +Z, `up` -> +Y, `side` -> +X.
                // Positions are rebased first (rear of the gun to the origin on
                // the barrel axis, centred on the other two), then scaled.
                p.vertices[v]     = (pick(pos, side) - pick(mid, side)) * scale;
                p.vertices[v + 1] = (pick(pos, up)   - pick(mid, up))   * scale;
                p.vertices[v + 2] = (pick(pos, fwd)  - pick(loA, fwd))  * scale;

                // Normals and tangents take the same axis swap, but no rebase and
                // no scale -- they are directions, and the scale is uniform.
                p.vertices[v + 3] = pick(nml, side);
                p.vertices[v + 4] = pick(nml, up);
                p.vertices[v + 5] = pick(nml, fwd);
                p.vertices[v + 8] = pick(tan, side);
                p.vertices[v + 9] = pick(tan, up);
                p.vertices[v + 10] = pick(tan, fwd);
            }

        // The axis swap above is a permutation of (x,y,z), and an ODD permutation
        // flips handedness -- which turns the mesh inside out under backface
        // culling. The even permutations of (0,1,2) are exactly the cyclic ones,
        // i.e. those with up == (side+1)%3. Anything else is odd: mirror X back to
        // restore a right-handed frame.
        const bool odd = (up != (side + 1) % 3);
        if (odd) {
            for (MeshPrimitive& p : prims) {
                for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                    p.vertices[v]     = -p.vertices[v];
                    p.vertices[v + 3] = -p.vertices[v + 3];
                    p.vertices[v + 8] = -p.vertices[v + 8];
                    // Offset 11 is the tangent handedness w, and the shader builds
                    // the bitangent as cross(N,T) * w. Mirroring reverses that
                    // cross product, so w has to flip too or normal mapping lights
                    // the surface inside-out.
                    p.vertices[v + 11] = -p.vertices[v + 11];
                }
                // Mirroring reverses winding; flip it back so culling still works.
                for (size_t i = 0; i + 2 < p.indices.size(); i += 3)
                    std::swap(p.indices[i + 1], p.indices[i + 2]);
            }
        }

        // Final extents in gun-local space, for the load log / placement tuning.
        s_lo = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        s_hi = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                s_lo.x = std::min(s_lo.x, p.vertices[v]);     s_hi.x = std::max(s_hi.x, p.vertices[v]);
                s_lo.y = std::min(s_lo.y, p.vertices[v + 1]); s_hi.y = std::max(s_hi.y, p.vertices[v + 1]);
                s_lo.z = std::min(s_lo.z, p.vertices[v + 2]); s_hi.z = std::max(s_hi.z, p.vertices[v + 2]);
            }
    }

    // One shared PBR material for the whole weapon, from the asset's TGA maps.
    static void AssignMaterial(std::vector<MeshPrimitive>& prims) {
        auto mat = std::make_shared<SceneMaterial>();
        mat->name = "ak47";
        mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);

        const std::string dir = "Content/Models/ak47/textures/";
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(
            Resolve(dir + "AK47_albedo.tga"), g_dx12.device, g_dx12.commandList,
            mat->uploadHeaps);
        mat->normalTexture = GLBImporter::LoadTextureFromFile(
            Resolve(dir + "AK47_normal.tga"), g_dx12.device, g_dx12.commandList,
            mat->uploadHeaps);

        // The shader reads one packed map (glTF layout: G = roughness, B = metal),
        // but the asset ships them as separate greyscale TGAs. Interleave them.
        std::vector<unsigned char> albedo, rough, metal, ao, packed;
        int cw = 0, ch = 0, rw = 0, rh = 0, mw = 0, mh = 0, aw = 0, ah = 0;
        const bool haveAlbedo = GLBImporter::LoadPixelsRGBA(Resolve(dir + "AK47_albedo.tga"), albedo, cw, ch);
        const bool haveRough = GLBImporter::LoadPixelsRGBA(Resolve(dir + "AK47_roughness.tga"), rough, rw, rh);
        const bool haveMetal = GLBImporter::LoadPixelsRGBA(Resolve(dir + "AK47_metalness.tga"), metal, mw, mh);
        const bool haveAO    = GLBImporter::LoadPixelsRGBA(Resolve(dir + "AK47_ao.tga"), ao, aw, ah);

        // Only pack if the two maps agree on size -- resampling here would be a
        // lot of code for an asset whose maps already match.
        if (haveRough && haveMetal && rw == mw && rh == mh && rw > 0) {
            const size_t texels = (size_t)rw * rh;
            const bool aoFits = haveAO && aw == rw && ah == rh;
            const bool albedoFits = haveAlbedo && cw == rw && ch == rh;
            packed.resize(texels * 4);
            for (size_t i = 0; i < texels; ++i) {
                unsigned char m = metal[i * 4];

                // This asset's metalness mask also covers the wooden furniture, and
                // the shader kills diffuse on metal (diffuseAlbedo = albedo * (1 - metal)),
                // so the stock and handguard lost their brown entirely and came out
                // as sky-coloured mirrors. Steel here is grey while the wood is
                // strongly warm, so gate metalness on albedo saturation: coloured
                // texels are wood and stay dielectric, neutral ones stay metal.
                if (albedoFits) {
                    const int r = albedo[i * 4 + 0], g = albedo[i * 4 + 1], b = albedo[i * 4 + 2];
                    const int mx = std::max(r, std::max(g, b));
                    const int mn = std::min(r, std::min(g, b));
                    const float sat = mx > 0 ? (float)(mx - mn) / (float)mx : 0.0f;
                    if (sat > 0.25f) m = 0;
                }

                packed[i * 4 + 0] = aoFits ? ao[i * 4] : 255;   // R = AO (unused by the shader)
                packed[i * 4 + 1] = rough[i * 4];               // G = roughness
                packed[i * 4 + 2] = m;                          // B = metalness
                packed[i * 4 + 3] = 255;
            }
            mat->metallicRoughnessTexture = GLBImporter::CreateTextureFromRGBA(
                g_dx12.device.Get(), g_dx12.commandList.Get(), packed, rw, rh,
                mat->uploadHeaps);
        }

        if (mat->metallicRoughnessTexture) {
            // Packed map present: the factors become plain multipliers, and the
            // texture carries the real values. roughnessOnlyTexture=false selects
            // the shader's "sample both channels" path.
            mat->roughnessOnlyTexture = false;
            mat->metallicFactor = 1.0f;
            mat->roughnessFactor = 1.0f;
        } else {
            // No maps: gunmetal-ish constants, so the weapon still reads as metal.
            std::cerr << "AK47 metal/rough maps unavailable; using constants\n";
            mat->metallicFactor = 0.85f;
            mat->roughnessFactor = 0.4f;
        }
        if (!mat->baseColorTexture) {
            std::cerr << "AK47 albedo missing; using flat gunmetal\n";
            mat->baseColorFactor = XMFLOAT4(0.16f, 0.15f, 0.15f, 1.0f);
        }

        Material() = mat;
        for (MeshPrimitive& p : prims) p.material = mat;
    }

    // RPG uses exactly four authored maps: albedo, normal, roughness and metal.
    // AO is deliberately excluded. Pack roughness/metal into glTF G/B channels.
    static void AssignRPGMaterial(std::vector<MeshPrimitive>& prims) {
        auto mat = std::make_shared<SceneMaterial>();
        mat->name = "rpg7";
        mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        const std::string dir = "Content/Models/RPG7/textures/";
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(
            Resolve(dir + "RPG7_Albedo.png"), g_dx12.device, g_dx12.commandList,
            mat->uploadHeaps);
        mat->normalTexture = GLBImporter::LoadTextureFromFile(
            Resolve(dir + "RPG7_Normal.png"), g_dx12.device, g_dx12.commandList,
            mat->uploadHeaps);

        std::vector<unsigned char> rough, metal, packed;
        int rw = 0, rh = 0, mw = 0, mh = 0;
        const bool haveRough = GLBImporter::LoadPixelsRGBA(
            Resolve(dir + "RPG7_Roughness.png"), rough, rw, rh);
        const bool haveMetal = GLBImporter::LoadPixelsRGBA(
            Resolve(dir + "RPG7_Metallic.png"), metal, mw, mh);
        if (haveRough && haveMetal && rw == mw && rh == mh && rw > 0) {
            const size_t texels = static_cast<size_t>(rw) * rh;
            packed.resize(texels * 4);
            for (size_t i = 0; i < texels; ++i) {
                packed[i * 4 + 0] = 255;
                packed[i * 4 + 1] = rough[i * 4];
                packed[i * 4 + 2] = metal[i * 4];
                packed[i * 4 + 3] = 255;
            }
            mat->metallicRoughnessTexture = GLBImporter::CreateTextureFromRGBA(
                g_dx12.device.Get(), g_dx12.commandList.Get(), packed, rw, rh,
                mat->uploadHeaps);
        }
        mat->roughnessOnlyTexture = false;
        mat->metallicFactor = mat->metallicRoughnessTexture ? 1.0f : 0.55f;
        mat->roughnessFactor = mat->metallicRoughnessTexture ? 1.0f : 0.48f;
        RPGMaterial() = mat;
        for (MeshPrimitive& primitive : prims) primitive.material = mat;
    }

    static void AssignRPGRocketMaterial(std::vector<MeshPrimitive>& prims) {
        auto mat = std::make_shared<SceneMaterial>();
        mat->name = "rpg7_rocket";
        mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        const std::string dir = "Content/Models/RPG7/textures/";
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(
            Resolve(dir + "RPG7Rocket_Albedo.png"), g_dx12.device,
            g_dx12.commandList, mat->uploadHeaps);
        mat->normalTexture = GLBImporter::LoadTextureFromFile(
            Resolve(dir + "RPG7Rocket_Normal.png"), g_dx12.device,
            g_dx12.commandList, mat->uploadHeaps);

        std::vector<unsigned char> rough, metal, packed;
        int rw = 0, rh = 0, mw = 0, mh = 0;
        const bool haveRough = GLBImporter::LoadPixelsRGBA(
            Resolve(dir + "RPG7Rocket_Roughness.png"), rough, rw, rh);
        const bool haveMetal = GLBImporter::LoadPixelsRGBA(
            Resolve(dir + "RPG7Rocket_Metallic.png"), metal, mw, mh);
        if (haveRough && haveMetal && rw == mw && rh == mh && rw > 0) {
            const size_t texels = static_cast<size_t>(rw) * rh;
            packed.resize(texels * 4);
            for (size_t i = 0; i < texels; ++i) {
                packed[i * 4] = 255;
                packed[i * 4 + 1] = rough[i * 4];
                packed[i * 4 + 2] = metal[i * 4];
                packed[i * 4 + 3] = 255;
            }
            mat->metallicRoughnessTexture = GLBImporter::CreateTextureFromRGBA(
                g_dx12.device.Get(), g_dx12.commandList.Get(), packed, rw, rh,
                mat->uploadHeaps);
        }
        mat->roughnessOnlyTexture = false;
        mat->metallicFactor = mat->metallicRoughnessTexture ? 1.0f : 0.55f;
        mat->roughnessFactor = mat->metallicRoughnessTexture ? 1.0f : 0.48f;
        RPGRocketMaterial() = mat;
        for (MeshPrimitive& primitive : prims) primitive.material = mat;
    }


    // Extents of the normalised mesh in gun-local space.
    static inline XMFLOAT3 s_lo{};
    static inline XMFLOAT3 s_hi{};
};
