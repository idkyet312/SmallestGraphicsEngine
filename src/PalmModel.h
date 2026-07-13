#pragma once

// Real coconut-palm geometry for the shootable trees.
//
// The trees' physics is unchanged: PalmTrees still simulates a stack of boxes, and
// still breaks at the segment you shoot, topples the top as one rigid log, and lets
// you split that log again. This file only replaces what those boxes LOOK like.
//
// To do that the FBX is sliced by height into one mesh per trunk segment, plus a
// crown mesh for everything above the trunk (the fronds). Each physics box then
// draws its own slice instead of a cube, so a tree that snaps in the middle shows
// real trunk geometry on both halves, and the crown rides the falling log because
// it is welded to that same body.
//
// Slicing is done once at load, in the model's own local space, normalised so the
// trunk base sits at y = 0 and the tree is kTreeHeight tall. PalmTrees then scales
// each slice to whatever height it planted the tree at.

#include "DX12Core.h"
#include "FBXImporter.h"
#include "GLBImporter.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace DirectX;

// One drawable slice of the palm, already in local space with the trunk base at
// the origin. `yLo`/`yHi` are the height band it was cut from.
struct PalmSlice {
    std::shared_ptr<SceneMesh> mesh;
    float yLo = 0.0f;
    float yHi = 0.0f;
};

class PalmModel {
public:
    // Trunk slices, bottom-up. One per trunk segment the physics uses.
    static std::vector<PalmSlice>& TrunkSlices() {
        static std::vector<PalmSlice> slices;
        return slices;
    }
    // Everything above the trunk: the frond crown, as a single mesh.
    static std::shared_ptr<SceneMesh>& Crown() {
        static std::shared_ptr<SceneMesh> crown;
        return crown;
    }
    // Keep both replacement materials alive even when one has no triangles in
    // the sliced output. Their texture uploads remain referenced by the open
    // DX12 command list until the caller flushes model loading.
    static std::vector<std::shared_ptr<SceneMaterial>>& Materials() {
        static std::vector<std::shared_ptr<SceneMaterial>> materials;
        return materials;
    }
    // Height of the normalised model (trunk base -> top of the crown).
    static float ModelHeight() { return s_height; }
    // Maximum horizontal distance from trunk axis after normalisation.
    static float ModelRadius() { return s_radius; }
    // Height at which the trunk ends and the crown begins, in the same space.
    static float CrownBaseY() { return s_crownBaseY; }
    static bool  Loaded() { return s_loaded; }

