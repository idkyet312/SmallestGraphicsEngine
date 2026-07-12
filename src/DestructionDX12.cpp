#define NOMINMAX
#include "DestructionDX12.h"

#include "GLBImporter.h"
#include "NvBlast.h"
#include "NvBlastTkActor.h"
#include "NvBlastTkAsset.h"
#include "NvBlastTkFamily.h"
#include "NvBlastTkFramework.h"
#include "NvBlastTkGroup.h"
#include "NvBlastTypes.h"
#include <box3d/box3d.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <list>
#include <unordered_map>

using namespace DirectX;
using namespace Nv::Blast;

namespace {
constexpr uint32_t InvalidIndex = 0xFFFFFFFFu;
// Uniform starting health for every bond/chunk. A bullet's per-hit damage is a
// fraction of this, so a joint takes several hits before it lets go.
constexpr float kBondHealth = 1.0f;
TkFramework* SharedTkFramework = nullptr;
uint32_t SharedTkFrameworkUsers = 0;

TkFramework* AcquireTkFramework() {
    if (!SharedTkFramework) {
        SharedTkFramework = NvBlastTkFrameworkGet();
        if (!SharedTkFramework) SharedTkFramework = NvBlastTkFrameworkCreate();
    }
    if (SharedTkFramework) ++SharedTkFrameworkUsers;
    return SharedTkFramework;
}

void ReleaseTkFramework() {
    if (!SharedTkFramework || SharedTkFrameworkUsers == 0) return;
    if (--SharedTkFrameworkUsers == 0) {
        SharedTkFramework->release();
        SharedTkFramework = nullptr;
    }
}

struct RadialDamageParams {
    float position[3];
    float radius;
    float damage;
    uint32_t targetChunk;
};

// Marks a set of chunks whose remaining bonds should all be severed, used to
// drop pieces left hanging by fewer than a minimum number of bonds.
struct IsolateChunksParams {
    const uint8_t* breakChunk;  // indexed by asset chunk index; nonzero = sever its bonds
    uint32_t chunkCount;
};

void IsolateGraphShader(NvBlastFractureBuffers* commands,
                        const NvBlastGraphShaderActor* actor, const void* rawParams) {
    const auto& params = *static_cast<const IsolateChunksParams*>(rawParams);
    const uint32_t capacity = commands->bondFractureCount;
    commands->bondFractureCount = 0;
    commands->chunkFractureCount = 0;
    for (uint32_t node = actor->firstGraphNodeIndex; node != InvalidIndex;
         node = actor->graphNodeIndexLinks[node]) {
        for (uint32_t edge = actor->adjacencyPartition[node];
             edge < actor->adjacencyPartition[node + 1]; ++edge) {
            const uint32_t other = actor->adjacentNodeIndices[edge];
            if (node >= other || actor->nodeActorIndices[other] != actor->actorIndex) continue;
            const uint32_t chunkA = actor->chunkIndices[node];
            const uint32_t chunkB = actor->chunkIndices[other];
            const bool breakIt = (chunkA < params.chunkCount && params.breakChunk[chunkA]) ||
                                 (chunkB < params.chunkCount && params.breakChunk[chunkB]);
            if (!breakIt || commands->bondFractureCount >= capacity) continue;
            const uint32_t bondIndex = actor->adjacentBondIndices[edge];
            NvBlastBondFractureData& fracture =
                commands->bondFractures[commands->bondFractureCount++];
            fracture.userdata = actor->assetBonds[bondIndex].userData;
            fracture.nodeIndex0 = node;
            fracture.nodeIndex1 = other;
            fracture.health = FLT_MAX;  // sever completely
        }
    }
}

// Sever only the bonds that cross OUT of one plank group: a bond breaks when
// exactly one of its two chunks belongs to the target group. Bonds internal to
// the plank (both endpoints in the group) survive, so the whole plank frees off
// the wall as one connected cluster that can still fracture further on a later
// hit. chunkGroup is indexed by asset chunk index (root at 0 has group -2).
struct IsolateGroupParams {
    const int* chunkGroup;   // asset-chunk-indexed plank id (-1 = none)
    uint32_t chunkCount;
    int targetGroup;
};

void IsolateGroupShader(NvBlastFractureBuffers* commands,
                        const NvBlastGraphShaderActor* actor, const void* rawParams) {
    const auto& params = *static_cast<const IsolateGroupParams*>(rawParams);
    const uint32_t capacity = commands->bondFractureCount;
    commands->bondFractureCount = 0;
    commands->chunkFractureCount = 0;
    for (uint32_t node = actor->firstGraphNodeIndex; node != InvalidIndex;
         node = actor->graphNodeIndexLinks[node]) {
        for (uint32_t edge = actor->adjacencyPartition[node];
             edge < actor->adjacencyPartition[node + 1]; ++edge) {
            const uint32_t other = actor->adjacentNodeIndices[edge];
            if (node >= other || actor->nodeActorIndices[other] != actor->actorIndex) continue;
            const uint32_t chunkA = actor->chunkIndices[node];
            const uint32_t chunkB = actor->chunkIndices[other];
            const int gA = chunkA < params.chunkCount ? params.chunkGroup[chunkA] : -1;
            const int gB = chunkB < params.chunkCount ? params.chunkGroup[chunkB] : -1;
            const bool inA = gA == params.targetGroup;
            const bool inB = gB == params.targetGroup;
            if (inA == inB || commands->bondFractureCount >= capacity) continue;  // keep internal + far bonds
            const uint32_t bondIndex = actor->adjacentBondIndices[edge];
            NvBlastBondFractureData& fracture =
                commands->bondFractures[commands->bondFractureCount++];
            fracture.userdata = actor->assetBonds[bondIndex].userData;
            fracture.nodeIndex0 = node;
            fracture.nodeIndex1 = other;
            fracture.health = FLT_MAX;  // sever completely
        }
    }
}

void RadialGraphShader(NvBlastFractureBuffers* commands,
                       const NvBlastGraphShaderActor* actor, const void* rawParams) {
    const auto& params = *static_cast<const RadialDamageParams*>(rawParams);
    const uint32_t capacity = commands->bondFractureCount;
    commands->bondFractureCount = 0;
    commands->chunkFractureCount = 0;
    const float radiusSquared = params.radius * params.radius;

    for (uint32_t node = actor->firstGraphNodeIndex; node != InvalidIndex;
         node = actor->graphNodeIndexLinks[node]) {
        for (uint32_t edge = actor->adjacencyPartition[node];
             edge < actor->adjacencyPartition[node + 1]; ++edge) {
            const uint32_t other = actor->adjacentNodeIndices[edge];
            if (node >= other || actor->nodeActorIndices[other] != actor->actorIndex) continue;
            const uint32_t bondIndex = actor->adjacentBondIndices[edge];
            const NvBlastBond& bond = actor->assetBonds[bondIndex];
            const float dx = bond.centroid[0] - params.position[0];
            const float dy = bond.centroid[1] - params.position[1];
            const float dz = bond.centroid[2] - params.position[2];
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            // Only bonds whose seam lies within the blast radius take damage, and
            // the damage falls off with distance so the impact stays local
            // instead of severing an entire wall in one shot.
            if (distanceSquared > radiusSquared || commands->bondFractureCount >= capacity) continue;
            const float falloff = 1.0f - std::sqrt(distanceSquared) / std::max(0.0001f, params.radius);
            NvBlastBondFractureData& fracture =
                commands->bondFractures[commands->bondFractureCount++];
            fracture.userdata = bond.userData;
            fracture.nodeIndex0 = node;
            fracture.nodeIndex1 = other;
            fracture.health = params.damage * std::max(0.15f, falloff);
        }
    }
}

XMMATRIX BoxTransform(const b3BodyId body, const XMFLOAT3& center) {
    const b3Pos p = b3Body_GetPosition(body);
    const b3Quat q = b3Body_GetRotation(body);
    const XMVECTOR rotation = XMVectorSet(q.v.x, q.v.y, q.v.z, q.s);
    return XMMatrixTranslation(-center.x, -center.y, -center.z) *
           XMMatrixRotationQuaternion(rotation) * XMMatrixTranslation((float)p.x, (float)p.y, (float)p.z);
}

bool SphereAabb(const XMFLOAT3& p, float radius, const XMFLOAT3& lo, const XMFLOAT3& hi) {
    const float x = std::max(lo.x, std::min(p.x, hi.x));
    const float y = std::max(lo.y, std::min(p.y, hi.y));
    const float z = std::max(lo.z, std::min(p.z, hi.z));
    const float dx = p.x - x, dy = p.y - y, dz = p.z - z;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

bool SegmentAabb(const XMFLOAT3& start, const XMFLOAT3& end, float radius,
                 const XMFLOAT3& lo, const XMFLOAT3& hi, float& hitT) {
    const float s[3] = { start.x, start.y, start.z };
    const float d[3] = { end.x - start.x, end.y - start.y, end.z - start.z };
    const float lower[3] = { lo.x - radius, lo.y - radius, lo.z - radius };
    const float upper[3] = { hi.x + radius, hi.y + radius, hi.z + radius };
    float t0 = 0.0f, t1 = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (s[axis] < lower[axis] || s[axis] > upper[axis]) return false;
            continue;
        }
        float a = (lower[axis] - s[axis]) / d[axis];
        float b = (upper[axis] - s[axis]) / d[axis];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a); t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    hitT = t0;
    return true;
}
}

struct DestructionDX12::Impl {
    struct Chunk {
        std::shared_ptr<SceneNode> node;
        XMFLOAT3 minimum = {};
        XMFLOAT3 maximum = {};
        XMFLOAT3 center = {};
        std::vector<XMFLOAT3> collisionPoints;
        int x = 0, y = 0, z = 0;
        bool support = false;  // anchored to the world (foundation, sill plates)
        // Cladding sub-pieces of the same board share a plank id (>=0). A hit
        // frees the whole plank as a cluster; sub-pieces stay bonded so they can
        // fracture further later. -1 = a standalone chunk (stud, roof, ...).
        int plankGroup = -1;
        bool glass = false;    // window pane cell: any hit shatters the whole pane
        bool sheet = false;    // corrugated roof sheet: a hit tears the whole sheet off
    };

    struct ActorRuntime {
        TkActor* actor = nullptr;
        std::vector<uint32_t> chunks;
        b3BodyId body = b3_nullBodyId;
        XMFLOAT3 center = {};
        bool dynamic = false;
        bool wasSubmerged = false;   // for splash-on-entry detection
    };

    struct BodySeed {
        bool valid = false;
        XMFLOAT3 modelCenter = {};
        b3Pos position = {};
        b3Quat rotation = b3Quat_identity;
        b3Vec3 linearVelocity = {};
        b3Vec3 angularVelocity = {};
    };

