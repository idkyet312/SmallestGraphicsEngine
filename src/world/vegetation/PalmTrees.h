#pragma once

// Shootable palm trees, Crysis-style: put enough rounds into a trunk and the tree
// snaps at that height, and the whole top -- trunk and crown together -- topples
// over as ONE rigid log. It does not crumble into loose blocks. Then keep shooting
// the log where it lies and it splits again, into two smaller logs.
//
// The model is deliberately uniform, which is what makes "cut it again" fall out
// for free rather than being a special case:
//
//   * A STANDING tree is a single static body: a box shape per trunk segment plus
//     the frond boxes, all welded to that one body. Static bodies cost the solver
//     nothing and physically cannot jitter.
//
//   * A LOG is a single dynamic body, built the same way -- a run of trunk pieces
//     (and the crown, if it took the crown down with it) welded together, so it
//     falls and lands as one solid object.
//
//   * Damage is tracked per trunk SEGMENT as a plain float. Segments are pure
//     bookkeeping, never separate bodies. They exist so a break happens at the
//     height you actually hit.
//
// Cutting anything -- standing tree or fallen log -- means the same thing: split
// its segment run at the hit, keep the lower part where it is, and spawn a new
// dynamic log for the upper part. A log can be cut as many times as it has
// segments, each split producing a shorter log that is itself still shootable.
//
// So a standing forest is free, and every cut creates exactly one new body.

#include <DirectXMath.h>
#include "DX12Core.h"
#include "PalmModel.h"
#include "PalmMeshCutter.h"
#include "PalmWindGPU.h"
#include <box3d/box3d.h>
#include <vector>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <algorithm>
#include <functional>

using namespace DirectX;

struct TreeItem {
    XMFLOAT4X4 transform;
    XMFLOAT3   color;
    bool       isFrond = false;

    // Which piece of the real palm model this box stands in for. The renderer draws
    // that slice's mesh at `transform` instead of a cube; if the model failed to
    // load these stay -1 and it falls back to drawing boxes.
    //   segment >= 0 : that trunk segment's mesh
    //   crown        : the frond crown mesh
    int  segment = -1;
    bool crown = false;

    // Scale to apply to the model slice: the model is normalised to a fixed height,
    // but trees are planted at varying heights.
    float modelScale = 1.0f;
    // Runtime boolean fragments own unique meshes. Null keeps shared slice path.
    std::shared_ptr<SceneMesh> meshOverride;
    // root.xy = tree world XZ, z = GPU wind enabled. Falling/cut logs leave zero.
    XMFLOAT4 palmWindRoot{};
};

class PalmTrees {
public:
    void SetTerrainSampler(std::function<float(float, float)> fn) {
        m_terrain = std::move(fn);
    }

    // Shares the grass controls so one gust moves the whole landscape.
    void SetWind(float strength, float speed) {
        m_windStrength = (std::max)(0.0f, strength);
        m_windSpeed = (std::max)(0.0f, speed);
    }

    void SetHelicopterWind(const XMFLOAT3& primary, bool primaryEnabled,
                           const XMFLOAT3& secondary, bool secondaryEnabled) {
        m_previousPrimaryHelicopter = m_primaryHelicopter;
        m_previousSecondaryHelicopter = m_secondaryHelicopter;
        m_previousPrimaryHelicopterWind = m_primaryHelicopterWind;
        m_previousSecondaryHelicopterWind = m_secondaryHelicopterWind;
        m_primaryHelicopter = primary;
        m_secondaryHelicopter = secondary;
        m_primaryHelicopterWind = primaryEnabled;
        m_secondaryHelicopterWind = secondaryEnabled;
    }

    void Initialize() {
        Shutdown();
        b3WorldDef wd = b3DefaultWorldDef();
        wd.gravity = { 0.0f, -9.81f, 0.0f };
        m_world = b3CreateWorld(&wd);
        if (B3_IS_NULL(m_world)) return;

        BuildGround();
    }

