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

    static std::shared_ptr<SceneNode>& HarpoonSpearModel() {
        static std::shared_ptr<SceneNode> model;
        return model;
    }

    static int& SelectedWeapon() {
        static int weapon = 0; // 0 AK, 1 shotgun, 2 RPG, 3 SVD, 4 laser, 5 C4, 6 flame, 7 harpoon
        return weapon;
    }
    static const char* WeaponName(int weapon) {
        static constexpr const char* names[8] = {
            "AK47", "Mossberg 590A1", "RPG-7", "SVD Sniper",
            "ARC Laser Cutter", "Remote C4", "M2 Flamethrower",
            "Mako Harpoon Gun"
        };
        return names[(std::max)(0, (std::min)(weapon, 7))];
    }
    static bool ShotgunSelected() { return SelectedWeapon() == 1 && ShotgunLoaded(); }
    static bool RPGSelected() { return SelectedWeapon() == 2 && RPGLoaded(); }
    static bool SVDSelected() { return SelectedWeapon() == 3 && SVDLoaded(); }
    static bool LaserSelected() { return SelectedWeapon() == 4; }
    static bool C4Selected() { return SelectedWeapon() == 5; }
    static bool FlamethrowerSelected() { return SelectedWeapon() == 6; }
    static bool HarpoonSelected() { return SelectedWeapon() == 7; }
    static const char* SelectedWeaponName() {
        return WeaponName(SelectedWeapon());
    }
    static std::shared_ptr<SceneMesh>& PlayerMesh() {
        if (HarpoonSelected())
            return HarpoonGunLoaded() ? HarpoonGunMesh() :
                (ShotgunLoaded() ? ShotgunMesh() : Mesh());
        if (FlamethrowerSelected())
            return ShotgunLoaded() ? ShotgunMesh() : Mesh();
        if (SVDSelected()) return SVDMesh();
        if (RPGSelected()) return RPGMesh();
        return ShotgunSelected() ? ShotgunMesh() : Mesh();
    }
    // Per-weapon placement in gun-local space. Each imported mesh has a
    // different distance from its rear bound to its support-hand grip, so one
    // shared offset cannot keep every weapon inside the same animated hands.
    static XMFLOAT3& WeaponOffset(int weapon) {
        static std::array<XMFLOAT3, 8> offsets = {{
            { 0.000f, -0.100f, -0.440f }, // AK47 handguard
            { 0.045f, -0.100f, -0.290f }, // Mossberg pump
            { 0.020f, -0.030f, -0.330f }, // RPG forward grip
            { 0.030f, -0.060f, -0.490f }, // SVD handguard
            { 0.015f, -0.080f, -0.390f }, // laser emitter
            { 0.000f, -0.080f, -0.180f }, // C4 pack
            { 0.030f, -0.095f, -0.330f }, // flamethrower nozzle
            { 0.020f, -0.085f, -0.305f }, // harpoon barrel
        }};
        const int slot = (std::max)(0, (std::min)(weapon, 7));
        return offsets[static_cast<size_t>(slot)];
    }
    static XMFLOAT3& PlayerOffset() {
        return WeaponOffset(SelectedWeapon());
    }
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
        int candidate = SelectedWeapon();
        for (int attempt = 0; attempt < 8; ++attempt) {
            candidate = (candidate + step + 8) % 8;
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

    static void LoadShotgun() {
        const std::string path = Resolve("Content/Models/shotgun_fbx/Mossberg 590A1.fbx");
        std::cout << "Loading Mossberg 590A1 " << path << "...\n";
        auto root = FBXImporter::Load(path, g_dx12.device, g_dx12.commandList,
                                      1.0f, false, false);
        if (!root) {
            std::cerr << "Mossberg 590A1 FBX unavailable\n";
            return;
        }

        std::vector<MeshPrimitive> prims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims);
        if (prims.empty()) {
            std::cerr << "Mossberg 590A1 FBX had no geometry\n";
            return;
        }

        FlipV(prims);
        Orient(prims);
        auto material = std::make_shared<SceneMaterial>();
        material->name = "mossberg_590a1";
        material->baseColorFactor = XMFLOAT4(0.075f, 0.085f, 0.09f, 1.0f);
        material->metallicFactor = 0.78f;
        material->roughnessFactor = 0.36f;
        ShotgunMaterial() = material;
        for (MeshPrimitive& primitive : prims) primitive.material = material;

        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(prims);
        for (MeshPrimitive& primitive : mesh->primitives)
            GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
        ShotgunMesh() = mesh;
        std::cout << "Mossberg 590A1 loaded: " << mesh->primitives.size()
                  << " primitive(s)\n";
        if (FILE* file = std::fopen("gun_load.log", "a")) {
            size_t triangles = 0, vertices = 0;
            for (const MeshPrimitive& primitive : mesh->primitives) {
                triangles += primitive.indices.size() / 3;
                vertices += primitive.vertices.size() / 12;
            }
            std::fprintf(file, "shotgun_loaded=1 prims=%zu verts=%zu tris=%zu\n",
                         mesh->primitives.size(), vertices, triangles);
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

    static void LoadSVD() {
        // Prefer the cooked blob generated from the original FBX. Runtime
        // Assimp cannot parse this old FBX 6100 file, so retain the ufbx-made
        // OBJ as a source fallback when cooked content is absent or stale.
        const std::string sourcePath =
            Resolve("Content/Models/SVD_v1.3/Models/SVD.FBX");
        std::cout << "Loading SVD " << sourcePath << "...\n";
        auto root = FBXImporter::Load(sourcePath, g_dx12.device,
                                      g_dx12.commandList, 1.0f, false, false);
        if (!root) {
            const std::string fallbackPath =
                Resolve("Content/Models/SVD_v1.3/Models/SVD.obj");
            std::cout << "SVD cooked asset unavailable; using "
                      << fallbackPath << "\n";
            root = FBXImporter::Load(fallbackPath, g_dx12.device,
                                     g_dx12.commandList, 1.0f, false, false);
        }
        if (!root) {
            std::cerr << "SVD model unavailable\n";
            return;
        }

        std::vector<MeshPrimitive> prims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims);
        if (prims.empty()) {
            std::cerr << "SVD FBX had no geometry\n";
            return;
        }

        FlipV(prims);
        Orient(prims, 1.55f);
        AssignSVDMaterials(prims);

        auto mesh = std::make_shared<SceneMesh>();
        mesh->primitives = std::move(prims);
        for (MeshPrimitive& primitive : mesh->primitives)
            GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get());
        SVDMesh() = mesh;
        std::cout << "SVD loaded: " << mesh->primitives.size() << " primitive(s)\n";
    }

    // Collapse the node tree into world-space primitives (same as PalmModel).
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
        if (node->mesh) {
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

    static void AssignSVDMaterials(std::vector<MeshPrimitive>& prims) {
        const std::string dir = "Content/Models/SVD_v1.3/Textures/";
        auto makeMaterial = [&](const char* name, const char* textureStem,
                                float metallic, float roughness) {
            auto material = std::make_shared<SceneMaterial>();
            material->name = name;
            material->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
            material->baseColorTexture = GLBImporter::LoadTextureFromFile(
                Resolve(dir + textureStem + "_dif.png"), g_dx12.device,
                g_dx12.commandList, material->uploadHeaps);
            material->normalTexture = GLBImporter::LoadTextureFromFile(
                Resolve(dir + textureStem + "_normal.png"), g_dx12.device,
                g_dx12.commandList, material->uploadHeaps);
            material->metallicFactor = metallic;
            material->roughnessFactor = roughness;
            return material;
        };

        auto body = makeMaterial("svd", "SVD", 0.62f, 0.42f);
        auto optics = makeMaterial("svd_optics", "Optics", 0.52f, 0.30f);
        auto bullet = makeMaterial("svd_bullet", "bullet", 0.78f, 0.28f);
        SVDMaterials() = { body, optics, bullet };

        for (MeshPrimitive& primitive : prims) {
            std::string sourceName = primitive.material ? primitive.material->name : "";
            std::transform(sourceName.begin(), sourceName.end(), sourceName.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            primitive.material = sourceName.find("optic") != std::string::npos ? optics
                : sourceName.find("bullet") != std::string::npos ? bullet : body;
        }
    }

    // Extents of the normalised mesh in gun-local space.
    static inline XMFLOAT3 s_lo{};
    static inline XMFLOAT3 s_hi{};
};