    DestructionDX12* owner = nullptr;
    std::shared_ptr<SceneNode> source;
    ID3D12Device* device = nullptr;
    int gridX = 4, gridY = 3, gridZ = 4;
    struct BondPair { uint32_t a = 0, b = 0; };  // chunk indices (0-based)
    std::vector<Chunk> chunks;
    std::vector<BondPair> bondPairs;              // for debug visualization
    std::vector<int> chunkGroupByAsset;           // asset-chunk-indexed plank id (root=[0]=-2)
    std::list<std::unique_ptr<ActorRuntime>> actors;
    std::vector<DestructionRenderItem> renderItems;
    struct RagdollPart {
        b3BodyId body = b3_nullBodyId;
        XMFLOAT3 half = {};
        XMFLOAT3 color = {};
        bool wasSubmerged = false;   // for splash-on-entry detection
    };
    std::vector<RagdollPart> ragdollParts;
    std::vector<RagdollRenderItem> ragdollRenderItems;
    mutable int lastRagdollHit = -1;
    TkFramework* framework = nullptr;
    TkAsset* asset = nullptr;
    TkFamily* family = nullptr;
    TkGroup* group = nullptr;
    b3WorldId world = b3_nullWorldId;
    b3BodyId ground = b3_nullBodyId;
    // Optional terrain-height sampler. When set, the ground is a static
    // heightfield matching the drawn terrain, so debris rests on the real hills
    // and rolls into the basin instead of hovering on a flat plane.
    std::function<float(float, float)> terrainSampler;
    // Called (x, z, strength) when a body first breaks the water surface, so the
    // pool can spawn a ripple/splash. Set from main.
    std::function<void(float, float, float)> splashCallback;
    // World positions where the building just fractured a piece loose this frame;
    // drained by the caller to spawn smoke at the actual break points.
    std::vector<XMFLOAT3> breakPoints;
    float accumulator = 0.0f;
    XMFLOAT3 lastDamagePosition = {};
    float lastDamageRadius = 0.0f;
    bool initialized = false;

    // Optional water region: any dynamic fragment whose centre lies within this
    // AABB gets buoyancy so house debris knocked into the pool floats.
    bool  waterEnabled = false;
    XMFLOAT3 waterMin = {};
    XMFLOAT3 waterMax = {};
    float waterSurfaceY = 0.0f;

    // Archimedes buoyancy on any dynamic fragment sitting in the water AABB,
    // matching the standalone WaterVolume so house pieces and crates behave the
    // same in the pool.
    // Fire the splash callback when a body first pierces the surface while
    // moving down: returns the now-submerged state for the caller to store.
    bool SplashOnEntry(const b3Pos& p, bool wasSubmerged, bool nowSubmerged,
                       float vy) {
        if (splashCallback && nowSubmerged && !wasSubmerged && vy < -1.2f)
            splashCallback((float)p.x, (float)p.z, std::min(2.5f, -vy * 0.18f));
        return nowSubmerged;
    }

    bool InWaterColumn(const b3Pos& p) const {
        return !(p.x < waterMin.x || p.x > waterMax.x ||
                 p.z < waterMin.z || p.z > waterMax.z ||
                 p.y - 1.0f > waterMax.y);
    }

    void ApplyWaterBuoyancy() {
        if (!waterEnabled) return;
        constexpr float kWaterDensity = 1000.0f;
        constexpr float kGravity = 9.81f;
        for (auto& runtime : actors) {
            if (!runtime->dynamic || B3_IS_NULL(runtime->body)) continue;
            const b3Pos p = b3Body_GetPosition(runtime->body);
            if (!InWaterColumn(p)) { runtime->wasSubmerged = false; continue; }

            // Approximate the piece as a ~0.5m tall box for the submerged span.
            constexpr float halfH = 0.4f;
            const float bottom = (float)p.y - halfH;
            const float boxH = halfH * 2.0f;
            float submerged = (waterSurfaceY - bottom) / boxH;
            submerged = std::max(0.0f, std::min(1.0f, submerged));

            const b3Vec3 vel = b3Body_GetLinearVelocity(runtime->body);
            runtime->wasSubmerged =
                SplashOnEntry(p, runtime->wasSubmerged, submerged > 0.0f, vel.y);
            if (submerged <= 0.0f) continue;

            const float mass = std::max(0.05f, b3Body_GetMass(runtime->body));
            // Displaced-volume force scaled so a light fragment rides high; drag
            // settles the bob and viscosity kills horizontal drift.
            const float volume = std::max(0.02f, mass / 8.0f);   // density ~8
            const float buoyancy = kWaterDensity * kGravity * volume * submerged * 0.02f;
            const b3Vec3 force = {
                -vel.x * mass * 4.0f * submerged,
                buoyancy - vel.y * mass * 6.0f * submerged,
                -vel.z * mass * 4.0f * submerged
            };
            b3Body_ApplyForceToCenter(runtime->body, force, true);
        }

        // Ragdolls float too: each body part is buoyed against its real box
        // volume (density 55 << water), so a corpse knocked into the pool bobs
        // face-up-ish and drifts instead of sinking.
        for (RagdollPart& part : ragdollParts) {
            if (B3_IS_NULL(part.body)) continue;
            const b3Pos p = b3Body_GetPosition(part.body);
            if (!InWaterColumn(p)) { part.wasSubmerged = false; continue; }

            const float bottom = (float)p.y - part.half.y;
            const float boxH = std::max(1e-3f, part.half.y * 2.0f);
            float submerged = (waterSurfaceY - bottom) / boxH;
            submerged = std::max(0.0f, std::min(1.0f, submerged));

            const b3Vec3 vel = b3Body_GetLinearVelocity(part.body);
            part.wasSubmerged =
                SplashOnEntry(p, part.wasSubmerged, submerged > 0.0f, vel.y);
            if (submerged <= 0.0f) continue;

            const float mass = std::max(0.02f, b3Body_GetMass(part.body));
            const float volume = part.half.x * 2.0f * part.half.y * 2.0f * part.half.z * 2.0f;
            const float displaced = volume * submerged;
            // Full Archimedes lift (limbs are far less dense than water) plus drag.
            const float buoyancy = kWaterDensity * kGravity * displaced;
            const float vDrag = -vel.y * mass * 8.0f * submerged;
            const b3Vec3 force = {
                -vel.x * mass * 5.0f * submerged,
                buoyancy + vDrag,
                -vel.z * mass * 5.0f * submerged
            };
            b3Body_ApplyForceToCenter(part.body, force, true);
        }
    }

    bool BuildChunks() {
        if (!source || !device) return false;

        // Procedural Voronoi wall supplies one closed convex-prism child per
        // fracture chunk. Preserve those exact touching cells instead of
        // repartitioning their triangles through the regular grid.
        if (!source->children.empty()) {
            uint32_t chunkIndex = 0;
            for (const auto& sourceChunk : source->children) {
                if (!sourceChunk || !sourceChunk->mesh) continue;
                // Roof geometry now comes from build/models/roof/roof.fbx.
                // Do not turn the procedural roof panels into render chunks.
                if (sourceChunk->name.rfind("Roof@", 0) == 0 ||
                    sourceChunk->name.rfind("MetalRoof@", 0) == 0) continue;
                Chunk chunk;
                chunk.x = (int)(chunkIndex % (uint32_t)gridX);
                chunk.y = (int)((chunkIndex / (uint32_t)gridX) % (uint32_t)gridY);
                chunk.z = (int)(chunkIndex / (uint32_t)(gridX * gridY));
                // Pieces authored with a "Support:" name prefix stay anchored to
                // the world; they hold the structure up until disconnected.
                chunk.support = sourceChunk->name.rfind("Support:", 0) == 0;
                // "<piece>@<id>" tags a Voronoi board (cladding plank, stud,
                // roof slab): all cells of one board carry the same id so they
                // bond into a single piece until it is shot apart.
                const size_t at = sourceChunk->name.find('@');
                if (at != std::string::npos) {
                    chunk.plankGroup = std::atoi(sourceChunk->name.c_str() + at + 1);
                }
                chunk.glass = sourceChunk->name.rfind("Glass@", 0) == 0;
                chunk.sheet = sourceChunk->name.rfind("Roof@", 0) == 0;
                const bool simpleRoofBox = sourceChunk->name.rfind("ImportedRoof@", 0) == 0;
                chunk.minimum = { FLT_MAX, FLT_MAX, FLT_MAX };
                chunk.maximum = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                chunk.node = std::make_shared<SceneNode>("VoronoiChunk");
                chunk.node->mesh = std::make_shared<SceneMesh>();
                for (const MeshPrimitive& sourcePrimitive : sourceChunk->mesh->primitives) {
                    if (sourcePrimitive.indices.empty()) continue;
                    MeshPrimitive primitive;
                    primitive.vertices = sourcePrimitive.vertices;
                    primitive.indices = sourcePrimitive.indices;
                    primitive.material = sourcePrimitive.material;
                    primitive.materialIndex = sourcePrimitive.materialIndex;
                    for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                        chunk.minimum.x = std::min(chunk.minimum.x, primitive.vertices[v]);
                        chunk.minimum.y = std::min(chunk.minimum.y, primitive.vertices[v + 1]);
                        chunk.minimum.z = std::min(chunk.minimum.z, primitive.vertices[v + 2]);
                        chunk.maximum.x = std::max(chunk.maximum.x, primitive.vertices[v]);
                        chunk.maximum.y = std::max(chunk.maximum.y, primitive.vertices[v + 1]);
                        chunk.maximum.z = std::max(chunk.maximum.z, primitive.vertices[v + 2]);
                        chunk.collisionPoints.push_back({ primitive.vertices[v],
                            primitive.vertices[v + 1], primitive.vertices[v + 2] });
                    }
                    if (!GLBImporter::BuildMeshletData(primitive, device)) return false;
                    chunk.node->mesh->primitives.push_back(std::move(primitive));
                }
                if (chunk.minimum.x == FLT_MAX) continue;
                if (simpleRoofBox && !chunk.collisionPoints.empty()) {
                    // Fit one thin oriented box to the pitched sheet. This
                    // avoids oversized vertical AABBs while keeping collision
                    // geometry simple (eight points, one convex box).
                    float meanX=0,meanY=0;
                    for(const XMFLOAT3& p:chunk.collisionPoints){meanX+=p.x;meanY+=p.y;}
                    const float inv=1.0f/(float)chunk.collisionPoints.size();meanX*=inv;meanY*=inv;
                    float covXX=0,covXY=0;
                    for(const XMFLOAT3& p:chunk.collisionPoints){covXX+=(p.x-meanX)*(p.x-meanX);covXY+=(p.x-meanX)*(p.y-meanY);}
                    const float slope=covXX>0.000001f?covXY/covXX:0.0f;
                    const float il=1.0f/std::sqrt(1.0f+slope*slope);
                    const XMFLOAT3 u(il,slope*il,0), n(u.y,-u.x,0);
                    float u0=FLT_MAX,u1=-FLT_MAX,z0=FLT_MAX,z1=-FLT_MAX,n0=FLT_MAX,n1=-FLT_MAX;
                    for(const XMFLOAT3& p:chunk.collisionPoints){
                        const float pu=p.x*u.x+p.y*u.y,pn=p.x*n.x+p.y*n.y;
                        u0=(std::min)(u0,pu);u1=(std::max)(u1,pu);z0=(std::min)(z0,p.z);z1=(std::max)(z1,p.z);
                        n0=(std::min)(n0,pn);n1=(std::max)(n1,pn);
                    }
                    const float midN=(n0+n1)*0.5f;n0=midN-0.035f;n1=midN+0.035f;
                    chunk.collisionPoints.clear();
                    for(float pu:{u0,u1})for(float pz:{z0,z1})for(float pn:{n0,n1})
                        chunk.collisionPoints.push_back({u.x*pu+n.x*pn,u.y*pu+n.y*pn,pz});
                }
                chunk.center = { (chunk.minimum.x + chunk.maximum.x) * 0.5f,
                                 (chunk.minimum.y + chunk.maximum.y) * 0.5f,
                                 (chunk.minimum.z + chunk.maximum.z) * 0.5f };
                chunks.push_back(std::move(chunk));
                ++chunkIndex;
            }
            return !chunks.empty();
        }