    // The ground felled logs land on. Sampled from the real terrain when a sampler
    // is available, so trunks come to rest on the actual sloping island (and roll
    // down the beach into the sea) instead of resting on an invisible flat plane.
    void BuildGround() {
        if (B3_IS_NULL(m_world)) return;

        if (!m_terrain) {
            // No sampler: fall back to a flat slab at y = 0.
            b3BodyDef gd = b3DefaultBodyDef();
            gd.position = { 0.0f, -kGroundHalf, 0.0f };
            b3BodyId ground = b3CreateBody(m_world, &gd);
            b3BoxHull gh = b3MakeBoxHull(kGroundSpan, kGroundHalf, kGroundSpan);
            b3ShapeDef gsd = b3DefaultShapeDef();
            gsd.baseMaterial.friction = 0.9f;
            b3CreateHullShape(ground, &gsd, &gh.base);
            return;
        }

        // Height-field over the whole island. Box3D quantizes the heights into
        // [globalMin, globalMax], so that range has to span the real terrain --
        // too narrow and the island gets clipped flat.
        const float span = kGroundSpan * 2.0f;             // full width, world units
        const float cell = span / (kGroundCells - 1);      // spacing between samples
        const float x0 = -kGroundSpan, z0 = -kGroundSpan;  // grid origin (a corner)

        m_heights.resize((size_t)kGroundCells * kGroundCells);
        float lo = FLT_MAX, hi = -FLT_MAX;
        for (int iz = 0; iz < kGroundCells; ++iz) {
            for (int ix = 0; ix < kGroundCells; ++ix) {
                const float h = m_terrain(x0 + ix * cell, z0 + iz * cell);
                m_heights[(size_t)iz * kGroundCells + ix] = h;
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
        }
        // Pad the range so quantization never clamps the extremes flat.
        lo -= 1.0f;
        hi += 1.0f;

        b3HeightFieldDef hfd = {};
        hfd.heights = m_heights.data();
        hfd.materialIndices = nullptr;
        hfd.countX = kGroundCells;
        hfd.countZ = kGroundCells;
        // Y scale of 1: heights are already in world units.
        hfd.scale = { cell, 1.0f, cell };
        hfd.globalMinimumHeight = lo;
        hfd.globalMaximumHeight = hi;
        hfd.clockwiseWinding = false;

        m_heightField = b3CreateHeightField(&hfd);
        if (!m_heightField) return;

        // The field's origin is the body's position, so anchor the body at the
        // grid's corner rather than its centre.
        b3BodyDef gd = b3DefaultBodyDef();
        gd.type = b3_staticBody;              // height fields must be static
        gd.position = { x0, 0.0f, z0 };
        b3BodyId ground = b3CreateBody(m_world, &gd);

        b3ShapeDef gsd = b3DefaultShapeDef();
        gsd.baseMaterial.friction = 0.9f;
        gsd.baseMaterial.restitution = 0.0f;
        b3CreateHeightFieldShape(ground, &gsd, m_heightField);
    }

    // Plant one palm. `lean` bends the trunk so a grove isn't a row of poles.
    void Plant(float x, float z, float height = 7.0f, float lean = 0.0f) {
        if (B3_IS_NULL(m_world)) return;

        Tree tree;
        tree.x = x;
        tree.z = z;
        tree.baseY = m_terrain ? m_terrain(x, z) : 0.0f;
        tree.sizeScale = height / 7.0f;
        tree.lean = lean * tree.sizeScale;
        const float seed = static_cast<float>(m_trees.size()) * 17.17f;
        // Asset uses layered alpha cards that only form a coherent crown near
        // its authored orientation. Large rotations expose isolated card groups.
        tree.crownYaw = XMConvertToRadians(
            -18.0f + 36.0f * Variation01(x, z, seed + 1.0f));

        // Slice the palm into `count` trunk pieces (once, on first plant). Uniform
        // model scale = planted height / the model's normalised height.
        const int count = kTrunkSegments;
        PalmModel::Load(count);
        if (PalmModel::Loaded() && PalmModel::ModelHeight() > 0.0f) {
            tree.modelScale = height / PalmModel::ModelHeight();
            // Preserve authored proportions. Previous XZ clamp made tall palms
            // scale only vertically, producing identical narrow crowns.
            tree.modelScaleXZ = tree.modelScale;
        }
        // Physics must end where visible trunk ends, not at full tree height.
        // The previous full-height boxes put cuts and crown collisions ~25% above
        // their rendered geometry.
        const float trunkHeight = PalmModel::Loaded()
            ? PalmModel::CrownBaseY() * tree.modelScale
            : height;
        tree.segLen = trunkHeight / count;

        for (int i = 0; i < count; ++i) {
            const float t = (float)i / std::max(1, count - 1);
            Segment s;
            s.radius  = kTrunkBase * tree.sizeScale * (1.0f - 0.35f * t);
            s.offX    = tree.lean * t * t;
            s.centerY = tree.baseY + tree.segLen * (i + 0.5f);
            s.health  = kSegmentHealth;
            tree.segments.push_back(s);
        }

        BuildStanding(tree);
        m_trees.push_back(std::move(tree));
        RebuildItems();
    }

    void Update(float dt) {
        if (B3_IS_NULL(m_world)) return;
        CollectRetiredMeshes();
        m_previousWindTime = m_windTime;
        m_windTime += dt;
        bool burnedTreeFell = false;
        for (Tree& tree : m_trees) {
            if (tree.felled || tree.burningTime <= 0.0f) continue;
            tree.burningTime -= dt;
            if (tree.burningTime > 0.0f) continue;
            const float windAngle = m_windTime * 0.075f + 0.65f;
            FellTree(tree, 0, 0.0f,
                { std::cos(windAngle), 0.0f, std::sin(windAngle) });
            burnedTreeFell = true;
        }
        if (burnedTreeFell) RebuildItems();
        if (m_activeBodies == 0) {
            // Loaded palms now bend in the vertex shader, so standing-tree CPU
            // transforms stay immutable. Box fallback still needs CPU wind.
            if (!PalmModel::Loaded()) RebuildItems();
            return;
        }

        m_accumulator = std::min(0.1f, m_accumulator + dt);
        constexpr float step = 1.0f / 60.0f;
        while (m_accumulator >= step) {
            b3World_Step(m_world, step, 8);
            m_accumulator -= step;
        }
        SettleLogs();
        RebuildItems();
    }

    PalmWindFrameDX12 GetWindFrame() const {
        PalmWindFrameDX12 frame;
        frame.wind = { m_windTime, m_previousWindTime,
                       m_windStrength, m_windSpeed };
        frame.primary = { m_primaryHelicopter.x, m_primaryHelicopter.y,
                          m_primaryHelicopter.z,
                          m_primaryHelicopterWind ? 1.0f : 0.0f };
        frame.secondary = { m_secondaryHelicopter.x, m_secondaryHelicopter.y,
                            m_secondaryHelicopter.z,
                            m_secondaryHelicopterWind ? 1.0f : 0.0f };
        frame.previousPrimary = {
            m_previousPrimaryHelicopter.x, m_previousPrimaryHelicopter.y,
            m_previousPrimaryHelicopter.z,
            m_previousPrimaryHelicopterWind ? 1.0f : 0.0f };
        frame.previousSecondary = {
            m_previousSecondaryHelicopter.x, m_previousSecondaryHelicopter.y,
            m_previousSecondaryHelicopter.z,
            m_previousSecondaryHelicopterWind ? 1.0f : 0.0f };
        frame.params = { m_helicopterWindRadius, m_helicopterWindStrength,
                         PalmModel::Loaded() ? PalmModel::ModelHeight() : 8.0f,
                         0.0f };
        return frame;
    }

    bool BlocksSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                       float radius) const {
        if (B3_IS_NULL(m_world)) return false;
        const XMVECTOR a = XMLoadFloat3(&start);
        const XMVECTOR b = XMLoadFloat3(&end);
        const XMVECTOR ab = b - a;
        const float abLenSq = std::max(1e-6f, XMVectorGetX(XMVector3LengthSq(ab)));
        auto blocked = [&](const XMVECTOR& center, float rad, float halfLen) {
            float t = XMVectorGetX(XMVector3Dot(center - a, ab)) / abLenSq;
            t = std::max(0.0f, std::min(1.0f, t));
            const float distance = XMVectorGetX(
                XMVector3Length(center - (a + ab * t)));
            return distance <= radius + std::max(rad, halfLen);
        };
        for (const Tree& tree : m_trees) {
            const int standTop = tree.felled ? tree.cutIndex : (int)tree.segments.size();
            for (int i = 0; i < standTop; ++i) {
                const Segment& segment = tree.segments[i];
                if (blocked(XMVectorSet(tree.x + segment.offX, segment.centerY,
                                        tree.z, 0.0f),
                            segment.radius, tree.segLen * 0.5f))
                    return true;
            }
        }
        for (const Log& log : m_logs) {
            if (B3_IS_NULL(log.body)) continue;
            const XMMATRIX transform = BodyTransform(log.body);
            for (const Piece& piece : log.pieces) {
                if (piece.frond) continue;
                const XMVECTOR worldPosition = XMVector3Transform(
                    XMVectorSet(piece.localPos.x, piece.localPos.y,
                                piece.localPos.z, 1.0f), transform);
                if (blocked(worldPosition, std::max(piece.half.x, piece.half.z),
                            piece.half.y))
                    return true;
            }
        }
        return false;
    }

    // Shoot the grove. Damages whichever trunk segment the bullet passes closest
    // to -- standing tree or fallen log alike. When that segment's health runs out
    // the thing splits there: the part above the cut becomes a new dynamic log.
    bool Shoot(const XMFLOAT3& start, const XMFLOAT3& end,
               const XMFLOAT3& direction, float radius, float damage,
               XMFLOAT3& hitPos) {
        if (B3_IS_NULL(m_world)) return false;

        const XMVECTOR a = XMLoadFloat3(&start);
        const XMVECTOR b = XMLoadFloat3(&end);
        const XMVECTOR ab = b - a;

        float    bestT = FLT_MAX;
        float    bestModelY = 0.0f;
        Tree*    bestTree = nullptr;
        size_t   bestLog  = SIZE_MAX;   // index into m_logs; SIZE_MAX = none
        int      bestSeg  = -1;
        XMFLOAT3 bestPos{};

        // Capsule test against the actual trunk axis. The old bounding-sphere
        // approximation could register air beside a trunk and choose the wrong
        // height band.
        auto test = [&](const XMVECTOR& axisStart, const XMVECTOR& axisEnd,
                        float rad, float modelYStart, float modelYEnd,
                        Tree* tree, size_t log, int seg) {
            float bulletT = 0.0f;
            float trunkT = 0.0f;
            const float distanceSq = ClosestSegmentDistanceSquared(
                a, b, axisStart, axisEnd, bulletT, trunkT);
            const float reach = radius + rad;
            if (distanceSq > reach * reach || bulletT >= bestT) return;

            bestT = bulletT;
            bestTree = tree;
            bestLog = log;
            bestSeg = seg;
            bestModelY =
                modelYStart + (modelYEnd - modelYStart) * trunkT;
            XMStoreFloat3(&bestPos, a + ab * bulletT);
        };

        // Standing trees (their still-upright segments).
        for (Tree& tree : m_trees) {
            const int standTop = tree.felled ? tree.cutIndex : (int)tree.segments.size();
            for (int i = 0; i < standTop; ++i) {
                const Segment& s = tree.segments[i];
                const XMVECTOR center =
                    XMVectorSet(tree.x + s.offX, s.centerY, tree.z, 0.0f);
                const XMVECTOR half =
                    XMVectorSet(0.0f, tree.segLen * 0.5f, 0.0f, 0.0f);
                const float modelYStart = tree.modelScale > 1e-5f
                    ? (s.centerY - tree.segLen * 0.5f - tree.baseY) /
                        tree.modelScale
                    : 0.0f;
                const float modelYEnd = tree.modelScale > 1e-5f
                    ? (s.centerY + tree.segLen * 0.5f - tree.baseY) /
                        tree.modelScale
                    : 0.0f;
                test(center - half, center + half, s.radius,
                     modelYStart, modelYEnd, &tree, SIZE_MAX, i);
            }
        }

        // Fallen logs: transform each trunk piece by its body's current pose.
        for (size_t li = 0; li < m_logs.size(); ++li) {
            const Log& log = m_logs[li];
            if (B3_IS_NULL(log.body)) continue;
            const XMMATRIX xf = BodyTransform(log.body);
            for (const Piece& p : log.pieces) {
                if (p.frond) continue;                     // fronds aren't structural
                const XMVECTOR localStart = XMVectorSet(
                    p.localPos.x, p.localPos.y - p.half.y, p.localPos.z, 1.0f);
                const XMVECTOR localEnd = XMVectorSet(
                    p.localPos.x, p.localPos.y + p.half.y, p.localPos.z, 1.0f);
                test(XMVector3Transform(localStart, xf),
                     XMVector3Transform(localEnd, xf),
                     std::max(p.half.x, p.half.z),
                     p.modelYLo, p.modelYHi, nullptr, li, p.seg);
            }
        }

        if (bestSeg < 0) return false;
        hitPos = bestPos;

        // Drain the struck segment's health, wherever it lives.
        Segment& seg = bestTree ? bestTree->segments[bestSeg]
                                : m_logs[bestLog].segments[bestSeg];
        seg.health -= damage;
        seg.hitDirectionX += direction.x * damage;
        seg.hitDirectionZ += direction.z * damage;
        if (seg.health > 0.0f) return true;   // chewing through, not severed yet

        const XMFLOAT3 fractureDirection =
            FallDir({ seg.hitDirectionX, 0.0f, seg.hitDirectionZ });
        if (bestTree)
            FellTree(*bestTree, bestSeg, bestModelY, fractureDirection);
        else
            SplitLog(bestLog, bestSeg, bestModelY, fractureDirection);
        RebuildItems();
        return true;
    }

