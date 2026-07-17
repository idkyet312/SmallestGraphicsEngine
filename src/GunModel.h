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
#include <cfloat>
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

    static int& SelectedWeapon() {
        static int weapon = 0; // 0 = AK47, 1 = Mossberg 590A1
        return weapon;
    }
    static bool ShotgunSelected() { return SelectedWeapon() == 1 && ShotgunLoaded(); }
    static const char* SelectedWeaponName() {
        return ShotgunSelected() ? "Mossberg 590A1" : "AK47";
    }
    static std::shared_ptr<SceneMesh>& PlayerMesh() {
        return ShotgunSelected() ? ShotgunMesh() : Mesh();
    }
    static bool PlayerLoaded() { return PlayerMesh() != nullptr; }
    static void CycleWeapon(int direction) {
        if (!ShotgunLoaded() || !Loaded()) return;
        (void)direction; // two weapons: either wheel direction toggles the slot
        SelectedWeapon() = (SelectedWeapon() + 1) % 2;
    }

    // Load and normalise the AK. Safe to call repeatedly; only the first call
    // does anything. Must run inside the model-loading command-list window --
    // it records texture uploads.
    static void Load() {
        static bool attempted = false;
        if (attempted) return;
        attempted = true;

        const std::string path = Resolve("models/ak47/AK47.FBX");
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

    static void LoadShotgun() {
        const std::string path = Resolve("models/shotgun_fbx/Mossberg 590A1.fbx");
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

    // Collapse the node tree into world-space primitives (same as PalmModel).
    static void Flatten(const std::shared_ptr<SceneNode>& node,
                        std::vector<MeshPrimitive>& out) {
        if (!node) return;
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
                out.push_back(std::move(p));
            }
        }
        for (const auto& child : node->children) Flatten(child, out);
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

    // Bake the asset's own axis convention and units away, so the mesh comes out
    // in the gun's local space: barrel along +Z, sights up +Z, sized to
    // kBarrelLength, origin at the rear of the weapon (roughly the grip).
    static void Orient(std::vector<MeshPrimitive>& prims) {
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

        const float scale = kBarrelLength / ext[fwd];
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

        const std::string dir = "models/ak47/textures/";
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

    // Extents of the normalised mesh in gun-local space.
    static inline XMFLOAT3 s_lo{};
    static inline XMFLOAT3 s_hi{};
};