        if (!source->mesh) return false;
        XMFLOAT3 sceneMin(FLT_MAX, FLT_MAX, FLT_MAX);
        XMFLOAT3 sceneMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const MeshPrimitive& primitive : source->mesh->primitives) {
            for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                sceneMin.x = std::min(sceneMin.x, primitive.vertices[v]);
                sceneMin.y = std::min(sceneMin.y, primitive.vertices[v + 1]);
                sceneMin.z = std::min(sceneMin.z, primitive.vertices[v + 2]);
                sceneMax.x = std::max(sceneMax.x, primitive.vertices[v]);
                sceneMax.y = std::max(sceneMax.y, primitive.vertices[v + 1]);
                sceneMax.z = std::max(sceneMax.z, primitive.vertices[v + 2]);
            }
        }
        if (sceneMin.x == FLT_MAX) return false;
        const XMFLOAT3 cellSize((sceneMax.x - sceneMin.x) / gridX,
                                (sceneMax.y - sceneMin.y) / gridY,
                                (sceneMax.z - sceneMin.z) / gridZ);

        struct CellBuild {
            std::vector<MeshPrimitive> primitives;
            bool occupied = false;
        };
        std::vector<CellBuild> cells((size_t)gridX * gridY * gridZ);
        for (CellBuild& cell : cells) cell.primitives.resize(source->mesh->primitives.size());

        struct VoronoiSite { XMFLOAT3 position; int x, y, z; };
        std::vector<VoronoiSite> sites;
        sites.reserve((size_t)gridX * gridY * gridZ);
        for (int z = 0; z < gridZ; ++z) for (int y = 0; y < gridY; ++y) for (int x = 0; x < gridX; ++x) {
            const uint32_t hash = (uint32_t)(x * 73856093 ^ y * 19349663 ^ z * 83492791);
            const float jx = ((hash & 255) / 255.0f - 0.5f) * 0.72f;
            const float jy = (((hash >> 8) & 255) / 255.0f - 0.5f) * 0.72f;
            const float jz = (((hash >> 16) & 255) / 255.0f - 0.5f) * 0.72f;
            sites.push_back({ { sceneMin.x + (x + 0.5f + jx) * cellSize.x,
                                sceneMin.y + (y + 0.5f + jy) * cellSize.y,
                                sceneMin.z + (z + 0.5f + jz) * cellSize.z }, x, y, z });
        }
        for (size_t material = 0; material < source->mesh->primitives.size(); ++material) {
            const MeshPrimitive& src = source->mesh->primitives[material];
            for (size_t tri = 0; tri + 2 < src.indices.size(); tri += 3) {
                const UINT i0 = src.indices[tri], i1 = src.indices[tri + 1], i2 = src.indices[tri + 2];
                if ((size_t)std::max({ i0, i1, i2 }) * 12 + 11 >= src.vertices.size()) continue;
                const float cx = (src.vertices[(size_t)i0 * 12] + src.vertices[(size_t)i1 * 12] + src.vertices[(size_t)i2 * 12]) / 3.0f;
                const float cy = (src.vertices[(size_t)i0 * 12 + 1] + src.vertices[(size_t)i1 * 12 + 1] + src.vertices[(size_t)i2 * 12 + 1]) / 3.0f;
                const float cz = (src.vertices[(size_t)i0 * 12 + 2] + src.vertices[(size_t)i1 * 12 + 2] + src.vertices[(size_t)i2 * 12 + 2]) / 3.0f;
                size_t nearestSite = 0;
                float nearestDistance = FLT_MAX;
                for (size_t site = 0; site < sites.size(); ++site) {
                    const float dx = (cx - sites[site].position.x) / std::max(0.001f, cellSize.x);
                    const float dy = (cy - sites[site].position.y) / std::max(0.001f, cellSize.y);
                    const float dz = (cz - sites[site].position.z) / std::max(0.001f, cellSize.z);
                    const float distance = dx * dx + dy * dy + dz * dz;
                    if (distance < nearestDistance) { nearestDistance = distance; nearestSite = site; }
                }
                CellBuild& cell = cells[nearestSite];
                MeshPrimitive& dst = cell.primitives[material];
                dst.material = src.material;
                dst.materialIndex = src.materialIndex;
                for (UINT sourceIndex : { i0, i1, i2 }) {
                    const UINT newIndex = (UINT)(dst.vertices.size() / 12);
                    const float* vertex = &src.vertices[(size_t)sourceIndex * 12];
                    dst.vertices.insert(dst.vertices.end(), vertex, vertex + 12);
                    dst.indices.push_back(newIndex);
                }
                cell.occupied = true;
            }
        }

        for (int z = 0; z < gridZ; ++z) for (int y = 0; y < gridY; ++y) for (int x = 0; x < gridX; ++x) {
            CellBuild& build = cells[(size_t)(z * gridY + y) * gridX + x];
            if (!build.occupied) continue;
            Chunk chunk;
            chunk.x = x; chunk.y = y; chunk.z = z;
            chunk.minimum = { sceneMin.x + x * cellSize.x, sceneMin.y + y * cellSize.y, sceneMin.z + z * cellSize.z };
            chunk.maximum = { sceneMin.x + (x + 1) * cellSize.x, sceneMin.y + (y + 1) * cellSize.y, sceneMin.z + (z + 1) * cellSize.z };
            chunk.center = { (chunk.minimum.x + chunk.maximum.x) * 0.5f,
                             (chunk.minimum.y + chunk.maximum.y) * 0.5f,
                             (chunk.minimum.z + chunk.maximum.z) * 0.5f };
            chunk.node = std::make_shared<SceneNode>("DestructionChunk");
            chunk.node->mesh = std::make_shared<SceneMesh>();
            for (MeshPrimitive& primitive : build.primitives) {
                if (primitive.indices.empty()) continue;
                if (!GLBImporter::BuildMeshletData(primitive, device)) return false;
                chunk.node->mesh->primitives.push_back(std::move(primitive));
            }
            chunks.push_back(std::move(chunk));
        }
        return !chunks.empty();
    }

    bool BuildBlast() {
        framework = AcquireTkFramework();
        if (!framework) return false;
        std::vector<NvBlastChunkDesc> chunkDescs(chunks.size() + 1);
        XMFLOAT3 rootCenter = {};
        float rootVolume = 0.0f;
        for (const Chunk& chunk : chunks) {
            const float volume = std::max(0.001f, (chunk.maximum.x - chunk.minimum.x) *
                (chunk.maximum.y - chunk.minimum.y) * (chunk.maximum.z - chunk.minimum.z));
            rootCenter.x += chunk.center.x * volume; rootCenter.y += chunk.center.y * volume;
            rootCenter.z += chunk.center.z * volume; rootVolume += volume;
        }
        rootCenter.x /= rootVolume; rootCenter.y /= rootVolume; rootCenter.z /= rootVolume;
        chunkDescs[0].centroid[0] = rootCenter.x; chunkDescs[0].centroid[1] = rootCenter.y;
        chunkDescs[0].centroid[2] = rootCenter.z; chunkDescs[0].volume = rootVolume;
        chunkDescs[0].parentChunkDescIndex = InvalidIndex;
        chunkDescs[0].flags = NvBlastChunkDesc::NoFlags;
        chunkDescs[0].userData = InvalidIndex;
        chunkGroupByAsset.assign(chunks.size() + 1, -1);
        chunkGroupByAsset[0] = -2;  // root: never a plank
        for (uint32_t i = 0; i < chunks.size(); ++i) {
            const Chunk& chunk = chunks[i];
            NvBlastChunkDesc& desc = chunkDescs[i + 1];
            desc.centroid[0] = chunk.center.x; desc.centroid[1] = chunk.center.y; desc.centroid[2] = chunk.center.z;
            desc.volume = std::max(0.001f, (chunk.maximum.x - chunk.minimum.x) *
                (chunk.maximum.y - chunk.minimum.y) * (chunk.maximum.z - chunk.minimum.z));
            desc.parentChunkDescIndex = 0;
            desc.flags = NvBlastChunkDesc::SupportFlag;
            desc.userData = i;
            chunkGroupByAsset[i + 1] = chunk.plankGroup;
        }

        // Bond only chunks that share a genuine, local face contact -- adjacent
        // pieces whose faces actually meet -- rather than anything that merely
        // overlaps. This keeps the support graph local: a stud bonds to its
        // immediate neighbours, not to a distant wall it happens to graze, so a
        // hit tears loose only the pieces around it.
        constexpr float touchSlop = 0.3f;       // faces may sit a small gap apart and still bond
        constexpr float minContactArea = 0.04f;  // require a substantial shared face
        constexpr uint32_t maxNeighbours = 10;   // each chunk keeps up to its 10 closest bonds

        // First gather every candidate face contact, then keep only the closest
        // few per chunk so each piece bonds to just its nearest neighbours.
        struct Candidate { uint32_t a, b; float nx, ny, nz, area, distSq; };
        std::vector<Candidate> candidates;
        for (uint32_t a = 0; a < chunks.size(); ++a) for (uint32_t b = a + 1; b < chunks.size(); ++b) {
            const Chunk& ca = chunks[a];
            const Chunk& cb = chunks[b];
            const float ox = std::min(ca.maximum.x, cb.maximum.x) - std::max(ca.minimum.x, cb.minimum.x);
            const float oy = std::min(ca.maximum.y, cb.maximum.y) - std::max(ca.minimum.y, cb.minimum.y);
            const float oz = std::min(ca.maximum.z, cb.maximum.z) - std::max(ca.minimum.z, cb.minimum.z);
            // Must touch on all axes (small negative = a hair's gap is allowed).
            if (ox < -touchSlop || oy < -touchSlop || oz < -touchSlop) continue;
            // Voronoi cells inside one plank have slanted seams that defeat the
            // axis-aligned face test below, so bond any same-plank cells whose
            // AABBs touch. Glass panes are too thin to pass the face-area test
            // at all, so they likewise bond to whatever they touch (the stud
            // stubs and cladding edges framing the window). Normal and area are
            // nominal: these bonds are only ever severed outright and bond
            // healths are uniform anyway.
            if ((ca.plankGroup >= 0 && ca.plankGroup == cb.plankGroup) ||
                ca.glass || cb.glass) {
                float nx = cb.center.x - ca.center.x, ny = cb.center.y - ca.center.y,
                      nz = cb.center.z - ca.center.z;
                const float distSq = nx * nx + ny * ny + nz * nz;
                const float len = std::sqrt(distSq);
                if (len < 0.0001f) continue;
                nx /= len; ny /= len; nz /= len;
                candidates.push_back({ a, b, nx, ny, nz, minContactArea, distSq });
                continue;
            }
            // The seam is the thinnest-overlap axis. A real face contact meets
            // near-flush there (|overlap| small): deep interpenetration means the
            // pieces are stacked/nested, not edge-adjacent -- skip those so bonds
            // stay between true neighbours.
            const float overlaps[3] = { ox, oy, oz };
            const int seam = (overlaps[0] <= overlaps[1] && overlaps[0] <= overlaps[2]) ? 0
                           : (overlaps[1] <= overlaps[2] ? 1 : 2);
            if (overlaps[seam] > touchSlop) continue;  // faces not flush -> not a seam
            float nx = 0, ny = 0, nz = 0, area = 0;
            const float clampX = std::max(0.0f, ox), clampY = std::max(0.0f, oy), clampZ = std::max(0.0f, oz);
            if (seam == 0) { nx = cb.center.x >= ca.center.x ? 1.0f : -1.0f; area = clampY * clampZ; }
            else if (seam == 1) { ny = cb.center.y >= ca.center.y ? 1.0f : -1.0f; area = clampX * clampZ; }
            else { nz = cb.center.z >= ca.center.z ? 1.0f : -1.0f; area = clampX * clampY; }
            // Reject tiny grazing contacts -- a bond needs a real shared face.
            if (area < minContactArea) continue;
            const float cdx = cb.center.x - ca.center.x, cdy = cb.center.y - ca.center.y,
                        cdz = cb.center.z - ca.center.z;
            candidates.push_back({ a, b, nx, ny, nz, area, cdx * cdx + cdy * cdy + cdz * cdz });
        }

        // Keep a candidate only if it ranks among each endpoint's closest few, so
        // every chunk bonds to just its nearest neighbours -- a super-local graph.
        std::vector<std::vector<uint32_t>> perChunk(chunks.size());  // candidate indices
        for (uint32_t i = 0; i < candidates.size(); ++i) {
            perChunk[candidates[i].a].push_back(i);
            perChunk[candidates[i].b].push_back(i);
        }
        for (auto& list : perChunk) {
            std::sort(list.begin(), list.end(), [&](uint32_t l, uint32_t r) {
                return candidates[l].distSq < candidates[r].distSq;
            });
        }
        auto rankFor = [&](uint32_t chunk, uint32_t candIndex) -> uint32_t {
            const auto& list = perChunk[chunk];
            for (uint32_t r = 0; r < list.size(); ++r) if (list[r] == candIndex) return r;
            return 0xFFFFFFFFu;
        };
        std::vector<NvBlastBondDesc> bonds;
        for (uint32_t i = 0; i < candidates.size(); ++i) {
            const Candidate& c = candidates[i];
            if (rankFor(c.a, i) >= maxNeighbours || rankFor(c.b, i) >= maxNeighbours) continue;
            NvBlastBondDesc bond = {};
            bond.chunkIndices[0] = c.a + 1; bond.chunkIndices[1] = c.b + 1;
            bond.bond.normal[0] = c.nx; bond.bond.normal[1] = c.ny; bond.bond.normal[2] = c.nz;
            bond.bond.centroid[0] = (chunks[c.a].center.x + chunks[c.b].center.x) * 0.5f;
            bond.bond.centroid[1] = (chunks[c.a].center.y + chunks[c.b].center.y) * 0.5f;
            bond.bond.centroid[2] = (chunks[c.a].center.z + chunks[c.b].center.z) * 0.5f;
            bond.bond.area = c.area;
            bond.bond.userData = (uint32_t)bonds.size();
            bonds.push_back(bond);
            bondPairs.push_back({ c.a, c.b });
        }

        TkAssetDesc assetDesc;
        assetDesc.chunkCount = (uint32_t)chunkDescs.size(); assetDesc.chunkDescs = chunkDescs.data();
        assetDesc.bondCount = (uint32_t)bonds.size(); assetDesc.bondDescs = bonds.data();
        asset = framework->createAsset(assetDesc);
        if (!asset) return false;
        TkGroupDesc groupDesc = {}; groupDesc.workerCount = 1;
        group = framework->createGroup(groupDesc);
        if (!group) return false;
        // Give every bond and chunk the same starting health so a hit removes a
        // predictable fraction of it. Default health tracks bond area, and our
        // structural pieces have tiny contact faces, so bonds would otherwise be
        // near-zero health and shatter the whole house in one shot.
        std::vector<float> bondHealths(bonds.size(), kBondHealth);
        std::vector<float> chunkHealths(chunks.size(), kBondHealth);
        TkActorDesc actorDesc(asset);
        actorDesc.initialBondHealths = bondHealths.empty() ? nullptr : bondHealths.data();
        actorDesc.initialSupportChunkHealths = chunkHealths.empty() ? nullptr : chunkHealths.data();
        TkActor* actor = framework->createActor(actorDesc);
        if (!actor) return false;
        family = &actor->getFamily();
        family->addListener(*owner);
        group->addActor(*actor);
        auto runtime = std::make_unique<ActorRuntime>();
        runtime->actor = actor;
        runtime->chunks.resize(chunks.size());
        for (uint32_t i = 0; i < chunks.size(); ++i) runtime->chunks[i] = i;
        actor->userData = runtime.get();
        actors.push_back(std::move(runtime));
        return true;
    }

    bool BuildPhysics() {
        b3WorldDef worldDef = b3DefaultWorldDef();
        worldDef.gravity = { 0.0f, -9.81f, 0.0f };
        // Collisions faster than this report hit events, which Update turns
        // into fracture damage -- debris smashing into the house or ground
        // breaks cells. Set high so only really violent impacts do damage.
        worldDef.hitEventThreshold = 3.0f;
        world = b3CreateWorld(&worldDef);
        if (B3_IS_NULL(world)) return false;
        BuildGround();
        CreateBody(*actors.front(), false, nullptr);
        CreateRagdolls();
        return true;
    }

    // (Re)build the static ground collider. With a terrain sampler, lay down a
    // grid of static boxes whose tops follow the drawn terrain (hills + pool
    // basin) so debris collides with real ground. Without one, a single flat
    // plane. Called at build time and again whenever the sampler is set.
    void BuildGround() {
        if (B3_IS_NULL(world)) return;
        if (!B3_IS_NULL(ground)) { b3DestroyBody(ground); ground = b3_nullBodyId; }

        auto addStaticBox = [&](float px, float py, float pz, float ex, float ey, float ez) {
            b3BodyDef bd = b3DefaultBodyDef();
            bd.position = { px, py, pz };
            b3BodyId body = b3CreateBody(world, &bd);
            b3BoxHull hull = b3MakeBoxHull(ex, ey, ez);
            b3ShapeDef sd = b3DefaultShapeDef();
            sd.baseMaterial.friction = 0.8f;
            sd.enableHitEvents = true;
            b3CreateHullShape(body, &sd, &hull.base);
            return body;
        };

        if (terrainSampler) {
            // Heightfield: cover the play area with a grid of columns sunk deep
            // below their terrain-height tops. `ground` keeps the first column so
            // Shutdown/rebuild can destroy the set (the rest leak until world
            // teardown, which is fine -- they are static and never rebuilt mid-run
            // except via this method, which recreates the world's ground wholesale
            // only through the single `ground` handle at init).
            constexpr float extent = 60.0f;    // half-size of the covered area
            constexpr int   cells = 60;        // resolution -> 2m columns
            constexpr float thick = 30.0f;     // column half-height (buries base)
            const float cell = (extent * 2.0f) / cells;
            const float x0 = -extent + cell * 0.5f;
            const float z0 = -extent + cell * 0.5f;
            for (int gz = 0; gz < cells; ++gz)
            for (int gx = 0; gx < cells; ++gx) {
                const float px = x0 + gx * cell;
                const float pz = z0 + gz * cell;
                const float gy = terrainSampler(px, pz);       // ground surface here
                b3BodyId col = addStaticBox(px, gy - thick, pz,
                                            cell * 0.5f + 0.02f, thick, cell * 0.5f + 0.02f);
                if (gx == 0 && gz == 0) ground = col;
            }
        } else {
            ground = addStaticBox(0.0f, -0.5f, 0.0f, 60.0f, 0.5f, 60.0f);
        }
    }

    void CreateRagdolls() {
        struct PartDef { XMFLOAT3 center, half, color; };
        const XMFLOAT3 skin{ 0.62f, 0.39f, 0.27f };
        const XMFLOAT3 shirt{ 0.12f, 0.24f, 0.42f };
        const XMFLOAT3 pants{ 0.10f, 0.11f, 0.13f };
        const PartDef defs[] = {
            {{0,1.45f,0},{0.25f,0.38f,0.15f},shirt},   // torso
            {{0,0.92f,0},{0.23f,0.16f,0.14f},pants},   // pelvis
            {{0,2.02f,0},{0.18f,0.20f,0.18f},skin},    // head
            {{-0.38f,1.48f,0},{0.12f,0.30f,0.11f},shirt},
            {{-0.38f,0.94f,0},{0.10f,0.27f,0.09f},skin},
            {{ 0.38f,1.48f,0},{0.12f,0.30f,0.11f},shirt},
            {{ 0.38f,0.94f,0},{0.10f,0.27f,0.09f},skin},
            {{-0.15f,0.53f,0},{0.14f,0.28f,0.13f},pants},
            {{-0.15f,0.04f,0},{0.12f,0.25f,0.11f},pants},
            {{ 0.15f,0.53f,0},{0.14f,0.28f,0.13f},pants},
            {{ 0.15f,0.04f,0},{0.12f,0.25f,0.11f},pants},
        };
        struct Link { int a, b; XMFLOAT3 anchor; };
        const Link links[] = {
            {0,1,{0,1.08f,0}}, {0,2,{0,1.82f,0}},
            {0,3,{-0.29f,1.68f,0}}, {3,4,{-0.38f,1.20f,0}},
            {0,5,{ 0.29f,1.68f,0}}, {5,6,{ 0.38f,1.20f,0}},
            {1,7,{-0.15f,0.76f,0}}, {7,8,{-0.15f,0.28f,0}},
            {1,9,{ 0.15f,0.76f,0}}, {9,10,{0.15f,0.28f,0}},
        };

        auto spawn = [&](XMFLOAT3 origin, float pitch, float yaw, float roll) {
            const size_t base = ragdollParts.size();
            XMVECTOR rq = XMQuaternionRotationRollPitchYaw(pitch, yaw, roll);
            XMFLOAT4 qf; XMStoreFloat4(&qf, rq);
            for (const PartDef& def : defs) {
                XMFLOAT3 rotated;
                XMStoreFloat3(&rotated, XMVector3Rotate(XMLoadFloat3(&def.center), rq));
                b3BodyDef bd = b3DefaultBodyDef();
                bd.type = b3_dynamicBody;
                bd.position = { origin.x + rotated.x, origin.y + rotated.y, origin.z + rotated.z };
                bd.rotation = { { qf.x, qf.y, qf.z }, qf.w };
                bd.linearDamping = 0.12f; bd.angularDamping = 0.35f;
                b3BodyId body = b3CreateBody(world, &bd);
                b3ShapeDef sd = b3DefaultShapeDef();
                sd.density = 55.0f; sd.baseMaterial.friction = 0.72f;
                sd.baseMaterial.restitution = 0.02f;
                b3BoxHull box = b3MakeBoxHull(def.half.x, def.half.y, def.half.z);
                b3CreateHullShape(body, &sd, &box.base);
                ragdollParts.push_back({ body, def.half, def.color });
            }
            for (const Link& link : links) {
                b3SphericalJointDef jd = b3DefaultSphericalJointDef();
                jd.base.bodyIdA = ragdollParts[base + link.a].body;
                jd.base.bodyIdB = ragdollParts[base + link.b].body;
                const XMFLOAT3& ca = defs[link.a].center;
                const XMFLOAT3& cb = defs[link.b].center;
                jd.base.localFrameA.p = { link.anchor.x-ca.x, link.anchor.y-ca.y, link.anchor.z-ca.z };
                jd.base.localFrameB.p = { link.anchor.x-cb.x, link.anchor.y-cb.y, link.anchor.z-cb.z };
                jd.base.collideConnected = false;
                jd.enableConeLimit = true; jd.coneAngle = 1.15f;
                jd.enableTwistLimit = true; jd.lowerTwistAngle = -0.65f; jd.upperTwistAngle = 0.65f;
                b3CreateSphericalJoint(world, &jd);
            }
        };

        // One body leans against the metal shed; one starts sprawled on wood roof.
        spawn({ 7.78f, 0.38f, 3.45f }, 0.0f, 0.0f, -0.28f);
        spawn({ -3.4f, 4.75f, 3.55f }, 0.0f, 0.0f, 1.48f);
    }

    void CreateBody(ActorRuntime& runtime, bool forceDynamic, const BodySeed* seed) {
        if (!B3_IS_NULL(runtime.body)) b3DestroyBody(runtime.body);
        runtime.center = {};
        for (uint32_t index : runtime.chunks) {
            runtime.center.x += chunks[index].center.x;
            runtime.center.y += chunks[index].center.y;
            runtime.center.z += chunks[index].center.z;
        }
        const float inv = 1.0f / std::max<size_t>(1, runtime.chunks.size());
        runtime.center.x *= inv; runtime.center.y *= inv; runtime.center.z *= inv;
        // Initial house is static. Every Blast split child is dynamic, including
        // foundation chunks; otherwise a projectile hitting the bottom row can
        // split correctly but appear to do nothing.
        runtime.dynamic = forceDynamic;
        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = runtime.dynamic ? b3_dynamicBody : b3_staticBody;
        bodyDef.enableSleep = true;
        bodyDef.position = { runtime.center.x, runtime.center.y, runtime.center.z };
        if (seed && seed->valid) {
            const XMVECTOR localOffset = XMVectorSet(runtime.center.x - seed->modelCenter.x,
                runtime.center.y - seed->modelCenter.y, runtime.center.z - seed->modelCenter.z, 0.0f);
            const XMVECTOR rotation = XMVectorSet(seed->rotation.v.x, seed->rotation.v.y,
                                                  seed->rotation.v.z, seed->rotation.s);
            XMFLOAT3 rotated; XMStoreFloat3(&rotated, XMVector3Rotate(localOffset, rotation));
            bodyDef.position = { (float)seed->position.x + rotated.x,
                                 (float)seed->position.y + rotated.y,
                                 (float)seed->position.z + rotated.z };
            bodyDef.rotation = seed->rotation;
            const b3Vec3 tangential = b3Cross(seed->angularVelocity,
                { rotated.x, rotated.y, rotated.z });
            bodyDef.linearVelocity = { seed->linearVelocity.x + tangential.x,
                                       seed->linearVelocity.y + tangential.y,
                                       seed->linearVelocity.z + tangential.z };
            bodyDef.angularVelocity = seed->angularVelocity;
        }
        bodyDef.linearDamping = 0.05f; bodyDef.angularDamping = 0.12f;
        runtime.body = b3CreateBody(world, &bodyDef);
        b3ShapeDef shapeDef = b3DefaultShapeDef();
        // Keep fragments light so ordinary projectile impulses move them and,
        // more importantly, so a fallen sheet resting on a ragdoll or on other
        // debris settles onto it rather than bulldozing it. Zero restitution
        // means a landing sheet does not bounce and kick things away.
        shapeDef.density = runtime.dynamic ? 8.0f : 0.0f;
        shapeDef.baseMaterial.friction = 0.75f;
        shapeDef.baseMaterial.restitution = 0.0f;
        shapeDef.enableHitEvents = true;  // hard impacts fracture cells
        for (uint32_t index : runtime.chunks) {
            const Chunk& chunk = chunks[index];
            if (!chunk.collisionPoints.empty()) {
                std::vector<b3Vec3> points;
                points.reserve(chunk.collisionPoints.size());
                for (const XMFLOAT3& point : chunk.collisionPoints) {
                    points.push_back({ point.x - runtime.center.x,
                                       point.y - runtime.center.y,
                                       point.z - runtime.center.z });
                }
                b3HullData* hull = b3CreateHull(points.data(), (int)points.size(), 16);
                if (hull) {
                    b3CreateHullShape(runtime.body, &shapeDef, hull);
                    b3DestroyHull(hull);
                    continue;
                }
            }
            const float hx = std::max(0.05f, (chunk.maximum.x - chunk.minimum.x) * 0.48f);
            const float hy = std::max(0.05f, (chunk.maximum.y - chunk.minimum.y) * 0.48f);
            const float hz = std::max(0.05f, (chunk.maximum.z - chunk.minimum.z) * 0.48f);
            const b3Vec3 offset = { chunk.center.x - runtime.center.x,
                                    chunk.center.y - runtime.center.y,
                                    chunk.center.z - runtime.center.z };
            b3BoxHull box = b3MakeOffsetBoxHull(hx, hy, hz, offset);
            b3CreateHullShape(runtime.body, &shapeDef, &box.base);
        }
        // No scripted burst on split: fragments keep only their inherited seed
        // velocity, and any push comes from the bullet's ApplyImpulse (or a
        // grenade's explosion shove). Pieces otherwise just fall.
    }

    void RebuildRenderItems() {
        renderItems.clear();
        for (const auto& runtime : actors) {
            XMMATRIX transform = XMMatrixIdentity();
            if (!B3_IS_NULL(runtime->body)) transform = BoxTransform(runtime->body, runtime->center);
            XMFLOAT4X4 stored; XMStoreFloat4x4(&stored, transform);
            for (uint32_t chunk : runtime->chunks) renderItems.push_back({ chunks[chunk].node, stored });
        }
        ragdollRenderItems.clear();
        for (const RagdollPart& part : ragdollParts) {
            const b3Pos p = b3Body_GetPosition(part.body);
            const b3Quat q = b3Body_GetRotation(part.body);
            XMVECTOR rotation = XMVectorSet(q.v.x, q.v.y, q.v.z, q.s);
            XMMATRIX transform = XMMatrixScaling(part.half.x * 2.0f, part.half.y * 2.0f, part.half.z * 2.0f) *
                XMMatrixRotationQuaternion(rotation) * XMMatrixTranslation((float)p.x, (float)p.y, (float)p.z);
            XMFLOAT4X4 stored; XMStoreFloat4x4(&stored, transform);
            ragdollRenderItems.push_back({ stored, part.color });
        }
    }

    // Sever the bonds of the single non-support cell nearest `worldPosition`
    // so it splits off. Shared by bullet strikes and physics-impact damage.
    // Caller runs DropUnderConnectedChunks + RebuildRenderItems afterwards.
    bool BreakNearestCell(const XMFLOAT3& worldPosition) {
        ActorRuntime* hitActor = nullptr;
        uint32_t hitChunk = InvalidIndex;   // 0-based chunk index of the struck piece
        float bestDistanceSquared = FLT_MAX;
        for (auto& runtime : actors) {
            const b3Vec3 local = b3Body_GetLocalPoint(runtime->body,
                { worldPosition.x, worldPosition.y, worldPosition.z });
            const XMFLOAT3 modelHit(local.x + runtime->center.x,
                                    local.y + runtime->center.y,
                                    local.z + runtime->center.z);
            for (uint32_t chunkIndex : runtime->chunks) {
                const Chunk& chunk = chunks[chunkIndex];
                const float x = std::max(chunk.minimum.x, std::min(modelHit.x, chunk.maximum.x));
                const float y = std::max(chunk.minimum.y, std::min(modelHit.y, chunk.maximum.y));
                const float z = std::max(chunk.minimum.z, std::min(modelHit.z, chunk.maximum.z));
                const float dx = modelHit.x - x, dy = modelHit.y - y, dz = modelHit.z - z;
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (distanceSquared < bestDistanceSquared) {
                    bestDistanceSquared = distanceSquared;
                    hitActor = runtime.get();
                    hitChunk = chunkIndex;
                }
            }
        }
        if (!hitActor || !hitActor->actor || hitChunk == InvalidIndex) return false;
        if (chunks[hitChunk].support) return false;        // anchored pieces shrug it off
        if (hitActor->chunks.size() <= 1) return false;    // lone cell: nothing left to sever
        lastDamagePosition = worldPosition;  // outward burst origin for the split
        const int hitGroup = chunks[hitChunk].plankGroup;
        // Corrugated sheet still on the roof: cut only the bonds leaving its
        // group so the whole sheet tears off in one piece (cells stay bonded).
        // Once the sheet has fallen, its actor holds just that group and later
        // hits drop through to per-cell fracture below.
        if (chunks[hitChunk].sheet && hitGroup >= 0) {
            bool attachedToOthers = false;
            for (uint32_t ci : hitActor->chunks)
                if (chunks[ci].plankGroup != hitGroup) { attachedToOthers = true; break; }
            if (attachedToOthers) {
                IsolateGroupParams groupParams{ chunkGroupByAsset.data(),
                    (uint32_t)chunkGroupByAsset.size(), hitGroup };
                const NvBlastDamageProgram isolateGroup = { IsolateGroupShader, nullptr };
                hitActor->actor->damage(isolateGroup, &groupParams);
                group->process();
                return true;
            }
        }
        std::vector<uint8_t> mask(chunks.size() + 1, 0);  // asset chunk index; 0 = root
        if (chunks[hitChunk].glass) {
            // Glass breaks like glass: one hit severs every cell of the pane so
            // the whole window bursts into individual shards at once.
            const int pane = chunks[hitChunk].plankGroup;
            for (uint32_t ci : hitActor->chunks)
                if (chunks[ci].plankGroup == pane) mask[ci + 1] = 1;
        } else {
            // Wood: break just the struck Voronoi cell -- a local jagged hole
            // rather than a whole straight-edged board.
            mask[hitChunk + 1] = 1;
        }
        IsolateChunksParams isolateParams{ mask.data(), (uint32_t)mask.size() };
        const NvBlastDamageProgram isolate = { IsolateGraphShader, nullptr };
        hitActor->actor->damage(isolate, &isolateParams);
        group->process();
        return true;
    }

    // Any non-support chunk left hanging by fewer than kMinBonds live bonds is
    // cut loose so it falls, matching real structures where a piece needs at
    // least a couple of solid connections to stay attached. Cascades: dropping
    // one piece can leave a neighbour under-connected, so repeat until stable.
    void DropUnderConnectedChunks() {
        // Drop a piece only once it has NO live bonds left. Using a higher
        // threshold cascades: isolating the struck chunk drops its neighbours to
        // one bond, which would then fall too, chaining across the whole wall.
        constexpr uint32_t kMinBonds = 1;
        if (!asset) return;
        const uint32_t assetBondCount = asset->getBondCount();

        for (int pass = 0; pass < 8; ++pass) {
            bool anyMarked = false;
            // damage() defers the fracture until group->process(), so the per-
            // actor break mask and params must outlive this loop. Keep them in
            // stable storage (deque never reallocates its elements).
            std::list<std::vector<uint8_t>> masks;
            std::list<IsolateChunksParams> paramStore;
            for (auto& runtime : actors) {
                if (!runtime->actor) continue;
                const NvBlastActor* ll = runtime->actor->getActorLL();
                if (!ll) continue;
                const float* bondHealths = NvBlastActorGetBondHealths(ll, nullptr);
                if (!bondHealths) continue;

                // Count this actor's live bonds per chunk.
                std::unordered_map<uint32_t, uint32_t> liveBonds;  // chunkIndex(0-based) -> count
                for (uint32_t bp = 0; bp < bondPairs.size() && bp < assetBondCount; ++bp) {
                    if (bondHealths[bp] <= 0.0f) continue;
                    const uint32_t ca = bondPairs[bp].a, cb = bondPairs[bp].b;
                    const bool ownsA = std::find(runtime->chunks.begin(), runtime->chunks.end(), ca) != runtime->chunks.end();
                    const bool ownsB = std::find(runtime->chunks.begin(), runtime->chunks.end(), cb) != runtime->chunks.end();
                    if (ownsA && ownsB) { ++liveBonds[ca]; ++liveBonds[cb]; }
                }

                std::vector<uint8_t> mask(chunks.size() + 1, 0);  // asset chunk index; 0 = root
                bool actorMarked = false;
                for (uint32_t chunkIndex : runtime->chunks) {
                    if (chunks[chunkIndex].support) continue;         // anchored: never auto-drop
                    if (chunks[chunkIndex].plankGroup >= 0) continue;  // plank sub-pieces break only on a hit, not by the low-bond rule
                    if (runtime->chunks.size() <= 1) continue;         // already a loose single piece
                    if (liveBonds[chunkIndex] < kMinBonds) {
                        mask[chunkIndex + 1] = 1;                       // asset chunk index = 0-based + 1
                        actorMarked = true;
                    }
                }
                if (actorMarked) {
                    masks.push_back(std::move(mask));
                    paramStore.push_back({ masks.back().data(), (uint32_t)masks.back().size() });
                    const NvBlastDamageProgram program = { IsolateGraphShader, nullptr };
                    runtime->actor->damage(program, &paramStore.back());
                    anyMarked = true;
                }
            }
            if (!anyMarked) break;
            group->process();  // splits off the newly isolated chunks
        }
    }
};