    void ApplyExplosion(const XMFLOAT3& center, float radius) {
        if (B3_IS_NULL(m_world) || radius <= 0.0f) return;
        bool changed = false;
        for (Tree& tree : m_trees) {
            if (tree.felled) continue;
            const float dx = tree.x - center.x;
            const float dz = tree.z - center.z;
            const float trunkTop = tree.baseY +
                tree.segLen * static_cast<float>(tree.segments.size());
            const float nearestY = std::clamp(center.y, tree.baseY, trunkTop);
            const float dy = nearestY - center.y;
            if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
            const int cut = std::clamp(
                static_cast<int>((nearestY - tree.baseY) / tree.segLen),
                0, static_cast<int>(tree.segments.size()) - 1);
            const float modelCutY = tree.modelScale > 1e-5f
                ? (nearestY - tree.baseY) / tree.modelScale
                : static_cast<float>(cut);
            FellTree(tree, cut, modelCutY, { dx, 0.0f, dz });
            changed = true;
        }

        // Blast pressure keeps affecting already-felled sections. This is what
        // lets a second grenade roll or flip a fallen crown instead of treating
        // it as frozen scenery.
        for (Log& log : m_logs) {
            if (B3_IS_NULL(log.body)) continue;
            const b3Pos position = b3Body_GetPosition(log.body);
            float dx = static_cast<float>(position.x) - center.x;
            float dy = static_cast<float>(position.y) - center.y;
            float dz = static_cast<float>(position.z) - center.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance >= radius) continue;
            if (distance > 1e-3f) {
                const float inverseDistance = 1.0f / distance;
                dx *= inverseDistance;
                dy *= inverseDistance;
                dz *= inverseDistance;
            } else {
                dx = 1.0f;
                dy = 0.35f;
                dz = 0.0f;
            }
            if (log.asleep) {
                b3Body_SetType(log.body, b3_dynamicBody);
                log.asleep = false;
                ++m_activeBodies;
            }
            const float falloff = 1.0f - distance / radius;
            const float impulse =
                std::max(1.0f, b3Body_GetMass(log.body)) *
                kExplosionPush * falloff;
            b3Body_ApplyLinearImpulseToCenter(
                log.body, { dx * impulse, dy * impulse, dz * impulse }, true);
        }
        if (changed) RebuildItems();
    }

    bool IgniteNear(const XMFLOAT3& center, float radius) {
        if (radius <= 0.0f) return false;
        bool ignited = false;
        for (Tree& tree : m_trees) {
            if (tree.felled) continue;
            const float dx = tree.x - center.x;
            const float dz = tree.z - center.z;
            if (dx * dx + dz * dz > radius * radius) continue;
            if (tree.burningTime > 0.0f) continue;
            tree.burningTime = 10.0f;
            ignited = true;
        }
        return ignited;
    }

    std::vector<XMFLOAT3> GetBurningPositions() const {
        std::vector<XMFLOAT3> positions;
        for (const Tree& tree : m_trees) {
            if (tree.felled || tree.burningTime <= 0.0f) continue;
            positions.push_back({ tree.x, tree.baseY + 1.25f, tree.z });
        }
        return positions;
    }

    const std::vector<TreeItem>& GetItems() const { return m_items; }
    bool IsInitialized() const { return !B3_IS_NULL(m_world); }

    void Shutdown() {
        // Destroy the world first: it holds the shape that references the height
        // field, so the field has to outlive it.
        if (!B3_IS_NULL(m_world)) b3DestroyWorld(m_world);
        m_world = b3_nullWorldId;
        if (m_heightField) {
            b3DestroyHeightField(m_heightField);
            m_heightField = nullptr;
        }
        m_heights.clear();
        m_trees.clear();
        m_logs.clear();
        m_items.clear();
        m_retiredMeshes.clear();
        m_accumulator = 0.0f;
        m_activeBodies = 0;
    }

    ~PalmTrees() { Shutdown(); }