    // Slice the palm into `segmentCount` trunk pieces. Safe to call repeatedly.
    static void Load(int segmentCount) {
        static bool attempted = false;
        if (attempted) return;
        attempted = true;

        const std::string path = Resolve("models/palmtree/Fbx/Coconut Tree.fbx");
        std::cout << "Loading palm " << path << "...\n";
        // Scale 1.0: we normalise the height ourselves below, so whatever units the
        // FBX shipped in do not matter.
        // PalmModel replaces imported materials below. Loading FBX textures here
        // would record uploads for resources discarded before command-list close,
        // invalidating the list and removing the DX12 device.
        auto root = FBXImporter::Load(path, g_dx12.device, g_dx12.commandList,
                                      1.0f, false, false);
        if (!root) {
            std::cerr << "Palm FBX unavailable; trees fall back to boxes\n";
            return;
        }

        // Flatten every primitive in the hierarchy into world space.
        std::vector<MeshPrimitive> prims;
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        Flatten(root, prims);
        if (prims.empty()) {
            std::cerr << "Palm FBX had no geometry; trees fall back to boxes\n";
            return;
        }

        // The FBX is a little scene -- three palms and a ground quad -- not one
        // tree. Keep only the palm nearest the origin, recentred on its own
        // trunk; otherwise every planted tree draws the whole grove, and the
        // ground quad's huge radius squashes modelScaleXZ to a sliver.
        SelectOneTree(prims);
        if (prims.empty()) {
            std::cerr << "Palm FBX had no usable tree; trees fall back to boxes\n";
            return;
        }

        // Measure, then normalise so the base sits at y = 0 and the tree is a
        // predictable height -- the FBX's own scale and origin are not trustworthy.
        float lo = FLT_MAX, hi = -FLT_MAX;
        for (const MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                lo = std::min(lo, p.vertices[v + 1]);
                hi = std::max(hi, p.vertices[v + 1]);
            }
        const float rawHeight = hi - lo;
        if (rawHeight < 1e-3f) return;

        const float scale = kTreeHeight / rawHeight;
        s_radius = 0.0f;
        for (MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                p.vertices[v]     = p.vertices[v]     * scale;          // x
                p.vertices[v + 1] = (p.vertices[v + 1] - lo) * scale;   // y -> base at 0
                p.vertices[v + 2] = p.vertices[v + 2] * scale;          // z
                s_radius = std::max(s_radius, std::sqrt(
                    p.vertices[v] * p.vertices[v] +
                    p.vertices[v + 2] * p.vertices[v + 2]));
            }
        s_height = kTreeHeight;

        if (FILE* f = std::fopen("palm_source.log", "w")) {
            for (size_t pi = 0; pi < prims.size(); ++pi) {
                float minY = FLT_MAX, maxY = -FLT_MAX, maxR = 0.0f;
                for (size_t v = 0; v + 11 < prims[pi].vertices.size(); v += 12) {
                    const float x = prims[pi].vertices[v];
                    const float y = prims[pi].vertices[v + 1];
                    const float z = prims[pi].vertices[v + 2];
                    minY = std::min(minY, y); maxY = std::max(maxY, y);
                    maxR = std::max(maxR, std::sqrt(x * x + z * z));
                }
                std::fprintf(f, "%zu y=%.2f..%.2f r=%.2f verts=%zu tris=%zu\n",
                    pi, minY, maxY, maxR, prims[pi].vertices.size() / 12,
                    prims[pi].indices.size() / 3);
            }
            std::fclose(f);
        }

        // The FBX references its textures relative to its own folder, but this
        // asset keeps them in a sibling Texture/ directory, so they all failed to
        // resolve and the tree came out untextured grey. Assign them ourselves:
        // bark for the narrow trunk geometry, the leaf sheet for the wide fronds.
        // Using two SHARED materials also lets SliceBand merge primitives by
        // material -- the raw FBX is ~67 primitives per tree, which multiplied by
        // 8 trees was chewing through the per-frame draw-call and descriptor
        // budgets; merged, a slice is at most 2 draws.
        AssignMaterials(prims);

        // The crown starts where the fronds do. Rather than guess, find the height
        // above which the mesh suddenly gets wide: the trunk is a narrow column, the
        // fronds splay out far past it.
        s_crownBaseY = FindCrownBase(prims);

        // Trunk: slice the band [0, crownBaseY) into equal-height pieces.
        const float segLen = s_crownBaseY / std::max(1, segmentCount);
        TrunkSlices().clear();
        for (int i = 0; i < segmentCount; ++i) {
            const float y0 = i * segLen;
            const float y1 = (i == segmentCount - 1) ? s_crownBaseY : (i + 1) * segLen;
            PalmSlice slice;
            slice.yLo = y0;
            slice.yHi = y1;
            slice.mesh = SliceBand(prims, y0, y1);
            TrunkSlices().push_back(std::move(slice));
        }

        // Crown: everything from crownBaseY up, as one mesh.
        Crown() = SliceBand(prims, s_crownBaseY, s_height + 1.0f);

        s_loaded = !TrunkSlices().empty();
        std::cout << "Palm sliced: " << TrunkSlices().size() << " trunk segments, crown at y="
                  << s_crownBaseY << " of " << s_height << "\n";

        // Ground-truth marker (stdout is buffered while the app runs): record what
        // the slicer actually produced, so the integration can be verified without
        // seeing the screen. Harmless if the directory is read-only.
        if (FILE* f = std::fopen("palm_load.log", "w")) {
            std::fprintf(f, "loaded=%d height=%.2f radius=%.2f crownBaseY=%.2f trunkSlices=%zu crownPrims=%d\n",
                         (int)s_loaded, s_height, s_radius, s_crownBaseY, TrunkSlices().size(),
                         Crown() ? (int)Crown()->primitives.size() : 0);
            for (size_t i = 0; i < TrunkSlices().size(); ++i)
                std::fprintf(f, " slice %zu: y[%.2f..%.2f] prims=%d\n", i,
                             TrunkSlices()[i].yLo, TrunkSlices()[i].yHi,
                             TrunkSlices()[i].mesh ? (int)TrunkSlices()[i].mesh->primitives.size() : 0);
            std::fclose(f);
        }
    }

private:
    static constexpr float kTreeHeight = 8.0f;   // normalised palm height, world units