DestructionDX12::DestructionDX12() : m(std::make_unique<Impl>()) { m->owner = this; }
DestructionDX12::~DestructionDX12() { Shutdown(); }

bool DestructionDX12::Initialize(const std::shared_ptr<SceneNode>& mergedModel,
                                 ID3D12Device* device, int gridX, int gridY, int gridZ) {
    Shutdown();
    m = std::make_unique<Impl>(); m->owner = this;
    m->source = mergedModel; m->device = device;
    m->gridX = std::max(1, gridX); m->gridY = std::max(1, gridY); m->gridZ = std::max(1, gridZ);
    if (!m->BuildChunks() || !m->BuildBlast() || !m->BuildPhysics()) {
        std::cerr << "Destruction initialization failed\n";
        Shutdown(); return false;
    }
    m->initialized = true;
    m->RebuildRenderItems();
    std::cout << "Destruction ready: " << m->chunks.size() << " chunks, "
              << m->asset->getBondCount() << " bonds\n";
    return true;
}

void DestructionDX12::Shutdown() {
    if (!m) return;
    if (!B3_IS_NULL(m->world)) b3DestroyWorld(m->world);
    m->world = b3_nullWorldId;
    if (m->family) {
        m->family->removeListener(*this);
        const uint32_t count = m->family->getActorCount();
        std::vector<TkActor*> familyActors(count);
        m->family->getActors(familyActors.data(), count);
        for (TkActor* actor : familyActors) actor->removeFromGroup();
        m->family->release();
    }
    m->family = nullptr; m->actors.clear();
    if (m->group) m->group->release();
    if (m->asset) m->asset->release();
    if (m->framework) ReleaseTkFramework();
    m->group = nullptr; m->asset = nullptr; m->framework = nullptr;
    m->chunks.clear(); m->renderItems.clear(); m->ragdollParts.clear();
    m->ragdollRenderItems.clear(); m->initialized = false;
}