private:
    // Fine localized sections let bullets chew a narrow break ring before the
    // upper trunk separates, rather than snapping the tree in six huge blocks.
    static constexpr int   kTrunkSegments = 18;
    static constexpr float kSegmentLen    = 1.2f;
    static constexpr float kTrunkBase     = 0.17f;
    // Default bullets deal 15 damage. Two hits sever the struck band.
    static constexpr float kSegmentHealth = 30.0f;
    static constexpr int   kFronds        = 7;
    static constexpr float kFrondLen      = 2.6f;
    static constexpr float kFrondWidth    = 0.28f;
    static constexpr float kFrondDroop    = -0.35f;
    static constexpr float kGroundHalf    = 2.0f;   // flat-slab fallback thickness
    static constexpr float kGroundSpan    = 70.0f;  // half-width of the collision field
    static constexpr int   kGroundCells   = 129;    // samples per side (~1.1 m spacing)

    // Both per-kg, so they read as a target velocity rather than a raw shove:
    // "start the fall at this speed", independent of how much wood came off. Keep
    // them low -- gravity brings the tree down, the impulse only picks the
    // direction. Raising these launches the log instead of toppling it.
    static constexpr float kTopple = 0.6f;
    static constexpr float kToppleAngularSpeed = 0.8f;
    static constexpr float kExplosionPush = 2.8f;

    // Far above real wood (~700) on purpose: a palm trunk is thin, and at honest
    // density the log is light enough for the break impulse to fling it about.
    // The extra mass gives it the inertia to simply tip and fall under its weight.
    static constexpr float kWoodDensity  = 4000.0f;
    static constexpr float kFrondDensity = 600.0f;

    // Where a chunk of trunk is, and how chewed. Never a body in its own right.
    struct Segment {
        float radius  = 0.0f;
        float offX    = 0.0f;   // lean offset from the trunk axis
        float centerY = 0.0f;   // only meaningful while standing
        float health  = 0.0f;
        float hitDirectionX = 0.0f;
        float hitDirectionZ = 0.0f;
    };

    // One shape welded onto a log body, in that body's LOCAL frame. Trunk pieces
    // record which segment they are, so a log stays shootable: damage a piece and
    // we know whose health to drain and where to cut.
    struct Piece {
        XMFLOAT3 localPos{};
        XMFLOAT4 localRot{ 0.0f, 0.0f, 0.0f, 1.0f };
        XMFLOAT3 half{};
        bool     frond = false;
        int      seg = -1;   // index into the owning Log's segments; -1 for fronds

        // For drawing the real palm model instead of a box: which model slice this
        // piece stands in for, and the uniform scale to draw it at. `modelSeg` is a
        // trunk-slice index; crown pieces set `crown`. -1 means "no model, draw box".
        int      modelSeg = -1;
        bool     crown = false;
        float    modelScale = 1.0f;
        float    modelScaleXZ = 1.0f;
        XMFLOAT3 modelOrigin{};   // where the slice's local origin sits, in body frame
        float    crownYaw = 0.0f;
        float    modelYLo = 0.0f;
        float    modelYHi = 0.0f;
    };

    // A felled section of trunk: one dynamic body carrying its pieces. Shootable,
    // and splittable again into two smaller logs.
    struct Log {
        b3BodyId body = b3_nullBodyId;
        std::vector<Piece>   pieces;
        std::vector<Segment> segments;   // parallel to the trunk pieces' seg indices
        std::shared_ptr<SceneMesh> trunkMesh;
        XMFLOAT3 fallDirection{ 1.0f, 0.0f, 0.0f };
        bool mustFall = true;
        bool asleep = false;
    };

    struct Tree {
        std::vector<Segment> segments;
        float x = 0.0f, z = 0.0f, baseY = 0.0f, lean = 0.0f, segLen = 0.0f;
        float sizeScale = 1.0f; // uniform world scale relative to a 7 m palm
        b3BodyId standing = b3_nullBodyId;
        int  cutIndex = 0;
        bool felled = false;
        float cutModelY = 0.0f;
        std::shared_ptr<SceneMesh> stumpMesh;
        float modelScale = 1.0f;   // planted height / model height
        float modelScaleXZ = 1.0f; // uniform with modelScale for authored proportions
        // Stable yaw breaks cloned silhouettes without separating crown from trunk.
        float crownYaw = 0.0f;
        float burningTime = 0.0f;
    };

    struct RetiredMesh {
        uint64_t fenceValue = 0;
        std::shared_ptr<SceneMesh> mesh;
    };

    // ---- helpers ------------------------------------------------------------

    static XMMATRIX BodyTransform(b3BodyId body) {
        const b3Pos  p = b3Body_GetPosition(body);
        const b3Quat q = b3Body_GetRotation(body);
        return XMMatrixRotationQuaternion(XMVectorSet(q.v.x, q.v.y, q.v.z, q.s)) *
               XMMatrixTranslation((float)p.x, (float)p.y, (float)p.z);
    }

    static float Variation01(float x, float z, float salt) {
        const float value = std::sin(x * 12.9898f + z * 78.233f + salt * 37.719f) *
                            43758.5453f;
        return value - std::floor(value);
    }

    void RetireMesh(const std::shared_ptr<SceneMesh>& mesh) {
        if (!mesh) return;
        m_retiredMeshes.push_back({ g_dx12.lastDirectFenceValue, mesh });
    }

    void CollectRetiredMeshes() {
        if (!g_dx12.fence) return;
        const uint64_t completed = g_dx12.fence->GetCompletedValue();
        m_retiredMeshes.erase(std::remove_if(
            m_retiredMeshes.begin(), m_retiredMeshes.end(),
            [completed](const RetiredMesh& retired) {
                return retired.fenceValue <= completed;
            }), m_retiredMeshes.end());
    }

    static float ClosestSegmentDistanceSquared(
        const XMVECTOR& p0, const XMVECTOR& p1,
        const XMVECTOR& q0, const XMVECTOR& q1,
        float& pFraction, float& qFraction) {
        const XMVECTOR u = p1 - p0;
        const XMVECTOR v = q1 - q0;
        const XMVECTOR w = p0 - q0;
        const float uu = XMVectorGetX(XMVector3Dot(u, u));
        const float uv = XMVectorGetX(XMVector3Dot(u, v));
        const float vv = XMVectorGetX(XMVector3Dot(v, v));
        const float uw = XMVectorGetX(XMVector3Dot(u, w));
        const float vw = XMVectorGetX(XMVector3Dot(v, w));
        const float denominator = uu * vv - uv * uv;

        pFraction = denominator > 1e-6f
            ? std::clamp((uv * vw - vv * uw) / denominator, 0.0f, 1.0f)
            : 0.0f;
        qFraction = vv > 1e-6f
            ? (uv * pFraction + vw) / vv
            : 0.0f;

        if (qFraction < 0.0f) {
            qFraction = 0.0f;
            pFraction = uu > 1e-6f ? std::clamp(-uw / uu, 0.0f, 1.0f) : 0.0f;
        } else if (qFraction > 1.0f) {
            qFraction = 1.0f;
            pFraction = uu > 1e-6f
                ? std::clamp((uv - uw) / uu, 0.0f, 1.0f)
                : 0.0f;
        }

        const XMVECTOR delta =
            (p0 + u * pFraction) - (q0 + v * qFraction);
        return XMVectorGetX(XMVector3LengthSq(delta));
    }

    // Low-frequency ambient bend plus radial rotor wash from both helicopters.
    // Each tree gets a stable phase, preventing the grove from moving as one.
    XMFLOAT4 WindRotation(const Tree& tree, float heightFraction) const {
        const float phase = tree.x * 0.19f + tree.z * 0.13f;
        const float t = m_windTime * m_windSpeed;
        const float gust = std::sin(t + phase) + 0.35f * std::sin(t * 2.37f + phase * 1.71f);
        const float bend = m_windStrength * 0.13f * gust * heightFraction * heightFraction;
        const float heading = 0.35f + 0.22f * std::sin(t * 0.17f);
        float bendX = std::cos(heading) * bend;
        float bendZ = std::sin(heading) * bend;

        auto addRotorWash = [&](const XMFLOAT3& helicopter, bool enabled) {
            if (!enabled) return;
            const float dx = tree.x - helicopter.x;
            const float dz = tree.z - helicopter.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance >= m_helicopterWindRadius) return;
            const float falloff = std::pow(
                1.0f - distance / m_helicopterWindRadius, 0.65f);
            const float pulse = 0.88f + 0.12f * std::sin(
                m_windTime * 22.0f + distance * 1.7f + phase);
            const float rotorBend = m_helicopterWindStrength * falloff * pulse *
                                    heightFraction * heightFraction;
            const float invDistance = distance > 1e-3f ? 1.0f / distance : 0.0f;
            bendX += (distance > 1e-3f ? dx * invDistance : 1.0f) * rotorBend;
            bendZ += (distance > 1e-3f ? dz * invDistance : 0.0f) * rotorBend;
        };
        addRotorWash(m_primaryHelicopter, m_primaryHelicopterWind);
        addRotorWash(m_secondaryHelicopter, m_secondaryHelicopterWind);

        const float bendMagnitude = std::sqrt(bendX * bendX + bendZ * bendZ);
        if (bendMagnitude <= 1e-5f) return { 0.0f, 0.0f, 0.0f, 1.0f };
        const float totalBend = (std::min)(0.34f, bendMagnitude);
        const XMVECTOR axis = XMVectorSet(
            bendZ / bendMagnitude, 0.0f, -bendX / bendMagnitude, 0.0f);
        XMFLOAT4 result;
        XMStoreFloat4(&result, XMQuaternionRotationAxis(axis, totalBend));
        return result;
    }

    // Weld one piece onto a body as an offset/rotated box shape.
    void AddPieceShape(b3BodyId body, const Piece& p) {
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = p.frond ? kFrondDensity : kWoodDensity;
        sd.baseMaterial.friction = p.frond ? 0.7f : 0.85f;
        sd.baseMaterial.restitution = 0.0f;

        b3Transform xf;
        xf.p = { p.localPos.x, p.localPos.y, p.localPos.z };
        xf.q = { { p.localRot.x, p.localRot.y, p.localRot.z }, p.localRot.w };
        b3BoxHull hull = b3MakeTransformedBoxHull(p.half.x, p.half.y, p.half.z, xf);
        b3CreateHullShape(body, &sd, &hull.base);
    }

    // ---- standing tree ------------------------------------------------------

    void BuildStanding(Tree& tree) {
        if (!B3_IS_NULL(tree.standing)) {
            b3DestroyBody(tree.standing);
            tree.standing = b3_nullBodyId;
        }
        const int top = tree.felled ? tree.cutIndex : (int)tree.segments.size();
        float partialHeight = 0.0f;
        if (tree.felled && top < static_cast<int>(tree.segments.size())) {
            const float cutWorldY =
                tree.baseY + tree.cutModelY * tree.modelScale;
            const float partialBottom = tree.baseY + tree.segLen * top;
            partialHeight = std::clamp(
                cutWorldY - partialBottom, 0.0f, tree.segLen);
        }
        if (top <= 0 && partialHeight <= 1e-3f) return;

        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_staticBody;
        bd.position = { tree.x, tree.baseY, tree.z };
        tree.standing = b3CreateBody(m_world, &bd);

        for (int i = 0; i < top; ++i) {
            const Segment& s = tree.segments[i];
            Piece p;
            p.localPos = XMFLOAT3(s.offX, s.centerY - tree.baseY, 0.0f);
            p.half = XMFLOAT3(s.radius, tree.segLen * 0.5f, s.radius);
            AddPieceShape(tree.standing, p);
        }
        if (partialHeight > 1e-3f) {
            const Segment& segment = tree.segments[top];
            Piece p;
            p.localPos = XMFLOAT3(
                segment.offX, tree.segLen * top + partialHeight * 0.5f,
                0.0f);
            p.half = XMFLOAT3(
                segment.radius, partialHeight * 0.5f, segment.radius);
            AddPieceShape(tree.standing, p);
        }

        if (!tree.felled) {
            const float crownY = tree.baseY + tree.segLen * tree.segments.size();
            for (Piece p : CrownPieces(tree, crownY)) {
                p.localPos = XMFLOAT3(p.localPos.x - tree.x,
                                      p.localPos.y - tree.baseY,
                                      p.localPos.z - tree.z);
                AddPieceShape(tree.standing, p);
            }
        }
    }

    // Crown fronds in WORLD space, fanned around the trunk top at `crownY`.
    std::vector<Piece> CrownPieces(const Tree& tree, float crownY) const {
        std::vector<Piece> out;
        out.reserve(kFronds);
        const float topX = tree.x + tree.lean;
        const float frondLength = kFrondLen * tree.sizeScale;
        for (int f = 0; f < kFronds; ++f) {
            const float leafSeed = static_cast<float>(f) * 11.73f;
            const float angleJitter = XMConvertToRadians(
                -1.5f + 3.0f * Variation01(tree.x, tree.z, leafSeed + 6.0f));
            const float ang = tree.crownYaw + (XM_2PI * f) / kFronds + angleJitter;
            const float lengthScale = 0.97f + 0.06f *
                Variation01(tree.x, tree.z, leafSeed + 7.0f);
            const float droop = kFrondDroop + XMConvertToRadians(
                -1.0f + 2.0f * Variation01(tree.x, tree.z, leafSeed + 8.0f));
            const float variedLength = frondLength * lengthScale;
            Piece p;
            p.localPos = XMFLOAT3(
                topX + std::cos(ang) * variedLength * 0.5f,
                crownY + std::sin(droop) * variedLength * 0.5f,
                tree.z + std::sin(ang) * variedLength * 0.5f);
            const b3Quat q = b3MulQuat(
                b3MakeQuatFromAxisAngle({ 0.0f, 1.0f, 0.0f }, -ang),
                b3MakeQuatFromAxisAngle({ 0.0f, 0.0f, 1.0f }, droop));
            p.localRot = XMFLOAT4(q.v.x, q.v.y, q.v.z, q.s);
            p.half = XMFLOAT3(variedLength * 0.5f,
                              0.04f * tree.sizeScale,
                              kFrondWidth * tree.sizeScale *
                                  (0.98f + 0.04f * Variation01(
                                      tree.x, tree.z, leafSeed + 9.0f)));
            p.frond = true;
            out.push_back(p);
        }
        return out;
    }

    // ---- cutting ------------------------------------------------------------

    // Spawn a log body at `origin` from `pieces` (already in that origin's frame),
    // shove it along `dir`, and register it. `segments` carries the health of its
    // trunk pieces so the log can be shot again.
    //
    // Returns an INDEX, not a reference: this push_back can reallocate m_logs, so
    // any Log& held across this call would dangle. Callers must re-index.
    size_t SpawnLog(const XMFLOAT3& origin, std::vector<Piece> pieces,
                    std::vector<Segment> segments, const XMFLOAT3& dir,
                    std::shared_ptr<SceneMesh> trunkMesh = {}) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = { origin.x, origin.y, origin.z };
        // Kill the sideways drift from the break so the log doesn't sail away;
        // leave rotation comparatively free so it can topple.
        bd.linearDamping  = 0.9f;
        bd.angularDamping = 0.05f;

        Log log;
        log.body = b3CreateBody(m_world, &bd);
        log.pieces = std::move(pieces);
        log.segments = std::move(segments);
        log.trunkMesh = std::move(trunkMesh);
        log.fallDirection = FallDir(dir);

        for (const Piece& p : log.pieces) AddPieceShape(log.body, p);

        const float mass = std::max(1.0f, b3Body_GetMass(log.body));
        const b3Vec3 imp = { dir.x * kTopple * mass, 0.0f, dir.z * kTopple * mass };
        b3Body_ApplyLinearImpulseToCenter(log.body, imp, true);
        // Set a minimum angular speed instead of scaling an impulse by mass.
        // Angular impulse divided by a tall trunk's large inertia was sometimes
        // too small, leaving the new body balanced upright on its cut face.
        const b3Vec3 spin = {
            -log.fallDirection.z * kToppleAngularSpeed, 0.0f,
             log.fallDirection.x * kToppleAngularSpeed
        };
        b3Body_SetAngularVelocity(log.body, spin);

        ++m_activeBodies;
        m_logs.push_back(std::move(log));
        return m_logs.size() - 1;
    }

    // Horizontal unit fall direction from the shot.
    static XMFLOAT3 FallDir(const XMFLOAT3& direction) {
        XMVECTOR d = XMVectorSet(direction.x, 0.0f, direction.z, 0.0f);
        if (XMVectorGetX(XMVector3LengthSq(d)) < 1e-4f)
            d = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        XMFLOAT3 out;
        XMStoreFloat3(&out, XMVector3Normalize(d));
        return out;
    }

    // Fell a standing tree at segment `cut`: the stump stays, everything above
    // becomes one dynamic log (crown included).
    void FellTree(Tree& tree, int cut, float modelCutY,
                  const XMFLOAT3& direction) {
        if (tree.felled) return;
        const int count = (int)tree.segments.size();
        if (cut >= count) return;

        if (PalmModel::Loaded()) {
            modelCutY = std::clamp(modelCutY, 0.08f,
                (std::max)(0.09f, PalmModel::CrownBaseY() - 0.08f));
        } else {
            modelCutY = tree.modelScale > 1e-5f
                ? tree.segLen * (cut + 0.5f) / tree.modelScale
                : tree.segLen * (cut + 0.5f);
        }
        const float cutWorldY = tree.baseY + modelCutY * tree.modelScale;
        const XMFLOAT3 origin(tree.x + tree.segments[cut].offX,
                              cutWorldY,
                              tree.z);

        std::vector<Piece>   pieces;
        std::vector<Segment> segs;
        std::shared_ptr<SceneMesh> upperMesh;
        if (PalmModel::Loaded()) {
            const std::shared_ptr<SceneMesh> whole =
                PalmMeshCutter::BuildWholeTrunk();
            PalmMeshCut meshCut = PalmMeshCutter::Cut(
                whole, modelCutY, { direction.x, direction.z });
            PalmMeshCutter::Upload(meshCut.lower);
            PalmMeshCutter::Upload(meshCut.upper);
            tree.stumpMesh = std::move(meshCut.lower);
            upperMesh = std::move(meshCut.upper);
        }

        // The palm model's base (trunk foot), expressed in the new log body's frame.
        // Every slice this log carries is drawn relative to this point, so the whole
        // felled section stays one continuous tree rather than sliding apart.
        const XMFLOAT3 modelOrigin(tree.x - origin.x,
                                   tree.baseY - origin.y,
                                   tree.z - origin.z);

        for (int i = cut; i < count; ++i) {
            const Segment& s = tree.segments[i];
            Piece p;
            float pieceBottom = s.centerY - tree.segLen * 0.5f;
            const float pieceTop = s.centerY + tree.segLen * 0.5f;
            if (i == cut) pieceBottom = cutWorldY;
            const float pieceHeight = (std::max)(0.01f, pieceTop - pieceBottom);
            p.localPos = XMFLOAT3(
                tree.x + s.offX - origin.x,
                (pieceBottom + pieceTop) * 0.5f - origin.y, 0.0f);
            p.half = XMFLOAT3(s.radius, pieceHeight * 0.5f, s.radius);
            p.seg = (int)segs.size();          // index within the new log
            p.modelSeg = i;                    // model slice this piece stands in for
            p.modelScale = tree.modelScale;
            p.modelScaleXZ = tree.modelScaleXZ;
            p.modelOrigin = modelOrigin;
            p.modelYLo = i == cut
                ? modelCutY
                : static_cast<float>(i) * PalmModel::CrownBaseY() / count;
            p.modelYHi =
                static_cast<float>(i + 1) * PalmModel::CrownBaseY() / count;
            pieces.push_back(p);
            segs.push_back(s);                 // carries its remaining health
        }

        const float crownY = tree.baseY + tree.segLen * count;
        bool crownAssigned = false;
        for (Piece p : CrownPieces(tree, crownY)) {
            p.localPos = XMFLOAT3(p.localPos.x - origin.x,
                                  p.localPos.y - origin.y,
                                  p.localPos.z - origin.z);
            // Exactly ONE frond piece draws the crown mesh; the others exist only
            // as collision shapes. Marking them all once made a felled tree draw
            // the entire (very dense) crown seven times per frame, which was
            // enough GPU time to trip the watchdog and remove the device.
            if (!crownAssigned) {
                p.crown = true;
                p.modelSeg = count;            // sentinel: non-negative so it renders
                p.modelScale = tree.modelScale;
                p.modelScaleXZ = tree.modelScaleXZ;
                p.modelOrigin = modelOrigin;
                p.crownYaw = tree.crownYaw;
                crownAssigned = true;
            }
            pieces.push_back(p);               // seg stays -1: fronds are not structural
        }

        tree.cutIndex = cut;
        tree.cutModelY = modelCutY;
        tree.felled = true;
        BuildStanding(tree);

        SpawnLog(origin, std::move(pieces), std::move(segs),
                 FallDir(direction), std::move(upperMesh));
    }

    // Split a fallen log at segment `cut`: the log keeps the pieces below the cut,
    // and everything from `cut` up becomes a new log. Because a log can lie at any
    // angle, "above the cut" means further along its own trunk axis, not upward in
    // world space -- so we split by segment index, and rebuild both bodies.
    //
    // Takes an INDEX, not a Log&: SpawnLog below can reallocate m_logs, so a
    // reference held by the caller across this call would dangle.
    void SplitLog(size_t logIdx, int cut, float modelCutY,
                  const XMFLOAT3& direction) {
        Log& log = m_logs[logIdx];
        const int segCount = (int)log.segments.size();
        if (cut < 0 || cut >= segCount) return;

        const XMMATRIX xf = BodyTransform(log.body);

        const Piece* cutPiece = nullptr;
        for (const Piece& p : log.pieces)
            if (!p.frond && p.seg == cut) { cutPiece = &p; break; }
        if (!cutPiece) return;

        const float minimumCut = cutPiece->modelYLo + 0.015f;
        const float maximumCut = cutPiece->modelYHi - 0.015f;
        if (minimumCut >= maximumCut) {
            log.segments[cut].health = kSegmentHealth * 0.5f;
            return;
        }
        modelCutY = std::clamp(modelCutY, minimumCut, maximumCut);
        const float cutLocalY =
            cutPiece->modelOrigin.y + modelCutY * cutPiece->modelScale;
        const XMFLOAT3 localOrigin(
            cutPiece->localPos.x, cutLocalY, cutPiece->localPos.z);

        XMFLOAT3 origin;
        XMStoreFloat3(&origin, XMVector3Transform(
            XMVectorSet(localOrigin.x, localOrigin.y,
                        localOrigin.z, 1.0f), xf));

        const b3Quat bq = b3Body_GetRotation(log.body);
        const XMVECTOR bodyRot = XMVectorSet(bq.v.x, bq.v.y, bq.v.z, bq.s);
        const XMMATRIX rotOnly = XMMatrixRotationQuaternion(bodyRot);

        XMFLOAT3 localDirection{};
        XMStoreFloat3(&localDirection, XMVector3TransformNormal(
            XMLoadFloat3(&direction), XMMatrixTranspose(rotOnly)));
        const std::shared_ptr<SceneMesh> sourceMesh = log.trunkMesh
            ? log.trunkMesh : PalmMeshCutter::BuildWholeTrunk();
        PalmMeshCut meshCut = PalmMeshCutter::Cut(
            sourceMesh, modelCutY,
            { localDirection.x, localDirection.z });
        PalmMeshCutter::Upload(meshCut.lower);
        PalmMeshCutter::Upload(meshCut.upper);

        std::vector<Piece> upper, lower;
        std::vector<Segment> upperSegs, lowerSegs;

        auto rebaseUpper = [&](Piece piece) {
            piece.localPos.x -= localOrigin.x;
            piece.localPos.y -= localOrigin.y;
            piece.localPos.z -= localOrigin.z;
            if (piece.modelSeg >= 0) {
                piece.modelOrigin.x -= localOrigin.x;
                piece.modelOrigin.y -= localOrigin.y;
                piece.modelOrigin.z -= localOrigin.z;
            }
            return piece;
        };

        for (const Piece& p : log.pieces) {
            if (p.frond || p.seg > cut) {
                Piece np = rebaseUpper(p);
                if (!p.frond) np.seg = p.seg - cut;
                upper.push_back(np);
            } else if (p.seg < cut) {
                lower.push_back(p);
            } else {
                const float bottom = p.localPos.y - p.half.y;
                const float top = p.localPos.y + p.half.y;
                const float split = std::clamp(
                    cutLocalY, bottom + 0.005f, top - 0.005f);

                Piece lowerPiece = p;
                lowerPiece.localPos.y = (bottom + split) * 0.5f;
                lowerPiece.half.y = (split - bottom) * 0.5f;
                lowerPiece.modelYHi = modelCutY;
                lower.push_back(lowerPiece);

                Piece upperPiece = p;
                upperPiece.localPos.y = (split + top) * 0.5f;
                upperPiece.half.y = (top - split) * 0.5f;
                upperPiece.modelYLo = modelCutY;
                upperPiece.seg = 0;
                upper.push_back(rebaseUpper(upperPiece));
            }
        }

        for (int i = 0; i < segCount; ++i) {
            if (i <= cut) lowerSegs.push_back(log.segments[i]);
            if (i >= cut) upperSegs.push_back(log.segments[i]);
        }
        if (!lowerSegs.empty()) {
            lowerSegs.back().health = kSegmentHealth;
            lowerSegs.back().hitDirectionX = 0.0f;
            lowerSegs.back().hitDirectionZ = 0.0f;
        }
        if (!upperSegs.empty()) {
            upperSegs[0].health = kSegmentHealth;
            upperSegs[0].hitDirectionX = 0.0f;
            upperSegs[0].hitDirectionZ = 0.0f;
        }

        // Rebuild the existing body with only the lower pieces. Simplest correct
        // route: destroy and recreate at the same pose, since Box3D has no
        // "remove all shapes" that also refreshes the mass properties cleanly.
        const b3Pos  lp = b3Body_GetPosition(log.body);
        const b3Quat lq = b3Body_GetRotation(log.body);
        const b3Vec3 lv = b3Body_GetLinearVelocity(log.body);
        const b3Vec3 lw = b3Body_GetAngularVelocity(log.body);
        const bool wasAsleep = log.asleep;

        b3DestroyBody(log.body);
        if (wasAsleep && m_activeBodies > 0) {
            // It was static; it is about to become dynamic again below.
        }

        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = lp;
        bd.rotation = lq;
        bd.linearDamping  = 0.9f;
        bd.angularDamping = 0.05f;
        log.body = b3CreateBody(m_world, &bd);
        for (const Piece& p : lower) AddPieceShape(log.body, p);
        b3Body_SetLinearVelocity(log.body, lv);
        b3Body_SetAngularVelocity(log.body, lw);

        log.pieces = std::move(lower);
        log.segments = std::move(lowerSegs);
        RetireMesh(log.trunkMesh);
        log.trunkMesh = std::move(meshCut.lower);
        if (wasAsleep) {
            log.asleep = false;      // it is dynamic again, so it must be counted
            ++m_activeBodies;
        }

        // And the freed upper half becomes its own log, nudged along the shot.
        // Careful: SpawnLog may reallocate m_logs, which would invalidate `log`.
        // Nothing below touches `log`, and we index the new entry rather than
        // holding a reference to it.
        const XMFLOAT3 dir = FallDir(direction);
        const size_t idx = SpawnLog(
            origin, std::move(upper), std::move(upperSegs), dir,
            std::move(meshCut.upper));
        // Match the parent's orientation so the split reads as a clean break
        // rather than the new piece snapping to an axis.
        b3Body_SetTransform(m_logs[idx].body, { origin.x, origin.y, origin.z }, lq);
        const b3Vec3 upperLinear = b3Body_GetLinearVelocity(m_logs[idx].body);
        const b3Vec3 upperAngular = b3Body_GetAngularVelocity(m_logs[idx].body);
        b3Body_SetLinearVelocity(m_logs[idx].body,
            { upperLinear.x + lv.x, upperLinear.y + lv.y,
              upperLinear.z + lv.z });
        b3Body_SetAngularVelocity(m_logs[idx].body,
            { upperAngular.x + lw.x, upperAngular.y + lw.y,
              upperAngular.z + lw.z });
    }

    // A log that has come to rest freezes back to static: costs nothing again.
    void SettleLogs() {
        for (Log& log : m_logs) {
            if (log.asleep || B3_IS_NULL(log.body)) continue;
            const b3Vec3 v = b3Body_GetLinearVelocity(log.body);
            const b3Vec3 w = b3Body_GetAngularVelocity(log.body);
            const float lin = (float)(v.x * v.x + v.y * v.y + v.z * v.z);
            const float ang = (float)(w.x * w.x + w.y * w.y + w.z * w.z);

            // Never freeze a freshly detached section while it is still standing.
            // If collision friction kills its spin, wake it with the guaranteed
            // topple speed until its trunk axis is close to the ground.
            if (log.mustFall) {
                const b3Quat q = b3Body_GetRotation(log.body);
                const XMVECTOR up = XMVector3Rotate(
                    XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                    XMVectorSet(q.v.x, q.v.y, q.v.z, q.s));
                if (std::abs(XMVectorGetY(up)) <= 0.45f) {
                    log.mustFall = false;
                } else {
                    constexpr float retrySpeedSq =
                        kToppleAngularSpeed * kToppleAngularSpeed * 0.25f;
                    if (ang < retrySpeedSq) {
                        b3Body_SetAngularVelocity(log.body, {
                            -log.fallDirection.z * kToppleAngularSpeed, 0.0f,
                             log.fallDirection.x * kToppleAngularSpeed
                        });
                    }
                    continue;
                }
            }

            if (lin < 0.03f && ang < 0.03f) {
                b3Body_SetType(log.body, b3_staticBody);
                log.asleep = true;
                if (m_activeBodies > 0) --m_activeBodies;
            }
        }
    }

    // ---- drawing ------------------------------------------------------------

    void RebuildItems() {
        m_items.clear();
        const bool model = PalmModel::Loaded();

        for (const Tree& tree : m_trees) {
            const int standTop = tree.felled ? tree.cutIndex : (int)tree.segments.size();
            if (model && tree.felled && tree.stumpMesh) {
                TreeItem stump;
                const XMMATRIX transform =
                    XMMatrixScaling(tree.modelScaleXZ, tree.modelScale,
                                    tree.modelScaleXZ) *
                    XMMatrixTranslation(tree.x, tree.baseY, tree.z);
                XMStoreFloat4x4(&stump.transform, transform);
                stump.color = TrunkColor(kSegmentHealth);
                stump.meshOverride = tree.stumpMesh;
                stump.modelScale = tree.modelScale;
                m_items.push_back(std::move(stump));
            } else for (int i = 0; i < standTop; ++i) {
                const Segment& s = tree.segments[i];
                const float heightFraction = (i + 0.5f) / (float)(std::max)(1, standTop);
                if (model) {
                    // Draw this segment's slice of the real palm. The slice already
                    // carries its own height within the model, so it is placed from
                    // the TREE BASE -- not from the box's centre, which would double
                    // up the height offset.
                    m_items.push_back(ModelItem(tree, XMFLOAT4(0, 0, 0, 1),
                                                XMFLOAT3(tree.x, tree.baseY, tree.z),
                                                i, false, TrunkColor(s.health)));
                } else {
                    const XMFLOAT4 windRot = WindRotation(tree, heightFraction);
                    const XMVECTOR rel = XMVectorSet(s.offX, s.centerY - tree.baseY, 0, 0);
                    XMFLOAT3 bent;
                    XMStoreFloat3(&bent, XMVector3Rotate(rel, XMLoadFloat4(&windRot)));
                    m_items.push_back(BoxItem(
                        XMFLOAT3(tree.x + bent.x, tree.baseY + bent.y, tree.z + bent.z),
                        windRot,
                        XMFLOAT3(s.radius, tree.segLen * 0.5f, s.radius),
                        TrunkColor(s.health), false));
                }
            }
            if (!tree.felled) {
                if (model) {
                    m_items.push_back(ModelItem(tree, XMFLOAT4(0, 0, 0, 1),
                                                XMFLOAT3(tree.x, tree.baseY, tree.z),
                                                -1, true, kFrondColor));
                } else {
                    const XMFLOAT4 crownWind = WindRotation(tree, 1.0f);
                    const float crownY = tree.baseY + tree.segLen * tree.segments.size();
                    for (const Piece& p : CrownPieces(tree, crownY)) {
                        const XMVECTOR rel = XMVectorSet(p.localPos.x - tree.x,
                            p.localPos.y - tree.baseY, p.localPos.z - tree.z, 0);
                        XMFLOAT3 bent;
                        XMStoreFloat3(&bent, XMVector3Rotate(rel, XMLoadFloat4(&crownWind)));
                        XMFLOAT4 rotation;
                        XMStoreFloat4(&rotation, XMQuaternionMultiply(
                            XMLoadFloat4(&p.localRot), XMLoadFloat4(&crownWind)));
                        m_items.push_back(BoxItem(
                            XMFLOAT3(tree.x + bent.x, tree.baseY + bent.y, tree.z + bent.z),
                            rotation, p.half,
                                                  kFrondColor, true));
                    }
                }
            }
        }

        for (const Log& log : m_logs) {
            if (B3_IS_NULL(log.body)) continue;
            const XMMATRIX bodyXf = BodyTransform(log.body);

            if (model && log.trunkMesh) {
                const Piece* modelPiece = nullptr;
                for (const Piece& piece : log.pieces)
                    if (!piece.frond && piece.modelSeg >= 0) {
                        modelPiece = &piece;
                        break;
                    }
                if (modelPiece) {
                    TreeItem trunk;
                    const XMMATRIX local =
                        XMMatrixScaling(modelPiece->modelScaleXZ,
                                        modelPiece->modelScale,
                                        modelPiece->modelScaleXZ) *
                        XMMatrixTranslation(modelPiece->modelOrigin.x,
                                            modelPiece->modelOrigin.y,
                                            modelPiece->modelOrigin.z);
                    XMStoreFloat4x4(&trunk.transform, local * bodyXf);
                    trunk.color = TrunkColor(kSegmentHealth);
                    trunk.meshOverride = log.trunkMesh;
                    trunk.modelScale = modelPiece->modelScale;
                    m_items.push_back(std::move(trunk));
                }
            }

            for (const Piece& p : log.pieces) {
                // With the model loaded, frond boxes are collision-only: the crown
                // mesh is drawn once via the single piece marked `crown`.
                if (model && p.frond && !p.crown) continue;
                if (model && log.trunkMesh && !p.frond) continue;

                TreeItem item;

                if (model && p.modelSeg >= 0) {
                    // The log's body origin is the cut point. Each piece knows which
                    // model slice it is and how far that slice's own origin sits from
                    // the cut, so we rebuild the slice's pose in the body's frame and
                    // let the body transform carry it. Uniform scale only: squashing
                    // a mesh to the box's extents would deform the trunk.
                    const XMMATRIX local =
                        XMMatrixScaling(p.modelScaleXZ,
                                        p.modelScale,
                                        p.modelScaleXZ) *
                        XMMatrixTranslation(p.modelOrigin.x, p.modelOrigin.y, p.modelOrigin.z);
                    XMStoreFloat4x4(&item.transform, local * bodyXf);
                    item.segment = p.crown ? -1 : p.modelSeg;
                    item.crown = p.crown;
                    item.palmWindRoot.w = p.crown ? p.crownYaw : 0.0f;
                    item.modelScale = p.modelScale;
                    item.color = p.crown ? kFrondColor
                                         : TrunkColor(p.seg >= 0 && p.seg < (int)log.segments.size()
                                                          ? log.segments[p.seg].health
                                                          : kSegmentHealth);
                    item.isFrond = p.crown;
                } else {
                    const XMMATRIX local =
                        XMMatrixScaling(p.half.x * 2.0f, p.half.y * 2.0f, p.half.z * 2.0f) *
                        XMMatrixRotationQuaternion(XMLoadFloat4(&p.localRot)) *
                        XMMatrixTranslation(p.localPos.x, p.localPos.y, p.localPos.z);
                    XMStoreFloat4x4(&item.transform, local * bodyXf);
                    item.color = p.frond
                        ? kFrondColor
                        : TrunkColor(p.seg >= 0 && p.seg < (int)log.segments.size()
                                         ? log.segments[p.seg].health
                                         : kSegmentHealth);
                    item.isFrond = p.frond;
                }
                m_items.push_back(item);
            }
        }
    }

    // A standing tree's model slice: uniform scale, anchored at the trunk base.
    TreeItem ModelItem(const Tree& tree, const XMFLOAT4& rot, const XMFLOAT3& base,
                       int segment, bool crown, const XMFLOAT3& color) const {
        // Keep authored geometry rigid. Scaling one height slice independently
        // opens seams and turns overhanging triangles into stretched spikes.
        // Damage remains visible through TrunkColor without deforming the mesh.
        const XMMATRIX t =
            XMMatrixScaling(tree.modelScaleXZ,
                            tree.modelScale,
                            tree.modelScaleXZ) *
            XMMatrixRotationQuaternion(XMLoadFloat4(&rot)) *
            XMMatrixTranslation(base.x, base.y, base.z);
        TreeItem item;
        XMStoreFloat4x4(&item.transform, t);
        item.color = color;
        item.segment = segment;
        item.crown = crown;
        item.isFrond = crown;
        item.modelScale = tree.modelScale;
        item.palmWindRoot = XMFLOAT4(
            tree.x, tree.z, 1.0f, crown ? tree.crownYaw : 0.0f);
        return item;
    }

    // Chewed wood darkens, so you can see where you have been shooting.
    static XMFLOAT3 TrunkColor(float health) {
        const float wear = 1.0f - 0.35f * (1.0f - std::max(0.0f, health) / kSegmentHealth);
        return XMFLOAT3(0.40f * wear, 0.28f * wear, 0.17f * wear);
    }

    TreeItem BoxItem(const XMFLOAT3& pos, const XMFLOAT4& rot, const XMFLOAT3& half,
                     const XMFLOAT3& color, bool frond) const {
        const XMMATRIX t =
            XMMatrixScaling(half.x * 2.0f, half.y * 2.0f, half.z * 2.0f) *
            XMMatrixRotationQuaternion(XMLoadFloat4(&rot)) *
            XMMatrixTranslation(pos.x, pos.y, pos.z);
        TreeItem item;
        XMStoreFloat4x4(&item.transform, t);
        item.color = color;
        item.isFrond = frond;
        return item;
    }

    static inline const XMFLOAT3 kFrondColor{ 0.16f, 0.42f, 0.13f };

    b3WorldId m_world = b3_nullWorldId;
    std::vector<Tree>     m_trees;
    std::vector<Log>      m_logs;
    std::vector<TreeItem> m_items;
    std::vector<RetiredMesh> m_retiredMeshes;
    std::function<float(float, float)> m_terrain;

    // Terrain collision. Box3D keeps a reference to the height field (it is not
    // owned by the world), so we own it and must free it ourselves in Shutdown.
    b3HeightFieldData* m_heightField = nullptr;
    std::vector<float> m_heights;

    float m_accumulator = 0.0f;
    float m_windTime = 0.0f;
    float m_previousWindTime = 0.0f;
    float m_windStrength = 0.28f;
    float m_windSpeed = 1.6f;
    XMFLOAT3 m_primaryHelicopter{};
    XMFLOAT3 m_secondaryHelicopter{};
    XMFLOAT3 m_previousPrimaryHelicopter{};
    XMFLOAT3 m_previousSecondaryHelicopter{};
    float m_helicopterWindRadius = 22.0f;
    float m_helicopterWindStrength = 0.24f;
    bool m_primaryHelicopterWind = false;
    bool m_secondaryHelicopterWind = false;
    bool m_previousPrimaryHelicopterWind = false;
    bool m_previousSecondaryHelicopterWind = false;
    int   m_activeBodies = 0;
};

extern PalmTrees g_trees;