    static std::string Resolve(const std::string& rel) {
        for (const std::string& c : { rel, "build/" + rel, "../" + rel, "../../build/" + rel })
            if (std::filesystem::exists(c)) return c;
        return rel;
    }

    // Collapse the node tree into world-space primitives.
    static void Flatten(const std::shared_ptr<SceneNode>& node,
                        std::vector<MeshPrimitive>& out) {
        if (!node) return;
        if (node->mesh) {
            const XMMATRIX world = XMLoadFloat4x4(&node->globalTransform);
            const XMMATRIX nrm = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
            for (const MeshPrimitive& src : node->mesh->primitives) {
                MeshPrimitive p = src;
                // Strip the GPU handles: these are CPU-side working copies.
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

    // The source FBX is a scene of three palms standing on a ground quad, not a
    // single tree. Keep the palm whose trunk is nearest the origin, drop the
    // rest (including the quad), and recentre it so its trunk base is on the
    // y axis. Prims are grouped into trees by nearest trunk, where a trunk is a
    // big primitive that reaches down to the scene floor.
    static void SelectOneTree(std::vector<MeshPrimitive>& prims) {
        float lo = FLT_MAX, hi = -FLT_MAX;
        for (const MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                lo = std::min(lo, p.vertices[v + 1]);
                hi = std::max(hi, p.vertices[v + 1]);
            }
        const float height = hi - lo;
        if (height < 1e-3f) return;

        struct Info {
            float minY = FLT_MAX, maxY = -FLT_MAX;
            float cx = 0.0f, cz = 0.0f;   // horizontal centroid
            size_t verts = 0;
        };
        std::vector<Info> info(prims.size());
        size_t maxVerts = 0;
        for (size_t i = 0; i < prims.size(); ++i) {
            Info& in = info[i];
            for (size_t v = 0; v + 11 < prims[i].vertices.size(); v += 12) {
                in.minY = std::min(in.minY, prims[i].vertices[v + 1]);
                in.maxY = std::max(in.maxY, prims[i].vertices[v + 1]);
                in.cx += prims[i].vertices[v];
                in.cz += prims[i].vertices[v + 2];
                ++in.verts;
            }
            if (in.verts) { in.cx /= in.verts; in.cz /= in.verts; }
            maxVerts = std::max(maxVerts, in.verts);
        }

        // Trunks: the dense primitives that reach down to the scene floor.
        std::vector<size_t> trunks;
        for (size_t i = 0; i < prims.size(); ++i)
            if (info[i].verts * 2 >= maxVerts && info[i].minY < lo + 0.25f * height)
                trunks.push_back(i);
        if (trunks.empty()) return;   // not the layout we expect; leave untouched

        size_t chosen = trunks[0];
        for (size_t t : trunks) {
            const auto d2 = [&](size_t i) {
                return info[i].cx * info[i].cx + info[i].cz * info[i].cz;
            };
            if (d2(t) < d2(chosen)) chosen = t;
        }

        // The chosen trunk's base: the average of its lowest verts. (Computed
        // before the keep pass below moves prims out from under us.)
        float bx = 0.0f, bz = 0.0f;
        size_t n = 0;
        const float baseBand = info[chosen].minY + 0.1f * height;
        for (size_t v = 0; v + 11 < prims[chosen].vertices.size(); v += 12)
            if (prims[chosen].vertices[v + 1] < baseBand) {
                bx += prims[chosen].vertices[v];
                bz += prims[chosen].vertices[v + 2];
                ++n;
            }
        if (n) { bx /= n; bz /= n; }

        std::vector<MeshPrimitive> kept;
        for (size_t i = 0; i < prims.size(); ++i) {
            // The ground quad is flat; no part of a palm is.
            if (info[i].maxY - info[i].minY < 0.05f * height) continue;
            size_t nearest = trunks[0];
            for (size_t t : trunks) {
                const float dxN = info[i].cx - info[nearest].cx;
                const float dzN = info[i].cz - info[nearest].cz;
                const float dxT = info[i].cx - info[t].cx;
                const float dzT = info[i].cz - info[t].cz;
                if (dxT * dxT + dzT * dzT < dxN * dxN + dzN * dzN) nearest = t;
            }
            if (nearest == chosen) kept.push_back(std::move(prims[i]));
        }
        if (kept.empty()) return;

        // Recentre on the trunk base.
        for (MeshPrimitive& p : kept)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                p.vertices[v]     -= bx;
                p.vertices[v + 2] -= bz;
            }

        prims = std::move(kept);
    }

    // Replace every primitive's material with one of two shared ones -- bark or
    // leaf -- picked by height: the trunk reaches the ground, frond geometry
    // hangs in the crown. (Radius is no good here: a leaning trunk swings wide
    // of the axis, and bark tagged as leaf turns see-through from the cutout.)
    static void AssignMaterials(std::vector<MeshPrimitive>& prims) {
        auto makeMat = [](const char* name, const char* texture) {
            auto m = std::make_shared<SceneMaterial>();
            m->name = name;
            m->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
            m->metallicFactor = 0.0f;
            m->roughnessFactor = 0.9f;
            m->baseColorTexture = GLBImporter::LoadTextureFromFile(
                Resolve(std::string("models/palmtree/Texture/") + texture),
                g_dx12.device, g_dx12.commandList, m->uploadHeaps);
            if (!m->baseColorTexture)
                std::cerr << "Palm texture missing: " << texture << "\n";
            return m;
        };
        auto bark = makeMat("palm_bark", "Bark.png");
        auto leaf = makeMat("palm_leaf", "leaf alpha texture.png");
        Materials() = { bark, leaf };
        // Untextured leaves read as grey plastic; give the flat colour a leafy
        // green fallback in case the texture is missing.
        leaf->baseColorFactor = XMFLOAT4(0.35f, 0.55f, 0.25f, 1.0f);
        // The leaf sheet shapes the cards through its alpha channel.
        leaf->alphaCutout = true;

        for (MeshPrimitive& p : prims) {
            float minY = FLT_MAX;
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12)
                minY = std::min(minY, p.vertices[v + 1]);
            p.material = (minY > kTreeHeight * 0.25f) ? leaf : bark;
        }
    }

    // Where the fronds start. Scan up in thin bands measuring how far the geometry
    // spreads from the trunk axis; the crown is the first band that is dramatically
    // wider than the narrow trunk below it.
    static float FindCrownBase(const std::vector<MeshPrimitive>& prims) {
        constexpr int kBands = 40;
        float radius[kBands] = {};
        for (const MeshPrimitive& p : prims)
            for (size_t v = 0; v + 11 < p.vertices.size(); v += 12) {
                const float y = p.vertices[v + 1];
                int b = (int)(y / kTreeHeight * kBands);
                b = std::clamp(b, 0, kBands - 1);
                const float r = std::sqrt(p.vertices[v] * p.vertices[v] +
                                          p.vertices[v + 2] * p.vertices[v + 2]);
                radius[b] = std::max(radius[b], r);
            }

        // Trunk width = the typical radius over the lower half of the tree.
        float trunkR = 0.0f;
        for (int b = 0; b < kBands / 2; ++b) trunkR = std::max(trunkR, radius[b]);

        // First band (in the upper half) that flares well past the trunk is the crown.
        for (int b = kBands / 2; b < kBands; ++b) {
            if (radius[b] > trunkR * 2.5f)
                return (float)b / kBands * kTreeHeight;
        }
        // No obvious flare: assume the top quarter is crown.
        return kTreeHeight * 0.75f;
    }

    // Build a mesh from every triangle whose centroid falls in [y0, y1). Triangles
    // are kept whole (never split), so a slice's geometry can overhang its band a
    // little -- which is what you want: a clean cut would leave a hollow trunk.
    //
    // Output is MERGED BY MATERIAL: the source FBX is dozens of small primitives,
    // and drawing each one per tree per frame was what exhausted the draw-call and
    // descriptor budgets. After AssignMaterials there are only two materials, so a
    // slice comes out as at most two primitives (bark + leaf).
    static std::shared_ptr<SceneMesh> SliceBand(const std::vector<MeshPrimitive>& prims,
                                                float y0, float y1) {
        auto mesh = std::make_shared<SceneMesh>();

        // material -> index of the merged output primitive in mesh->primitives
        std::vector<std::pair<SceneMaterial*, size_t>> outByMat;

        for (const MeshPrimitive& src : prims) {
            MeshPrimitive* out = nullptr;
            for (auto& [mat, idx] : outByMat)
                if (mat == src.material.get()) { out = &mesh->primitives[idx]; break; }

            // Remap: only emit vertices actually used by the kept triangles.
            std::vector<int> remap(src.vertices.size() / 12, -1);

            const size_t triCount = src.indices.size() / 3;
            for (size_t t = 0; t < triCount; ++t) {
                const unsigned i0 = src.indices[t*3], i1 = src.indices[t*3+1], i2 = src.indices[t*3+2];
                if ((size_t)i0*12+11 >= src.vertices.size() ||
                    (size_t)i1*12+11 >= src.vertices.size() ||
                    (size_t)i2*12+11 >= src.vertices.size()) continue;

                const float cy = (src.vertices[(size_t)i0*12+1] +
                                  src.vertices[(size_t)i1*12+1] +
                                  src.vertices[(size_t)i2*12+1]) / 3.0f;
                if (cy < y0 || cy >= y1) continue;

                // Lazily create the merged primitive on the first kept triangle.
                if (!out) {
                    MeshPrimitive fresh;
                    fresh.material = src.material;
                    fresh.materialIndex = src.materialIndex;
                    outByMat.emplace_back(src.material.get(), mesh->primitives.size());
                    mesh->primitives.push_back(std::move(fresh));
                    out = &mesh->primitives.back();
                }

                for (unsigned idx : { i0, i1, i2 }) {
                    if (remap[idx] < 0) {
                        remap[idx] = (int)(out->vertices.size() / 12);
                        out->vertices.insert(out->vertices.end(),
                                             src.vertices.begin() + (size_t)idx * 12,
                                             src.vertices.begin() + (size_t)idx * 12 + 12);
                    }
                    out->indices.push_back((unsigned)remap[idx]);
                }
            }
        }

        for (MeshPrimitive& p : mesh->primitives)
            GLBImporter::BuildMeshletData(p, g_dx12.device.Get());
        return mesh->primitives.empty() ? nullptr : mesh;
    }

    static inline float s_height = 0.0f;
    static inline float s_radius = 0.0f;
    static inline float s_crownBaseY = 0.0f;
    static inline bool  s_loaded = false;
};