void DestructionDX12::Reset() {
    if (!m || !m->source) return;
    auto source = m->source; ID3D12Device* device = m->device;
    const int x = m->gridX, y = m->gridY, z = m->gridZ;
    Initialize(source, device, x, y, z);
}

void DestructionDX12::Update(float dt) {
    if (!m->initialized) return;
    m->accumulator = std::min(0.25f, m->accumulator + dt);
    constexpr float step = 1.0f / 60.0f;
    bool anyImpactBroke = false;
    while (m->accumulator >= step) {
        m->ApplyWaterBuoyancy();
        b3World_Step(m->world, step, 4);
        m->accumulator -= step;
        // Physics impact damage: collisions above the world's hit-event speed
        // threshold (debris slamming the house, pieces crashing onto the
        // ground) fracture the cell they land on, same as a bullet strike.
        const b3ContactEvents events = b3World_GetContactEvents(m->world);
        int budget = 2;  // low impact damage: at most two cells per step
        for (int i = 0; i < events.hitCount && budget > 0; ++i) {
            const b3ContactHitEvent& hit = events.hitEvents[i];
            const XMFLOAT3 point((float)hit.point.x, (float)hit.point.y, (float)hit.point.z);
            if (m->BreakNearestCell(point)) { --budget; anyImpactBroke = true; }
        }
    }
    if (anyImpactBroke) m->DropUnderConnectedChunks();
    m->RebuildRenderItems();
}

bool DestructionDX12::HitTest(const XMFLOAT3& worldPosition, float radius, XMFLOAT3& hitPosition) const {
    if (!m->initialized) return false;
    for (const auto& runtime : m->actors) {
        const b3Vec3 local = b3Body_GetLocalPoint(runtime->body,
            { worldPosition.x, worldPosition.y, worldPosition.z });
        const XMFLOAT3 modelPoint(local.x + runtime->center.x,
                                  local.y + runtime->center.y,
                                  local.z + runtime->center.z);
        for (uint32_t index : runtime->chunks) {
            if (SphereAabb(modelPoint, radius, m->chunks[index].minimum, m->chunks[index].maximum)) {
                hitPosition = worldPosition; return true;
            }
        }
    }
    return false;
}

bool DestructionDX12::HitTestSegment(const XMFLOAT3& worldStart, const XMFLOAT3& worldEnd,
                                     float radius, XMFLOAT3& hitPosition) const {
    if (!m->initialized) return false;
    m->lastRagdollHit = -1;
    float closest = FLT_MAX;
    bool hit = false;
    for (const auto& runtime : m->actors) {
        const b3Vec3 localStart = b3Body_GetLocalPoint(runtime->body,
            { worldStart.x, worldStart.y, worldStart.z });
        const b3Vec3 localEnd = b3Body_GetLocalPoint(runtime->body,
            { worldEnd.x, worldEnd.y, worldEnd.z });
        const XMFLOAT3 modelStart(localStart.x + runtime->center.x,
                                  localStart.y + runtime->center.y,
                                  localStart.z + runtime->center.z);
        const XMFLOAT3 modelEnd(localEnd.x + runtime->center.x,
                                localEnd.y + runtime->center.y,
                                localEnd.z + runtime->center.z);
        for (uint32_t index : runtime->chunks) {
            float t = 0.0f;
            if (SegmentAabb(modelStart, modelEnd, radius,
                            m->chunks[index].minimum, m->chunks[index].maximum, t) && t < closest) {
                closest = t; hit = true;
            }
        }
    }
    for (size_t i = 0; i < m->ragdollParts.size(); ++i) {
        const Impl::RagdollPart& part = m->ragdollParts[i];
        const b3Vec3 a = b3Body_GetLocalPoint(part.body,
            { worldStart.x, worldStart.y, worldStart.z });
        const b3Vec3 b = b3Body_GetLocalPoint(part.body,
            { worldEnd.x, worldEnd.y, worldEnd.z });
        float t = 0.0f;
        if (SegmentAabb({ a.x, a.y, a.z }, { b.x, b.y, b.z }, radius,
                        { -part.half.x, -part.half.y, -part.half.z },
                        {  part.half.x,  part.half.y,  part.half.z }, t) && t < closest) {
            closest = t; hit = true; m->lastRagdollHit = (int)i;
        }
    }
    if (hit) {
        hitPosition = { worldStart.x + (worldEnd.x - worldStart.x) * closest,
                        worldStart.y + (worldEnd.y - worldStart.y) * closest,
                        worldStart.z + (worldEnd.z - worldStart.z) * closest };
    }
    return hit;
}

void DestructionDX12::ApplyRadialDamage(const XMFLOAT3& worldPosition, float radius, float damage) {
    if (!m->initialized) return;
    // Bullet struck a person, not a Blast chunk. Preserve building bonds.
    if (m->lastRagdollHit >= 0) return;
    m->lastDamagePosition = worldPosition;
    m->lastDamageRadius = radius;
    // Break only the single piece that was hit: sever the nearest cell's bonds
    // so it splits off and falls. Neighbours stay untouched -- no radial spread.
    (void)damage;  // no radial falloff damage; each hit frees exactly one piece
    const uint32_t actorsBefore = (uint32_t)m->actors.size();
    m->BreakNearestCell(worldPosition);
    m->DropUnderConnectedChunks();
    m->RebuildRenderItems();
    std::cout << "Blast hit: actors " << actorsBefore << " -> " << m->actors.size() << "\n";
}

void DestructionDX12::ApplyExplosion(const XMFLOAT3& worldPosition, float radius,
                                     float damage, float impulse) {
    if (!m->initialized) return;
    (void)damage;  // explosion fully severs pieces in range rather than chipping
    m->lastDamagePosition = worldPosition;
    m->lastDamageRadius = radius;
    const float radiusSquared = radius * radius;

    // Mark every chunk whose centre lies within the blast radius (in that
    // actor's model space) and sever all its bonds, so the whole sphere of the
    // building around the blast breaks apart at once.
    std::list<std::vector<uint8_t>> masks;
    std::list<IsolateChunksParams> paramStore;
    const NvBlastDamageProgram isolate = { IsolateGraphShader, nullptr };
    for (auto& runtime : m->actors) {
        if (!runtime->actor) continue;
        const b3Vec3 local = b3Body_GetLocalPoint(runtime->body,
            { worldPosition.x, worldPosition.y, worldPosition.z });
        const XMFLOAT3 modelHit(local.x + runtime->center.x,
                                local.y + runtime->center.y,
                                local.z + runtime->center.z);
        std::vector<uint8_t> mask(m->chunks.size() + 1, 0);  // asset chunk index; 0 = root
        bool marked = false;
        for (uint32_t chunkIndex : runtime->chunks) {
            const Impl::Chunk& chunk = m->chunks[chunkIndex];
            if (chunk.support) continue;  // anchored pieces resist the blast
            const float dx = chunk.center.x - modelHit.x;
            const float dy = chunk.center.y - modelHit.y;
            const float dz = chunk.center.z - modelHit.z;
            if (dx * dx + dy * dy + dz * dz <= radiusSquared) {
                mask[chunkIndex + 1] = 1;
                marked = true;
            }
        }
        if (marked) {
            masks.push_back(std::move(mask));
            paramStore.push_back({ masks.back().data(), (uint32_t)masks.back().size() });
            runtime->actor->damage(isolate, &paramStore.back());
        }
    }
    const uint32_t actorsBefore = (uint32_t)m->actors.size();
    m->group->process();
    m->DropUnderConnectedChunks();
    m->RebuildRenderItems();

    // Blow the freed fragments outward from the blast centre, falling off with
    // distance and with an upward bias so debris lifts.
    const XMVECTOR center = XMLoadFloat3(&worldPosition);
    for (auto& runtime : m->actors) {
        if (!runtime->dynamic || B3_IS_NULL(runtime->body)) continue;
        const b3Vec3 localBlast = b3Body_GetLocalPoint(runtime->body,
            { worldPosition.x, worldPosition.y, worldPosition.z });
        const XMFLOAT3 modelBlast(localBlast.x + runtime->center.x,
                                  localBlast.y + runtime->center.y,
                                  localBlast.z + runtime->center.z);
        bool blastOverlapsChunk = false;
        for (uint32_t chunkIndex : runtime->chunks) {
            const Impl::Chunk& chunk = m->chunks[chunkIndex];
            if (SphereAabb(modelBlast, radius, chunk.minimum, chunk.maximum)) {
                blastOverlapsChunk = true; break;
            }
        }
        const b3Pos bp = b3Body_GetPosition(runtime->body);
        const XMVECTOR pos = XMVectorSet((float)bp.x, (float)bp.y, (float)bp.z, 0.0f);
        const float dist = XMVectorGetX(XMVector3Length(pos - center));
        if (dist > radius && !blastOverlapsChunk) continue;
        XMVECTOR dir = pos - center;
        if (dist < 0.001f) dir = XMVectorSet(0, 1, 0, 0);  // at the centre: straight up
        dir = XMVector3Normalize(dir + XMVectorSet(0, 0.4f, 0, 0));
        const float scale = blastOverlapsChunk
            ? std::max(0.35f, 1.0f - dist / radius)
            : std::max(0.2f, 1.0f - dist / radius);
        // Cap the velocity change so feather-light shards don't launch.
        constexpr float kMaxDeltaV = 14.0f;  // m/s
        const float mass = std::max(0.05f, b3Body_GetMass(runtime->body));
        const float magnitude = std::min(impulse * scale, kMaxDeltaV * mass);
        XMFLOAT3 v; XMStoreFloat3(&v, dir * magnitude);
        b3Body_ApplyLinearImpulse(runtime->body, { v.x, v.y, v.z }, { (float)bp.x, (float)bp.y, (float)bp.z }, true);
    }
    // Blast pressure acts on every ragdoll limb. Applying at each limb center
    // moves the whole articulated body while also producing natural rotation.
    for (Impl::RagdollPart& part : m->ragdollParts) {
        const b3Pos bp = b3Body_GetPosition(part.body);
        const XMVECTOR pos = XMVectorSet((float)bp.x, (float)bp.y, (float)bp.z, 0.0f);
        const float dist = XMVectorGetX(XMVector3Length(pos - center));
        if (dist > radius) continue;
        XMVECTOR dir = pos - center;
        if (dist < 0.001f) dir = XMVectorSet(0, 1, 0, 0);
        dir = XMVector3Normalize(dir + XMVectorSet(0, 0.55f, 0, 0));
        const float scale = std::max(0.15f, 1.0f - dist / radius);
        const float mass = std::max(0.05f, b3Body_GetMass(part.body));
        const float magnitude = std::min(impulse * scale, 18.0f * mass);
        XMFLOAT3 v; XMStoreFloat3(&v, dir * magnitude);
        b3Body_ApplyLinearImpulse(part.body, { v.x, v.y, v.z },
            { (float)bp.x, (float)bp.y, (float)bp.z }, true);
    }
    std::cout << "Grenade: actors " << actorsBefore << " -> " << m->actors.size() << "\n";
}

bool DestructionDX12::ApplyImpulse(const XMFLOAT3& worldPosition,
                                   const XMFLOAT3& worldDirection,
                                   float impulseStrength, float hitRadius) {
    if (!m->initialized || impulseStrength <= 0.0f) return false;
    bool applied = false;
    XMVECTOR direction = XMVector3Normalize(XMLoadFloat3(&worldDirection));
    const XMVECTOR hitPoint = XMLoadFloat3(&worldPosition);
    // Only shove fragments the bullet actually reaches, and fall off with
    // distance so a hit stays local instead of nudging every loose piece.
    const float falloffRadius = std::max(0.25f, hitRadius);
    const float falloffSquared = falloffRadius * falloffRadius;
    if (m->lastRagdollHit >= 0 && m->lastRagdollHit < (int)m->ragdollParts.size()) {
        Impl::RagdollPart& part = m->ragdollParts[(size_t)m->lastRagdollHit];
        const float mass = std::max(0.05f, b3Body_GetMass(part.body));
        const float magnitude = std::min(impulseStrength, 12.0f * mass);
        XMFLOAT3 impulse;
        XMStoreFloat3(&impulse, direction * magnitude);
        b3Body_ApplyLinearImpulse(part.body, { impulse.x, impulse.y, impulse.z },
            { worldPosition.x, worldPosition.y, worldPosition.z }, true);
        m->lastRagdollHit = -1;
        applied = true;
    }
    for (auto& runtime : m->actors) {
        if (!runtime->dynamic || B3_IS_NULL(runtime->body)) continue;
        const b3Pos bodyPosition = b3Body_GetPosition(runtime->body);
        const XMVECTOR bodyCenter =
            XMVectorSet((float)bodyPosition.x, (float)bodyPosition.y, (float)bodyPosition.z, 0.0f);
        const float distanceSquared = XMVectorGetX(XMVector3LengthSq(bodyCenter - hitPoint));
        if (distanceSquared > falloffSquared) continue;
        const float scale = 1.0f - std::sqrt(distanceSquared) / falloffRadius;
        // Cap the resulting velocity change: a fixed impulse on a feather-light
        // fragment (a glass shard) would otherwise launch it at bullet speed.
        constexpr float kMaxDeltaV = 9.0f;  // m/s
        const float mass = std::max(0.05f, b3Body_GetMass(runtime->body));
        const float magnitude = std::min(impulseStrength * std::max(0.15f, scale),
                                         kMaxDeltaV * mass);
        XMFLOAT3 impulse;
        XMStoreFloat3(&impulse, direction * magnitude);
        b3Body_ApplyLinearImpulse(runtime->body, { impulse.x, impulse.y, impulse.z },
            { worldPosition.x, worldPosition.y, worldPosition.z }, true);
        applied = true;
    }
    return applied;
}

void DestructionDX12::SetWaterRegion(const XMFLOAT3& minCorner, const XMFLOAT3& maxCorner) {
    if (!m) return;
    m->waterEnabled = true;
    m->waterMin = minCorner;
    m->waterMax = maxCorner;
    m->waterSurfaceY = maxCorner.y;
}

void DestructionDX12::SetTerrainSampler(std::function<float(float, float)> sampler) {
    if (!m) return;
    m->terrainSampler = std::move(sampler);
    m->BuildGround();   // rebuild the ground collider as a terrain heightfield
}

void DestructionDX12::SetSplashCallback(std::function<void(float, float, float)> cb) {
    if (!m) return;
    m->splashCallback = std::move(cb);
}

std::vector<XMFLOAT3> DestructionDX12::DrainBreakPoints() {
    if (!m) return {};
    std::vector<XMFLOAT3> out = std::move(m->breakPoints);
    m->breakPoints.clear();
    return out;
}

void DestructionDX12::ResolvePlayerCollision(XMFLOAT3& eyePosition, float& floorY,
                                             float radius, float height) {
    if (!m->initialized) return;
    const float feet = eyePosition.y - height;
    constexpr float kStepHeight = 1.0f / 3.0f;   // max ledge the player can step up onto
    auto resolveBox = [&](const XMFLOAT3& lo, const XMFLOAT3& hi, b3BodyId body, bool dynamic) {
        if (eyePosition.y < lo.y || feet > hi.y) return;
        const float nearestX = (std::max)(lo.x, (std::min)(eyePosition.x, hi.x));
        const float nearestZ = (std::max)(lo.z, (std::min)(eyePosition.z, hi.z));
        float dx = eyePosition.x - nearestX, dz = eyePosition.z - nearestZ;
        float distance = std::sqrt(dx * dx + dz * dz);
        if (distance >= radius) return;
        // Step-up: the box top is at most one step above the feet and the player
        // is horizontally over it -> stand on top instead of being shoved aside.
        // (distance ~0 means the eye column is inside the box footprint.)
        if (hi.y <= feet + kStepHeight && distance < radius) {
            floorY = (std::max)(floorY, hi.y);
            return;
        }
        if (distance < 0.0001f) {
            const float left = std::abs(eyePosition.x - lo.x);
            const float right = std::abs(hi.x - eyePosition.x);
            const float back = std::abs(eyePosition.z - lo.z);
            const float front = std::abs(hi.z - eyePosition.z);
            const float smallest = (std::min)((std::min)(left, right), (std::min)(back, front));
            if (smallest == left) { dx = -1; dz = 0; }
            else if (smallest == right) { dx = 1; dz = 0; }
            else if (smallest == back) { dx = 0; dz = -1; }
            else { dx = 0; dz = 1; }
            distance = 0.0f;
        } else { dx /= distance; dz /= distance; }
        const float push = radius - distance + 0.002f;
        eyePosition.x += dx * push; eyePosition.z += dz * push;
        if (dynamic && !B3_IS_NULL(body)) {
            const float mass = (std::max)(0.05f, b3Body_GetMass(body));
            const float magnitude = (std::min)(mass * 2.5f, 35.0f);
            const b3Pos p = b3Body_GetPosition(body);
            b3Body_ApplyLinearImpulse(body, { -dx * magnitude, 0.15f * magnitude, -dz * magnitude }, p, true);
        }
    };

    for (const auto& runtime : m->actors) {
        if (B3_IS_NULL(runtime->body)) continue;
        const XMMATRIX transform = BoxTransform(runtime->body, runtime->center);
        for (uint32_t index : runtime->chunks) {
            const Impl::Chunk& chunk = m->chunks[index];
            XMFLOAT3 lo(FLT_MAX,FLT_MAX,FLT_MAX), hi(-FLT_MAX,-FLT_MAX,-FLT_MAX);
            for (float x : {chunk.minimum.x,chunk.maximum.x})
            for (float y : {chunk.minimum.y,chunk.maximum.y})
            for (float z : {chunk.minimum.z,chunk.maximum.z}) {
                XMFLOAT3 p; XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(x,y,z,1), transform));
                lo.x=(std::min)(lo.x,p.x);lo.y=(std::min)(lo.y,p.y);lo.z=(std::min)(lo.z,p.z);
                hi.x=(std::max)(hi.x,p.x);hi.y=(std::max)(hi.y,p.y);hi.z=(std::max)(hi.z,p.z);
            }
            resolveBox(lo, hi, runtime->body, runtime->dynamic);
        }
    }
    for (const Impl::RagdollPart& part : m->ragdollParts) {
        const b3Pos p = b3Body_GetPosition(part.body);
        resolveBox({(float)p.x-part.half.x,(float)p.y-part.half.y,(float)p.z-part.half.z},
                   {(float)p.x+part.half.x,(float)p.y+part.half.y,(float)p.z+part.half.z},
                   part.body, true);
    }
}

void DestructionDX12::receive(const TkEvent* events, uint32_t eventCount) {
    for (uint32_t e = 0; e < eventCount; ++e) {
        if (events[e].type != TkEvent::Split) continue;
        const TkSplitEvent* split = events[e].getPayload<TkSplitEvent>();
        auto* parent = static_cast<Impl::ActorRuntime*>(split->parentData.userData);
        Impl::BodySeed seed;
        if (parent && !B3_IS_NULL(parent->body)) {
            seed.valid = true;
            seed.modelCenter = parent->center;
            seed.position = b3Body_GetPosition(parent->body);
            seed.rotation = b3Body_GetRotation(parent->body);
            seed.linearVelocity = b3Body_GetLinearVelocity(parent->body);
            seed.angularVelocity = b3Body_GetAngularVelocity(parent->body);
        }
        for (auto it = m->actors.begin(); it != m->actors.end(); ++it) {
            if (it->get() != parent) continue;
            if (!B3_IS_NULL((*it)->body)) b3DestroyBody((*it)->body);
            m->actors.erase(it); break;
        }
        for (uint32_t childIndex = 0; childIndex < split->numChildren; ++childIndex) {
            TkActor* child = split->children[childIndex];
            auto runtime = std::make_unique<Impl::ActorRuntime>();
            runtime->actor = child;
            const uint32_t visibleCount = child->getVisibleChunkCount();
            std::vector<uint32_t> visible(visibleCount);
            child->getVisibleChunkIndices(visible.data(), visibleCount);
            for (uint32_t assetChunk : visible) {
                if (assetChunk > 0 && assetChunk <= m->chunks.size())
                    runtime->chunks.push_back(assetChunk - 1);
            }
            if (runtime->chunks.empty()) continue;
            child->userData = runtime.get();
            // An island still containing an anchored support chunk is part of
            // the standing structure -> keep it static. Islands with no support
            // have been structurally freed -> simulate them dynamically.
            bool islandSupported = false;
            for (uint32_t chunkIndex : runtime->chunks) {
                if (m->chunks[chunkIndex].support) { islandSupported = true; break; }
            }
            m->CreateBody(*runtime, !islandSupported, islandSupported ? nullptr : &seed);
            // A freed (unsupported) island is a real break -> mark its spot for
            // a puff of smoke at the fracture.
            if (!islandSupported && !B3_IS_NULL(runtime->body)) {
                const b3Pos bp = b3Body_GetPosition(runtime->body);
                m->breakPoints.push_back({ (float)bp.x, (float)bp.y, (float)bp.z });
            }
            m->actors.push_back(std::move(runtime));
        }
    }
}

bool DestructionDX12::IsInitialized() const { return m && m->initialized; }
uint32_t DestructionDX12::GetChunkCount() const { return m ? (uint32_t)m->chunks.size() : 0; }
uint32_t DestructionDX12::GetActorCount() const { return m ? (uint32_t)m->actors.size() : 0; }
const std::vector<DestructionRenderItem>& DestructionDX12::GetRenderItems() const { return m->renderItems; }
const std::vector<RagdollRenderItem>& DestructionDX12::GetRagdollRenderItems() const { return m->ragdollRenderItems; }

DestructionDebugData DestructionDX12::GetDebugData() const {
    DestructionDebugData data;
    if (!m || !m->initialized) return data;
    data.hasHit = m->lastDamagePosition.x != 0.0f || m->lastDamagePosition.y != 0.0f ||
                  m->lastDamagePosition.z != 0.0f;
    data.lastHit = m->lastDamagePosition;
    data.hitRadius = m->lastDamageRadius;

    // Map each chunk to its owning actor so bonds can be flagged severed when
    // their two chunks now live in different actors (islands).
    std::unordered_map<uint32_t, const Impl::ActorRuntime*> chunkOwner;
    data.chunks.resize(m->chunks.size());
    for (const auto& runtime : m->actors) {
        ++data.actorCount;
        if (runtime->dynamic) ++data.dynamicActorCount;
        XMMATRIX transform = XMMatrixIdentity();
        if (!B3_IS_NULL(runtime->body)) transform = BoxTransform(runtime->body, runtime->center);
        for (uint32_t chunkIndex : runtime->chunks) {
            chunkOwner[chunkIndex] = runtime.get();
            const Impl::Chunk& chunk = m->chunks[chunkIndex];
            const XMVECTOR modelCenter = XMLoadFloat3(&chunk.center);
            XMFLOAT3 worldCenter; XMStoreFloat3(&worldCenter, XMVector3Transform(modelCenter, transform));
            const XMFLOAT3 half((chunk.maximum.x - chunk.minimum.x) * 0.5f,
                                (chunk.maximum.y - chunk.minimum.y) * 0.5f,
                                (chunk.maximum.z - chunk.minimum.z) * 0.5f);
            DestructionDebugChunk& out = data.chunks[chunkIndex];
            out.worldCenter = worldCenter;
            out.worldMin = { worldCenter.x - half.x, worldCenter.y - half.y, worldCenter.z - half.z };
            out.worldMax = { worldCenter.x + half.x, worldCenter.y + half.y, worldCenter.z + half.z };
            out.support = chunk.support;
            out.dynamic = runtime->dynamic;
        }
    }

    // Live bond healths are shared family-wide, indexed by asset bond index,
    // which matches the order we created bondPairs. Read them from any actor.
    const float* bondHealths = nullptr;
    uint32_t bondHealthCount = 0;
    if (!m->actors.empty() && m->actors.front()->actor) {
        const NvBlastActor* ll = m->actors.front()->actor->getActorLL();
        if (ll) {
            bondHealths = NvBlastActorGetBondHealths(ll, nullptr);
            bondHealthCount = m->asset ? m->asset->getBondCount() : 0;
        }
    }

    data.bonds.reserve(m->bondPairs.size());
    for (uint32_t i = 0; i < m->bondPairs.size(); ++i) {
        const Impl::BondPair& pair = m->bondPairs[i];
        if (pair.a >= data.chunks.size() || pair.b >= data.chunks.size()) continue;
        auto ownerA = chunkOwner.find(pair.a);
        auto ownerB = chunkOwner.find(pair.b);
        DestructionDebugBond bond;
        bond.a = data.chunks[pair.a].worldCenter;
        bond.b = data.chunks[pair.b].worldCenter;
        // Severed if either chunk is gone or the two now belong to different actors.
        bond.broken = ownerA == chunkOwner.end() || ownerB == chunkOwner.end() ||
                      ownerA->second != ownerB->second;
        if (bondHealths && i < bondHealthCount) {
            bond.health = std::max(0.0f, bondHealths[i]);
            bond.healthFraction = std::min(1.0f, bond.health / kBondHealth);
            if (bond.health <= 0.0f) bond.broken = true;
        }
        data.bonds.push_back(bond);
    }
    return data;
}
