#define NOMINMAX
#include "DestructionDX12.h"

#include "GLBImporter.h"
#include "DX12Core.h"
#include "NvBlast.h"
#include "NvBlastTkActor.h"
#include "NvBlastTkAsset.h"
#include "NvBlastTkFamily.h"
#include "NvBlastTkFramework.h"
#include "NvBlastTkGroup.h"
#include "NvBlastTypes.h"
#include "PhysicsImpactPolicy.h"
#include <box3d/box3d.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace DirectX;
using namespace Nv::Blast;

namespace {
constexpr uint32_t InvalidIndex = 0xFFFFFFFFu;
constexpr uint32_t MaxAwakeDebrisBodies = 256;
constexpr float StructuralSolverStep = 1.0f / 15.0f;
constexpr double StructuralSolverBudgetMs = 0.35;
constexpr uint32_t MaxStructuresPerSolverSlice = 2;
constexpr float DebrisCollisionLodAge = 3.0f;
constexpr float DebrisCollisionLodVolume = 0.035f;
constexpr float TinyDebrisMaxExtent = 0.20f;
constexpr float SettledPileFreezeSeconds = 3.0f;
constexpr uint64_t CollisionCategoryWorld = PhysicsImpactPolicy::World;
constexpr uint64_t CollisionCategoryDebris = PhysicsImpactPolicy::Debris;
constexpr uint64_t CollisionCategoryLodDebris = PhysicsImpactPolicy::LodDebris;
constexpr uint64_t CollisionCategoryBarrel = PhysicsImpactPolicy::Barrel;
constexpr uint64_t CollisionCategoryRagdoll = PhysicsImpactPolicy::Ragdoll;
constexpr uint64_t CollisionCategoryVehicle = PhysicsImpactPolicy::Vehicle;
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

b3Quat ToB3Quat(const XMFLOAT4& q) {
    return b3NormalizeQuat({ { q.x, q.y, q.z }, q.w });
}

b3Quat RagdollFrameRotation(const RagdollJointFrame& frame,
                            RagdollJointType type, uint8_t hingeAxis) {
    XMVECTOR primary = XMVector3Normalize(XMLoadFloat3(&frame.primary));
    if (XMVectorGetX(XMVector3LengthSq(primary)) < 1e-5f)
        primary = XMVectorSet(1,0,0,0);
    XMVECTOR secondary = XMLoadFloat3(&frame.secondary);
    secondary -= primary * XMVectorGetX(XMVector3Dot(primary, secondary));
    if (XMVectorGetX(XMVector3LengthSq(secondary)) < 1e-5f)
        secondary = XMVectorSet(0,1,0,0);
    secondary = XMVector3Normalize(secondary);
    XMVECTOR tertiary = XMVector3Normalize(XMVector3Cross(primary, secondary));

    XMVECTOR x = secondary, y = tertiary, z = primary;
    if (type == RagdollJointType::Hinge && hingeAxis == 1) {
        x = tertiary; y = primary; z = secondary;
    } else if (type == RagdollJointType::Hinge && hingeAxis == 2) {
        x = primary; y = secondary; z = tertiary;
    }
    const XMMATRIX basis(x, y, z, XMVectorSet(0,0,0,1));
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionRotationMatrix(basis)));
    return ToB3Quat(q);
}

XMFLOAT3 RagdollBodyHalfExtent(const AuthoredRagdollBody& body) {
    XMFLOAT3 half = { 0.05f, 0.05f, 0.05f };
    for (const RagdollShapeSpec& shape : body.shapes) {
        const float bound = shape.type == RagdollShapeType::Capsule
            ? shape.radius + shape.length * 0.5f
            : shape.type == RagdollShapeType::Sphere
                ? shape.radius
                : std::sqrt(shape.halfExtent.x * shape.halfExtent.x +
                            shape.halfExtent.y * shape.halfExtent.y +
                            shape.halfExtent.z * shape.halfExtent.z);
        half.x = std::max(half.x, std::abs(shape.center.x) + bound);
        half.y = std::max(half.y, std::abs(shape.center.y) + bound);
        half.z = std::max(half.z, std::abs(shape.center.z) + bound);
    }
    return half;
}

float PassiveJointTorque(const std::string& bone) {
    if (bone.find("spine") != std::string::npos ||
        bone.find("head") != std::string::npos) return 10.0f;
    if (bone.find("thigh") != std::string::npos ||
        bone.find("upperarm") != std::string::npos) return 7.0f;
    if (bone.find("calf") != std::string::npos ||
        bone.find("lowerarm") != std::string::npos) return 4.0f;
    return 2.0f;
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
        uint32_t structureId = 0;
    };

    struct ActorRuntime {
        TkActor* actor = nullptr;
        std::vector<uint32_t> chunks;
        b3BodyId body = b3_nullBodyId;
        XMFLOAT3 center = {};
        bool dynamic = false;
        float restTime = 0.0f;
        bool wasSubmerged = false;   // for splash-on-entry detection
        float debrisAge = 0.0f;
        // Only unsupported, fully detached debris receives cleanup lifetime.
        bool debrisCleanupEligible = false;
        bool collisionLod = false;
        bool frozen = false;
        float settledTime = 0.0f;
        uint32_t structureId = 0;
        uint64_t renderId = 0;
        uint64_t failedBatchHash = 0;
        XMFLOAT3 batchCenter = {};
        float batchRadius = 0.0f;
        bool batchBoundsValid = false;
    };

    struct VortexRuntime {
        XMFLOAT3 center = {};
        float radius = 0.0f;
        float age = 0.0f;
        float duration = 3.0f;
        std::unordered_set<uint64_t> capturedActorIds;
    };

    struct BarrelRuntime {
        uint32_t handle = 0;
        b3BodyId body = b3_nullBodyId;
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
    std::vector<VortexRuntime> vortices;
    struct BurningChunk {
        uint32_t chunkIndex = InvalidIndex;
        float life = 3.0f;
        float spreadCooldown = 0.25f;
        float damageCooldown = 0.45f;
    };
    std::vector<BurningChunk> burningChunks;
    struct HarpoonRagdollPart {
        size_t partIndex = 0;
        b3Vec3 ragdollLocalAnchor = {};
        b3Vec3 ragdollLocalDirection = { 0.0f, 0.0f, 1.0f };
        b3BodyId anchorBody = b3_nullBodyId;
        b3JointId joint = b3_nullJointId;
        XMFLOAT3 desiredAnchor = {};
    };
    struct HarpoonRagdollAttachment {
        uint32_t harpoonId = 0;
        uint32_t ragdollId = InvalidIndex;
        float shaftOffset = 0.0f;
        std::vector<HarpoonRagdollPart> parts;
    };
    std::vector<HarpoonRagdollAttachment> harpoonRagdolls;
    struct PinnedHarpoonRagdoll {
        uint32_t harpoonId = 0;
        uint32_t chunkIndex = InvalidIndex;
        size_t partIndex = 0;
        b3Vec3 ragdollLocalAnchor = {};
        b3Vec3 ragdollLocalDirection = { 0.0f, 0.0f, 1.0f };
        b3Vec3 targetLocalAnchor = {};
        b3Vec3 targetLocalDirection = { 0.0f, 0.0f, 1.0f };
        b3BodyId staticAnchorBody = b3_nullBodyId;
        b3BodyId targetBody = b3_nullBodyId;
        bool targetFrameValid = false;
        b3JointId joint = b3_nullJointId;
    };
    std::vector<PinnedHarpoonRagdoll> pinnedHarpoonRagdolls;
    std::vector<BarrelRuntime> barrelBodies;
    std::vector<uint32_t> barrelImpactEvents;
    uint32_t nextBarrelHandle = 1;
    std::vector<DestructionRenderItem> renderItems;
    std::vector<DestructionRenderBatch> renderBatches;
    struct BatchCacheEntry {
        uint64_t chunkHash = 0;
        std::shared_ptr<SceneNode> colourNode;
        std::shared_ptr<SceneNode> shadowNode;
    };
    struct BatchBuildResult {
        uint64_t actorId = 0;
        uint64_t chunkHash = 0;
        std::shared_ptr<SceneNode> colourNode;
        std::shared_ptr<SceneNode> shadowNode;
    };
    struct SpatialCellKey {
        int x = 0, y = 0, z = 0;
        bool operator==(const SpatialCellKey& rhs) const {
            return x == rhs.x && y == rhs.y && z == rhs.z;
        }
    };
    struct SpatialCellKeyHash {
        size_t operator()(const SpatialCellKey& key) const {
            size_t h = static_cast<size_t>(key.x) * 73856093u;
            h ^= static_cast<size_t>(key.y) * 19349663u;
            h ^= static_cast<size_t>(key.z) * 83492791u;
            return h;
        }
    };
    struct SpatialBatchSource {
        std::shared_ptr<SceneNode> node;
        XMFLOAT4X4 transform = {};
        XMFLOAT3 center = {};
        float radius = 0.0f;
    };
    struct SpatialBatchBuild {
        uint64_t signature = 1469598103934665603ull;
        std::vector<SpatialBatchSource> sources;
        XMFLOAT3 minimum = { FLT_MAX, FLT_MAX, FLT_MAX };
        XMFLOAT3 maximum = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        uint32_t chunkCount = 0;
    };
    struct SpatialBatchCacheEntry {
        uint64_t signature = 0;
        std::shared_ptr<SceneNode> colourNode;
        std::shared_ptr<SceneNode> shadowNode;
        XMFLOAT3 center = {};
        float radius = 0.0f;
        uint32_t chunkCount = 0;
    };
    struct SpatialBatchBuildResult {
        SpatialCellKey key;
        uint64_t signature = 0;
        std::shared_ptr<SceneNode> colourNode;
        std::shared_ptr<SceneNode> shadowNode;
        XMFLOAT3 center = {};
        float radius = 0.0f;
        uint32_t chunkCount = 0;
    };
    std::unordered_map<const ActorRuntime*, BatchCacheEntry> batchCache;
    std::unordered_map<SpatialCellKey, SpatialBatchCacheEntry,
        SpatialCellKeyHash> spatialBatchCache;
    std::array<std::vector<std::shared_ptr<SceneNode>>, FRAME_COUNT> retiredBatchNodes;
    std::array<UINT64, FRAME_COUNT> retiredBatchEpoch = {};
    std::future<BatchBuildResult> batchBuildFuture;
    bool batchBuildInFlight = false;
    std::future<SpatialBatchBuildResult> spatialBatchBuildFuture;
    bool spatialBatchBuildInFlight = false;
    bool initialBatchBuild = true;
    uint64_t nextActorRenderId = 1;
    uint64_t renderItemRebuildCount = 0;
    uint64_t batchGeometryRebuildCount = 0;
    static constexpr float SpatialBatchCellSize = 4.0f;
    // True once the render items have been rebuilt at a moment when nothing was
    // moving -- i.e. they are up to date and can be left alone until something
    // changes. Cleared by anything that alters the scene (a break, a split, a new
    // ragdoll) so the next Update refreshes them. See DestructionDX12::Update.
    bool rebuiltWhileStill = false;
    struct RagdollPart {
        b3BodyId body = b3_nullBodyId;
        XMFLOAT3 half = {};
        XMFLOAT3 color = {};
        uint8_t shape = 1; // 1 capsule, 2 sphere
        bool wasSubmerged = false;   // for splash-on-entry detection
        uint32_t authoredId = InvalidIndex;
        std::string authoredBone;
        bool lethalHazard = false;
    };
    std::vector<RagdollPart> ragdollParts;
    std::vector<RagdollRenderItem> ragdollRenderItems;
    uint32_t nextAuthoredRagdollId = 0;
    struct AuthoredRagdollRuntime {
        uint32_t id = InvalidIndex;
        float muscleTime = 0.0f;
        struct MuscleJoint {
            b3JointId id = b3_nullJointId;
            RagdollJointType type = RagdollJointType::Spherical;
            float passiveTorque = 4.0f;
        };
        std::vector<MuscleJoint> joints;
        struct SoftAngularLimit {
            b3BodyId bodyA = b3_nullBodyId;
            b3BodyId bodyB = b3_nullBodyId;
            b3Quat localFrameA = b3Quat_identity;
            b3Quat localFrameB = b3Quat_identity;
            float swing1 = 0.0f;
            float swing2 = 0.0f;
        };
        std::vector<SoftAngularLimit> softLimits;
    };
    std::vector<AuthoredRagdollRuntime> authoredRagdolls;
    struct HoverEnemy {
        size_t firstPart = 0;
        b3BodyId torso = b3_nullBodyId;
        float totalMass = 1.0f;
        float hoverY = 4.5f;
        float phase = 0.0f;
        float fireCooldown = 0.5f;
        int shotsInBurst = 0;
        float health = 100.0f;
        float respawnTimer = 0.0f;
        bool alive = true;
        std::vector<XMFLOAT3> spawnPositions;
        XMFLOAT4 spawnRotation{ 0, 0, 0, 1 };
    };
    std::vector<HoverEnemy> hoverEnemies;
    std::vector<EnemyGunRenderItem> enemyGunRenderItems;
    std::vector<EnemyShot> pendingEnemyShots;
    XMFLOAT3 enemyTarget = {};
    bool enemyTargetValid = false;
    mutable int lastRagdollHit = -1;
    mutable uint32_t lastHitChunk = InvalidIndex;
    TkFramework* framework = nullptr;
    TkAsset* asset = nullptr;
    TkFamily* family = nullptr;
    TkGroup* group = nullptr;
    b3WorldId world = b3_nullWorldId;
    b3BodyId ground = b3_nullBodyId;
    b3HeightFieldData* terrainHeightField = nullptr;
    b3BodyId vehicleChassis = b3_nullBodyId;
    std::array<b3BodyId, 4> vehicleWheels = {
        b3_nullBodyId, b3_nullBodyId, b3_nullBodyId, b3_nullBodyId };
    std::array<b3JointId, 4> vehicleJoints = {
        b3_nullJointId, b3_nullJointId, b3_nullJointId, b3_nullJointId };
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
    std::vector<DestructionCollisionSoundEvent> collisionSoundEvents;
    std::vector<TinyDebrisParticle> tinyDebrisParticles;
    float accumulator = 0.0f;
    float maintenanceAccumulator = 0.0f;
    std::unordered_set<uint32_t> bulletWeakenedChunks;
    float structuralAccumulator = 0.0f;
    float structuralClock = 0.0f;
    struct DirtyStructure { uint32_t id = 0; float readyTime = 0.0f; };
    std::deque<DirtyStructure> dirtyStructures;
    std::unordered_set<uint32_t> dirtyStructureSet;
    uint32_t lastBrokenStructure = InvalidIndex;
    DestructionStressStats stressStats;
    double stressUpdateTotalMs = 0.0;
    uint64_t stressStartRebuilds = 0;
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

    // Brief pose matching preserves animation continuity. Stiffness fades instead
    // of switching off in one frame; a weak zero-speed motor then supplies the
    // passive resistance of tissue and clothing.
    void RelaxAuthoredRagdolls(float dt) {
        for (AuthoredRagdollRuntime& ragdoll : authoredRagdolls) {
            if (ragdoll.muscleTime <= 0.0f) continue;
            ragdoll.muscleTime = std::max(0.0f, ragdoll.muscleTime - dt);
            const float blend = ragdoll.muscleTime / 0.20f;
            for (const AuthoredRagdollRuntime::MuscleJoint& joint : ragdoll.joints) {
                if (B3_IS_NULL(joint.id) || !b3Joint_IsValid(joint.id)) continue;
                if (joint.type == RagdollJointType::Hinge) {
                    if (blend > 0.0f) {
                        b3RevoluteJoint_SetSpringHertz(joint.id, 1.0f + 3.0f * blend);
                    } else {
                        b3RevoluteJoint_EnableSpring(joint.id, false);
                        b3RevoluteJoint_EnableMotor(joint.id, true);
                        b3RevoluteJoint_SetMotorSpeed(joint.id, 0.0f);
                        b3RevoluteJoint_SetMaxMotorTorque(joint.id, joint.passiveTorque);
                    }
                } else {
                    if (blend > 0.0f) {
                        b3SphericalJoint_SetSpringHertz(joint.id, 1.0f + 3.0f * blend);
                    } else {
                        b3SphericalJoint_EnableSpring(joint.id, false);
                        b3SphericalJoint_EnableMotor(joint.id, true);
                        b3SphericalJoint_SetMotorVelocity(joint.id, { 0,0,0 });
                        b3SphericalJoint_SetMaxMotorTorque(joint.id, joint.passiveTorque);
                    }
                }
            }
        }
    }

    void ApplyAnatomicalResistance() {
        for (const AuthoredRagdollRuntime& ragdoll : authoredRagdolls) {
            for (const AuthoredRagdollRuntime::SoftAngularLimit& limit :
                 ragdoll.softLimits) {
                if (B3_IS_NULL(limit.bodyA) || B3_IS_NULL(limit.bodyB)) continue;
                const b3Quat frameA = b3MulQuat(
                    b3Body_GetRotation(limit.bodyA), limit.localFrameA);
                const b3Quat frameB = b3MulQuat(
                    b3Body_GetRotation(limit.bodyB), limit.localFrameB);
                // Joint frame B relative to A. Frame axes are arranged so local
                // x is Unreal swing-1 and local y is swing-2.
                const b3Quat relative = b3InvMulQuat(
                    frameA, frameB);
                b3Quat q = relative.s < 0.0f ? b3NegateQuat(relative) : relative;
                q = b3NormalizeQuat(q);
                const float angle = 2.0f * std::acos(std::clamp(q.s, -1.0f, 1.0f));
                const float sinHalf = std::sqrt(std::max(0.0f, 1.0f - q.s*q.s));
                b3Vec3 rotationVector = {};
                if (sinHalf > 1e-4f)
                    rotationVector = b3MulSV(angle / sinHalf, q.v);
                const b3Vec3 relativeVelocity = b3InvRotateVector(frameA,
                    b3Sub(b3Body_GetAngularVelocity(limit.bodyB),
                          b3Body_GetAngularVelocity(limit.bodyA)));
                b3Vec3 localTorque = {};
                const auto resistance = [](float value, float velocity,
                                           float maximum) {
                    const float excess = std::abs(value) - maximum;
                    if (excess <= 0.0f) return 0.0f;
                    return std::clamp(-std::copysign(18.0f * excess, value) -
                                      1.8f * velocity, -24.0f, 24.0f);
                };
                localTorque.x = resistance(rotationVector.x,
                                           relativeVelocity.x, limit.swing1);
                localTorque.y = resistance(rotationVector.y,
                                           relativeVelocity.y, limit.swing2);
                if (localTorque.x == 0.0f && localTorque.y == 0.0f) continue;
                const b3Vec3 worldTorque = b3RotateVector(frameA, localTorque);
                b3Body_ApplyTorque(limit.bodyA, b3Neg(worldTorque), true);
                b3Body_ApplyTorque(limit.bodyB, worldTorque, true);
            }
        }
    }

    void UpdateHarpoonAttachments(float step) {
        for (HarpoonRagdollAttachment& attachment : harpoonRagdolls) {
            for (HarpoonRagdollPart& part : attachment.parts) {
                if (B3_IS_NULL(part.anchorBody)) continue;
                const b3WorldTransform target = {
                    { part.desiredAnchor.x, part.desiredAnchor.y, part.desiredAnchor.z },
                    b3Quat_identity };
                b3Body_SetTargetTransform(part.anchorBody, target, step, true);
            }
        }
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

    // Alive enemies are active ragdolls: targets keep a readable human pose while
    // Box3D joints/collisions still add motion. On death these targets stop and the
    // same bodies become an ordinary loose ragdoll.
    void ApplyEnemyHover(float dt) {
        if (!enemyTargetValid) return;
        constexpr float approachMultiplier = 5.0f;
        constexpr float animationMultiplier = 3.0f;
        for (HoverEnemy& enemy : hoverEnemies) {
            if (!enemy.alive || B3_IS_NULL(enemy.torso)) continue;
            const b3Pos p = b3Body_GetPosition(enemy.torso);
            const b3Vec3 v = b3Body_GetLinearVelocity(enemy.torso);

            float ax = (float)p.x - enemyTarget.x;
            float az = (float)p.z - enemyTarget.z;
            float dist = std::sqrt(ax * ax + az * az);
            if (dist < 0.1f) { ax = 1.0f; az = 0.0f; dist = 1.0f; }
            ax /= dist; az /= dist;
            // Keep combat distance while gently orbiting. Avoids motionless
            // turret behaviour without requiring navmesh support.
            const float orbit = std::sin(enemy.phase * 0.35f * animationMultiplier) * 1.2f;
            const float desiredX = enemyTarget.x + ax * 9.0f - az * orbit;
            const float desiredZ = enemyTarget.z + az * 9.0f + ax * orbit;
            const float desiredY = enemy.hoverY +
                std::sin(enemy.phase * 1.35f * animationMultiplier) * 0.45f;

            // Force-based translation avoids SetTargetTransform trying to cover a
            // large spawn-to-player distance in one pose interval.
            const float mass = (std::max)(1.0f, enemy.totalMass);
            b3Vec3 force = {
                mass * (((desiredX - (float)p.x) * 0.45f * approachMultiplier) - v.x * 1.5f),
                mass * (9.81f + (desiredY - (float)p.y) * 3.5f * animationMultiplier - v.y * 2.4f),
                mass * (((desiredZ - (float)p.z) * 0.45f * approachMultiplier) - v.z * 1.5f)
            };
            const float horizontalCap = mass * 10.0f * approachMultiplier;
            const float verticalCap = mass * 10.0f * animationMultiplier;
            force.x = (std::max)(-horizontalCap, (std::min)(horizontalCap, force.x));
            force.y = (std::max)(-verticalCap, (std::min)(verticalCap, force.y));
            force.z = (std::max)(-horizontalCap, (std::min)(horizontalCap, force.z));
            b3Body_ApplyForceToCenter(enemy.torso, force, true);

            const XMVECTOR torso = XMVectorSet((float)p.x, (float)p.y, (float)p.z, 1.0f);
            XMVECTOR aim = XMLoadFloat3(&enemyTarget) + XMVectorSet(0, -0.35f, 0, 0) - torso;
            if (XMVectorGetX(XMVector3LengthSq(aim)) < 1e-5f) aim = XMVectorSet(0, 0, 1, 0);
            aim = XMVector3Normalize(aim);
            const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
            XMVECTOR bodyForward = XMVectorSet(XMVectorGetX(aim), 0.0f, XMVectorGetZ(aim), 0.0f);
            if (XMVectorGetX(XMVector3LengthSq(bodyForward)) < 1e-5f)
                bodyForward = XMVectorSet(0, 0, 1, 0);
            bodyForward = XMVector3Normalize(bodyForward);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, bodyForward));
            const XMVECTOR up = worldUp;

            auto quatFromAxes = [](XMVECTOR x, XMVECTOR y, XMVECTOR z) {
                XMMATRIX m = XMMatrixIdentity();
                m.r[0] = XMVectorSetW(x, 0); m.r[1] = XMVectorSetW(y, 0);
                m.r[2] = XMVectorSetW(z, 0);
                return XMQuaternionNormalize(XMQuaternionRotationMatrix(m));
            };
            const XMVECTOR bodyQ = quatFromAxes(right, up, bodyForward);

            const float poseTime = (std::max)(0.24f, dt * 2.0f);
            auto targetBody = [&](size_t localIndex, XMVECTOR position, XMVECTOR rotation) {
                const b3BodyId body = ragdollParts[enemy.firstPart + localIndex].body;
                XMFLOAT3 pos; XMFLOAT4 q;
                XMStoreFloat3(&pos, position); XMStoreFloat4(&q, rotation);
                b3WorldTransform target = { { pos.x, pos.y, pos.z }, { { q.x, q.y, q.z }, q.w } };
                b3Body_SetTargetTransform(body, target, poseTime, true);
            };
            auto targetSegment = [&](size_t localIndex, XMVECTOR a, XMVECTOR b) {
                XMVECTOR axis = XMVector3Normalize(b - a);
                XMVECTOR side = XMVector3Cross(axis, aim);
                if (XMVectorGetX(XMVector3LengthSq(side)) < 1e-5f)
                    side = XMVector3Cross(axis, right);
                side = XMVector3Normalize(side);
                XMVECTOR segmentForward = XMVector3Normalize(XMVector3Cross(side, axis));
                targetBody(localIndex, (a + b) * 0.5f,
                           quatFromAxes(side, axis, segmentForward));
            };

            // Chest and pelvis receive no pose targets. Hover force, joints,
            // inertia, and collisions control them. Head still tracks player.
            const XMVECTOR predictedTorso = torso + XMVectorSet(v.x, v.y, v.z, 0) * poseTime;
            targetBody(2, predictedTorso + up * 0.57f, bodyQ);

            // Two-handed aiming pose. Right hand holds trigger; left supports barrel.
            const XMVECTOR gunPos = predictedTorso + right * 0.04f + up * 0.04f + aim * 0.68f;
            const XMVECTOR rightHand = gunPos + right * 0.10f - aim * 0.06f;
            const XMVECTOR leftHand  = gunPos - right * 0.01f - up * 0.12f + aim * 0.18f;
            const XMVECTOR rightShoulder = predictedTorso + right * 0.29f + up * 0.23f;
            const XMVECTOR leftShoulder  = predictedTorso - right * 0.29f + up * 0.23f;
            auto solveElbow = [&](XMVECTOR shoulder, XMVECTOR hand, XMVECTOR bendHint) {
                constexpr float upperLength = 0.60f;
                constexpr float lowerLength = 0.54f;
                XMVECTOR delta = hand - shoulder;
                float distance = XMVectorGetX(XMVector3Length(delta));
                distance = (std::max)(0.08f,
                    (std::min)(upperLength + lowerLength - 0.015f, distance));
                const XMVECTOR direction = XMVector3Normalize(delta);
                const float along = (upperLength * upperLength - lowerLength * lowerLength +
                                     distance * distance) / (2.0f * distance);
                const float height = std::sqrt((std::max)(0.0f,
                    upperLength * upperLength - along * along));
                bendHint -= direction * XMVector3Dot(bendHint, direction);
                if (XMVectorGetX(XMVector3LengthSq(bendHint)) < 1e-5f) bendHint = up;
                bendHint = XMVector3Normalize(bendHint);
                return shoulder + direction * along + bendHint * height;
            };
            const XMVECTOR rightElbow = solveElbow(
                rightShoulder, rightHand, right * 0.85f - up * 0.25f);
            const XMVECTOR leftElbow = solveElbow(
                leftShoulder, leftHand, -right * 0.22f - up * 0.55f);
            // Local +Y points inward/upward on every authored limb. Preserve
            // that convention or Box3D joint anchors land on opposite ends.
            targetSegment(5, rightElbow, rightShoulder);
            targetSegment(6, rightHand, rightElbow);
            targetSegment(3, leftElbow, leftShoulder);
            targetSegment(4, leftHand, leftElbow);

            // Legs receive no animation targets. Hip/knee joints, gravity,
            // acceleration, collisions, and inertia drive them completely.
        }
    }

    void UpdateEnemyFire(float dt) {
        if (!enemyTargetValid) return;
        for (HoverEnemy& enemy : hoverEnemies) {
            if (!enemy.alive) {
                enemy.respawnTimer -= dt;
                if (enemy.respawnTimer <= 0.0f && enemy.spawnPositions.size() == 11) {
                    for (size_t i = 0; i < 11; ++i) {
                        const b3BodyId body = ragdollParts[enemy.firstPart + i].body;
                        const XMFLOAT3& p = enemy.spawnPositions[i];
                        const XMFLOAT4& q = enemy.spawnRotation;
                        b3Body_SetTransform(body, { p.x, p.y, p.z },
                            { { q.x, q.y, q.z }, q.w });
                        b3Body_SetLinearVelocity(body, { 0, 0, 0 });
                        b3Body_SetAngularVelocity(body, { 0, 0, 0 });
                        b3Body_SetAwake(body, true);
                    }
                    enemy.health = 100.0f;
                    enemy.shotsInBurst = 0;
                    enemy.fireCooldown = 0.5f;
                    enemy.alive = true;
                }
                continue;
            }
            if (B3_IS_NULL(enemy.torso)) continue;
            enemy.phase += dt;
            enemy.fireCooldown -= dt;
            if (enemy.fireCooldown > 0.0f) continue;

            const RagdollPart& triggerArm = ragdollParts[enemy.firstPart + 6];
            const b3Pos hand = b3Body_GetWorldPoint(triggerArm.body,
                { 0.0f, -triggerArm.half.y, 0.0f });
            XMVECTOR from = XMVectorSet((float)hand.x, (float)hand.y,
                                        (float)hand.z, 1.0f);
            XMVECTOR to = XMLoadFloat3(&enemyTarget) + XMVectorSet(0, -0.35f, 0, 0);
            XMVECTOR dir = to - from;
            const float distance = XMVectorGetX(XMVector3Length(dir));
            if (distance > 40.0f || distance < 1.0f) {
                enemy.fireCooldown = 0.0625f;
                continue;
            }
            dir = XMVector3Normalize(dir);
            // Small inaccuracy keeps sustained fire dangerous but dodgeable.
            const float jx = ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * 0.018f;
            const float jy = ((float)std::rand() / RAND_MAX * 2.0f - 1.0f) * 0.014f;
            dir = XMVector3Normalize(dir + XMVectorSet(jx, jy, -jx * 0.4f, 0));
            XMFLOAT3 origin, direction;
            XMStoreFloat3(&origin, from + dir * 1.35f);
            XMStoreFloat3(&direction, dir);
            pendingEnemyShots.push_back({ origin, direction });
            ++enemy.shotsInBurst;
            if (enemy.shotsInBurst >= 7) {
                enemy.shotsInBurst = 0;
                enemy.fireCooldown = 4.0f;
            } else {
                enemy.fireCooldown = 0.18f + ((float)std::rand() / RAND_MAX) * 0.1375f;
            }
        }
    }

    float ActorVolume(const ActorRuntime& runtime) const {
        float volume = 0.0f;
        for (uint32_t index : runtime.chunks) {
            const Chunk& chunk = chunks[index];
            volume += (std::max)(0.0f, chunk.maximum.x - chunk.minimum.x) *
                      (std::max)(0.0f, chunk.maximum.y - chunk.minimum.y) *
                      (std::max)(0.0f, chunk.maximum.z - chunk.minimum.z);
        }
        return volume;
    }

    float ActorMaxExtent(const ActorRuntime& runtime) const {
        XMFLOAT3 lo{ FLT_MAX, FLT_MAX, FLT_MAX };
        XMFLOAT3 hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (uint32_t index : runtime.chunks) {
            const Chunk& chunk = chunks[index];
            lo.x = (std::min)(lo.x, chunk.minimum.x);
            lo.y = (std::min)(lo.y, chunk.minimum.y);
            lo.z = (std::min)(lo.z, chunk.minimum.z);
            hi.x = (std::max)(hi.x, chunk.maximum.x);
            hi.y = (std::max)(hi.y, chunk.maximum.y);
            hi.z = (std::max)(hi.z, chunk.maximum.z);
        }
        return (std::max)({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z });
    }

    void SetDebrisCollisionLod(ActorRuntime& runtime, bool lod) {
        if (B3_IS_NULL(runtime.body) || runtime.collisionLod == lod) return;
        const int count = b3Body_GetShapeCount(runtime.body);
        if (count <= 0) return;
        std::vector<b3ShapeId> shapes(static_cast<size_t>(count));
        const int found = b3Body_GetShapes(runtime.body, shapes.data(), count);
        for (int i = 0; i < found; ++i) {
            b3Filter filter = b3Shape_GetFilter(shapes[i]);
            filter.categoryBits = lod ? CollisionCategoryLodDebris :
                (runtime.dynamic ? CollisionCategoryDebris : CollisionCategoryWorld);
            filter.maskBits = lod ? CollisionCategoryWorld : B3_DEFAULT_MASK_BITS;
            b3Shape_SetFilter(shapes[i], filter, true);
            b3Shape_EnableHitEvents(shapes[i], !lod);
        }
        runtime.collisionLod = lod;
    }

    void WakeDebris(ActorRuntime& runtime, bool restoreFullCollision = true) {
        if (B3_IS_NULL(runtime.body) || !runtime.dynamic) return;
        if (runtime.frozen) {
            b3Body_SetType(runtime.body, b3_dynamicBody);
            runtime.frozen = false;
        }
        runtime.settledTime = 0.0f;
        runtime.restTime = 0.0f;
        runtime.debrisAge = 0.0f;
        if (restoreFullCollision) SetDebrisCollisionLod(runtime, false);
        b3Body_SetAwake(runtime.body, true);
    }

    void ApplyVortices(float dt) {
        for (VortexRuntime& vortex : vortices) {
            vortex.age += dt;
            const bool release = vortex.age >= vortex.duration;
            const float captureRadius = vortex.radius * 1.35f;
            const float targetRadius = (std::max)(1.4f, vortex.radius * 0.42f);

            auto pullBody = [&](b3BodyId body, uint64_t seedValue) {
                if (B3_IS_NULL(body)) return;
                const b3Pos position = b3Body_GetPosition(body);
                float radialX = (float)position.x - vortex.center.x;
                float radialY = (float)position.y - vortex.center.y;
                float radialZ = (float)position.z - vortex.center.z;
                float distance = std::sqrt(radialX * radialX +
                    radialY * radialY + radialZ * radialZ);
                if (distance > captureRadius) return;

                if (distance < 0.05f) {
                    const float seed = static_cast<float>(
                        seedValue % 997u) * 2.399963f;
                    radialX = std::cos(seed);
                    radialY = std::sin(seed * 0.73f) * 0.55f;
                    radialZ = std::sin(seed);
                    distance = std::sqrt(radialX * radialX +
                        radialY * radialY + radialZ * radialZ);
                }
                radialX /= distance;
                radialY /= distance;
                radialZ /= distance;
                const float seedAngle = static_cast<float>(seedValue % 251u) *
                    2.399963f;
                const float axisX = std::cos(seedAngle) * 0.48f;
                const float axisY = 0.72f;
                const float axisZ = std::sin(seedAngle) * 0.48f;
                float tangentX = axisY * radialZ - axisZ * radialY;
                float tangentY = axisZ * radialX - axisX * radialZ;
                float tangentZ = axisX * radialY - axisY * radialX;
                float tangentLength = std::sqrt(tangentX * tangentX +
                    tangentY * tangentY + tangentZ * tangentZ);
                if (tangentLength < 0.05f) {
                    tangentX = -radialZ;
                    tangentY = 0.0f;
                    tangentZ = radialX;
                    tangentLength = std::sqrt(tangentX * tangentX +
                        tangentZ * tangentZ);
                }
                tangentX /= tangentLength;
                tangentY /= tangentLength;
                tangentZ /= tangentLength;
                const b3Vec3 velocity = b3Body_GetLinearVelocity(body);

                if (release) {
                    b3Body_SetLinearVelocity(body, {
                        velocity.x + radialX * 9.0f + tangentX * 3.5f,
                        velocity.y + radialY * 9.0f + tangentY * 3.5f + 3.0f,
                        velocity.z + radialZ * 9.0f + tangentZ * 3.5f });
                    b3Body_SetAngularVelocity(body, {
                        3.5f, 7.5f, 2.8f });
                    return;
                }

                const float radialCorrection = (std::max)(-5.0f,
                    (std::min)(8.0f,
                        (distance - targetRadius) * 3.8f));
                const float orbitSpeed = 9.5f;
                const b3Vec3 desired = {
                    tangentX * orbitSpeed - radialX * radialCorrection,
                    tangentY * orbitSpeed - radialY * radialCorrection,
                    tangentZ * orbitSpeed - radialZ * radialCorrection };
                const float blend = (std::min)(1.0f, dt * 11.0f);
                b3Body_SetLinearVelocity(body, {
                    velocity.x + (desired.x - velocity.x) * blend,
                    velocity.y + (desired.y - velocity.y) * blend,
                    velocity.z + (desired.z - velocity.z) * blend });
                b3Body_SetAngularVelocity(body, {
                    2.4f + radialZ * 1.2f, 6.8f, 2.4f - radialX * 1.2f });
                b3Body_SetAwake(body, true);
            };

            for (auto& runtime : actors) {
                if (!runtime->dynamic || B3_IS_NULL(runtime->body)) continue;
                const b3Pos p = b3Body_GetPosition(runtime->body);
                const float dx = (float)p.x - vortex.center.x;
                const float dy = (float)p.y - vortex.center.y;
                const float dz = (float)p.z - vortex.center.z;
                const bool inside = dx * dx + dy * dy + dz * dz <=
                    captureRadius * captureRadius;
                const bool captured = vortex.capturedActorIds.count(
                    runtime->renderId) != 0;
                if (!inside && !captured)
                    continue;
                if (inside) vortex.capturedActorIds.insert(runtime->renderId);
                WakeDebris(*runtime, release);
                // Hundreds of mutually colliding fragments turn a controlled
                // spherical orbit into a solver storm. Keep world contacts but
                // suppress debris-debris contacts until the outward release.
                SetDebrisCollisionLod(*runtime, !release);
                pullBody(runtime->body, runtime->renderId);
            }
            for (const BarrelRuntime& barrel : barrelBodies)
                pullBody(barrel.body, 10000u + barrel.handle);
            for (size_t i = 0; i < ragdollParts.size(); ++i)
                pullBody(ragdollParts[i].body, 20000u + i);
        }
        vortices.erase(
            std::remove_if(vortices.begin(), vortices.end(),
                [](const VortexRuntime& vortex) {
                    return vortex.age >= vortex.duration;
                }),
            vortices.end());
    }

    void EmitTinyDebris(const ActorRuntime& runtime, const BodySeed* seed) {
        XMFLOAT3 position = runtime.center;
        XMFLOAT3 velocity{};
        if (seed && seed->valid) {
            const XMVECTOR offset = XMVectorSet(runtime.center.x - seed->modelCenter.x,
                runtime.center.y - seed->modelCenter.y, runtime.center.z - seed->modelCenter.z, 0.0f);
            XMFLOAT3 rotated;
            XMStoreFloat3(&rotated, XMVector3Rotate(offset,
                XMVectorSet(seed->rotation.v.x, seed->rotation.v.y,
                            seed->rotation.v.z, seed->rotation.s)));
            position = { (float)seed->position.x + rotated.x,
                         (float)seed->position.y + rotated.y,
                         (float)seed->position.z + rotated.z };
            velocity = { seed->linearVelocity.x, seed->linearVelocity.y,
                         seed->linearVelocity.z };
        }
        const float size = (std::clamp)(ActorMaxExtent(runtime) * 0.55f, 0.035f, 0.12f);
        for (int i = 0; i < 3; ++i) {
            const float rx = (float(std::rand()) / RAND_MAX) * 2.0f - 1.0f;
            const float rz = (float(std::rand()) / RAND_MAX) * 2.0f - 1.0f;
            tinyDebrisParticles.push_back({ position,
                { velocity.x + rx * 1.4f, velocity.y + 0.8f + 0.35f * i,
                  velocity.z + rz * 1.4f }, size });
        }
    }

    bool BuildChunks() {
        if (!source || !device) return false;

        auto buildChunkResources = [&]() {
            if (chunks.empty()) return false;
            std::atomic<size_t> nextChunk{0};
            std::atomic<bool> succeeded{true};
            const unsigned hardwareThreads = (std::max)(1u,
                std::thread::hardware_concurrency());
            const unsigned workerCount = (std::min)(
                (std::max)(1u, hardwareThreads > 2 ? hardwareThreads - 1 : hardwareThreads),
                (std::min)(16u, static_cast<unsigned>(chunks.size())));
            auto worker = [&]() {
                while (succeeded.load(std::memory_order_relaxed)) {
                    const size_t index = nextChunk.fetch_add(1,
                        std::memory_order_relaxed);
                    if (index >= chunks.size()) break;
                    for (MeshPrimitive& primitive : chunks[index].node->mesh->primitives) {
                        if (!GLBImporter::BuildMeshletData(primitive, device)) {
                            succeeded.store(false, std::memory_order_relaxed);
                            break;
                        }
                    }
                }
            };
            std::vector<std::thread> workers;
            workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
            for (unsigned i = 1; i < workerCount; ++i)
                workers.emplace_back(worker);
            worker();
            for (std::thread& thread : workers) thread.join();
            return succeeded.load(std::memory_order_relaxed);
        };

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
                    const char* groupText = sourceChunk->name.c_str() + at + 1;
                    char* groupEnd = nullptr;
                    const long parsedGroup = std::strtol(groupText, &groupEnd, 10);
                    // Only numeric suffixes identify multi-cell boards. Names
                    // such as MetalTrim@RidgeCap are independent rigid pieces;
                    // atoi() previously collapsed every one of them into group 0.
                    if (groupEnd != groupText && *groupEnd == '\0')
                        chunk.plankGroup = static_cast<int>(parsedGroup);
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
            return buildChunkResources();
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
                chunk.node->mesh->primitives.push_back(std::move(primitive));
            }
            chunks.push_back(std::move(chunk));
        }
        return buildChunkResources();
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
        // Corrugated wall cells meet their foundation along a deliberately thin
        // 6 cm edge. That contact is structurally valid but smaller than the
        // generic threshold. Without this exception, every metal house is a
        // disconnected unsupported island; splitting any house starts all metal
        // houses' debris timers at once.
        constexpr float minSupportContactArea = 0.004f;
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
            const bool touchesSupport = ca.support || cb.support;
            if (area < minContactArea &&
                (!touchesSupport || area < minSupportContactArea)) continue;
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

        // Initial connected components are independent buildings/structures.
        // Damage schedules only the affected component in the structural queue.
        std::vector<uint32_t> parent(chunks.size());
        for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
        std::function<uint32_t(uint32_t)> findRoot = [&](uint32_t v) -> uint32_t {
            return parent[v] == v ? v : (parent[v] = findRoot(parent[v]));
        };
        for (const BondPair& pair : bondPairs) {
            const uint32_t a = findRoot(pair.a), b = findRoot(pair.b);
            if (a != b) parent[b] = a;
        }
        std::unordered_map<uint32_t, uint32_t> componentIds;
        for (uint32_t i = 0; i < chunks.size(); ++i) {
            const uint32_t root = findRoot(i);
            auto [it, inserted] = componentIds.emplace(root,
                static_cast<uint32_t>(componentIds.size()));
            chunks[i].structureId = it->second;
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
        runtime->renderId = nextActorRenderId++;
        runtime->structureId = chunks.empty() ? 0 : chunks.front().structureId;
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
        // Flying prototype enemies disabled. Skinned Bandit is active test enemy.
        return true;
    }

    // (Re)build the static ground collider. The native Box3D height field uses
    // continuous triangles rather than stair-step columns, which lets vehicle
    // suspension follow the rendered hills without catching every grid edge.
    void BuildGround() {
        if (B3_IS_NULL(world)) return;
        if (!B3_IS_NULL(ground)) { b3DestroyBody(ground); ground = b3_nullBodyId; }
        if (terrainHeightField) {
            b3DestroyHeightField(terrainHeightField);
            terrainHeightField = nullptr;
        }

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
            constexpr float extent = 60.0f;
            constexpr float cell = 0.5f;
            constexpr int points = static_cast<int>(extent * 2.0f / cell) + 1;
            std::vector<float> heights(static_cast<size_t>(points) * points);
            float minimumHeight = FLT_MAX;
            float maximumHeight = -FLT_MAX;
            for (int z = 0; z < points; ++z)
            for (int x = 0; x < points; ++x) {
                const float height = terrainSampler(
                    -extent + x * cell, -extent + z * cell);
                heights[static_cast<size_t>(z) * points + x] = height;
                minimumHeight = (std::min)(minimumHeight, height);
                maximumHeight = (std::max)(maximumHeight, height);
            }

            b3HeightFieldDef heightFieldDef = {};
            heightFieldDef.heights = heights.data();
            heightFieldDef.scale = { cell, 1.0f, cell };
            heightFieldDef.countX = points;
            heightFieldDef.countZ = points;
            heightFieldDef.globalMinimumHeight = minimumHeight - 0.05f;
            heightFieldDef.globalMaximumHeight = maximumHeight + 0.05f;
            terrainHeightField = b3CreateHeightField(&heightFieldDef);

            b3BodyDef bodyDef = b3DefaultBodyDef();
            bodyDef.position = { -extent, 0.0f, -extent };
            ground = b3CreateBody(world, &bodyDef);
            b3ShapeDef shapeDef = b3DefaultShapeDef();
            shapeDef.baseMaterial.friction = 1.15f;
            shapeDef.enableHitEvents = true;
            b3CreateHeightFieldShape(ground, &shapeDef, terrainHeightField);
        } else {
            ground = addStaticBox(0.0f, -0.5f, 0.0f, 60.0f, 0.5f, 60.0f);
        }
    }

    void CreateRagdolls() {
        struct PartDef { XMFLOAT3 center, half, color; uint8_t shape; };
        const XMFLOAT3 skin{ 0.62f, 0.39f, 0.27f };
        const XMFLOAT3 shirt{ 0.20f, 0.27f, 0.10f }; // military olive drab
        const XMFLOAT3 pants{ 0.025f, 0.028f, 0.025f }; // tactical black
        const PartDef defs[] = {
            {{0,1.45f,0},{0.28f,0.38f,0.16f},shirt,1},   // torso
            {{0,0.92f,0},{0.23f,0.16f,0.15f},pants,1},   // pelvis
            {{0,2.02f,0},{0.18f,0.22f,0.18f},skin,2},    // head
            {{-0.39f,1.48f,0},{0.12f,0.30f,0.11f},shirt,1},
            {{-0.39f,0.94f,0},{0.10f,0.27f,0.09f},skin,1},
            {{ 0.39f,1.48f,0},{0.12f,0.30f,0.11f},shirt,1},
            {{ 0.39f,0.94f,0},{0.10f,0.27f,0.09f},skin,1},
            {{-0.15f,0.53f,0},{0.14f,0.28f,0.13f},pants,1},
            {{-0.15f,0.04f,0},{0.11f,0.25f,0.10f},pants,1},
            {{ 0.15f,0.53f,0},{0.14f,0.28f,0.13f},pants,1},
            {{ 0.15f,0.04f,0},{0.11f,0.25f,0.10f},pants,1},
        };
        struct Link { int a, b; XMFLOAT3 anchor; };
        const Link links[] = {
            {0,1,{0,1.08f,0}}, {0,2,{0,1.82f,0}},
            {0,3,{-0.29f,1.68f,0}}, {3,4,{-0.38f,1.20f,0}},
            {0,5,{ 0.29f,1.68f,0}}, {5,6,{ 0.38f,1.20f,0}},
            {1,7,{-0.15f,0.76f,0}}, {7,8,{-0.15f,0.28f,0}},
            {1,9,{ 0.15f,0.76f,0}}, {9,10,{0.15f,0.28f,0}},
        };

        auto spawn = [&](XMFLOAT3 origin, float pitch, float yaw, float roll, float phase) {
            const size_t base = ragdollParts.size();
            float totalMass = 0.0f;
            std::vector<XMFLOAT3> spawnPositions;
            spawnPositions.reserve(11);
            XMVECTOR rq = XMQuaternionRotationRollPitchYaw(pitch, yaw, roll);
            XMFLOAT4 qf; XMStoreFloat4(&qf, rq);
            for (const PartDef& def : defs) {
                XMFLOAT3 rotated;
                XMStoreFloat3(&rotated, XMVector3Rotate(XMLoadFloat3(&def.center), rq));
                b3BodyDef bd = b3DefaultBodyDef();
                bd.type = b3_dynamicBody;
                bd.position = { origin.x + rotated.x, origin.y + rotated.y, origin.z + rotated.z };
                spawnPositions.push_back({ (float)bd.position.x, (float)bd.position.y,
                                           (float)bd.position.z });
                bd.rotation = { { qf.x, qf.y, qf.z }, qf.w };
                bd.linearDamping = 0.12f; bd.angularDamping = 0.35f;
                b3BodyId body = b3CreateBody(world, &bd);
                b3ShapeDef sd = b3DefaultShapeDef();
                sd.density = 55.0f; sd.baseMaterial.friction = 0.72f;
                sd.baseMaterial.restitution = 0.02f;
                b3BoxHull box = b3MakeBoxHull(def.half.x, def.half.y, def.half.z);
                b3CreateHullShape(body, &sd, &box.base);
                ragdollParts.push_back({ body, def.half, def.color, def.shape });
                totalMass += b3Body_GetMass(body);
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
            HoverEnemy enemy;
            enemy.firstPart = base;
            enemy.torso = ragdollParts[base].body;
            enemy.totalMass = totalMass;
            enemy.hoverY = (std::max)(4.5f, origin.y + 1.8f);
            enemy.phase = phase;
            enemy.fireCooldown = 0.1125f + phase * 0.0425f;
            enemy.spawnPositions = std::move(spawnPositions);
            enemy.spawnRotation = qf;
            hoverEnemies.push_back(enemy);
        };

        // Player starts at z=+20 looking toward the house at the origin. Enemy
        // spawner mirrors that point 20 m behind the house at z=-20.
        constexpr float spawnerZ = -20.0f;
        spawn({ -3.0f, 3.2f, spawnerZ },       0.0f, 0.0f, -0.15f, 0.0f);
        spawn({  0.0f, 3.8f, spawnerZ - 1.0f },0.0f, 0.0f,  0.12f, 1.7f);
        spawn({  3.0f, 3.2f, spawnerZ },       0.0f, 0.0f, -0.10f, 3.4f);
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
        runtime.frozen = false;
        runtime.collisionLod = false;
        runtime.settledTime = 0.0f;
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
        shapeDef.filter.categoryBits = runtime.dynamic ?
            CollisionCategoryDebris : CollisionCategoryWorld;
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

    static uint64_t HashChunks(const std::vector<uint32_t>& actorChunks) {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t chunk : actorChunks) {
            hash ^= chunk;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    static BatchBuildResult BuildActorBatch(
        ID3D12Device* batchDevice, uint64_t actorId, uint64_t chunkHash,
        std::vector<std::shared_ptr<SceneNode>> nodes,
        bool background = false) {
        if (background)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        BatchBuildResult result;
        result.actorId = actorId;
        result.chunkHash = chunkHash;
        auto root = std::make_shared<SceneNode>("DestructionActorBatch");
        root->children = std::move(nodes);
        // Shared chunk nodes retain their authored local transforms. Do not
        // recursively rewrite their globals while building a background batch.
        ComPtr<ID3D12Device> deviceRef = background
            ? ComPtr<ID3D12Device>() : ComPtr<ID3D12Device>(batchDevice);
        auto shadowFuture = std::async(std::launch::async,
            [root, deviceRef, background]() {
                if (background)
                    SetThreadPriority(GetCurrentThread(),
                        THREAD_PRIORITY_BELOW_NORMAL);
                return GLBImporter::MergeSceneForDepth(root, deviceRef);
            });
        result.colourNode = GLBImporter::MergeSceneByMaterial(root, deviceRef);
        result.shadowNode = shadowFuture.get();
        return result;
    }

    static SpatialBatchBuildResult BuildSpatialBatch(
        const SpatialCellKey& key,
        uint64_t signature, std::vector<SpatialBatchSource> sources,
        const XMFLOAT3& center, float radius, uint32_t chunkCount) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        SpatialBatchBuildResult result;
        result.key = key;
        result.signature = signature;
        result.center = center;
        result.radius = radius;
        result.chunkCount = chunkCount;
        auto root = std::make_shared<SceneNode>("DestructionSpatialBatch");
        root->children.reserve(sources.size());
        for (const SpatialBatchSource& source : sources) {
            if (!source.node) continue;
            auto wrapper = std::make_shared<SceneNode>("SettledChunkTransform");
            const XMMATRIX combined =
                XMLoadFloat4x4(&source.node->globalTransform) *
                XMLoadFloat4x4(&source.transform);
            XMVECTOR scale, rotation, translation;
            if (XMMatrixDecompose(&scale, &rotation, &translation, combined)) {
                XMStoreFloat3(&wrapper->scale, scale);
                XMStoreFloat4(&wrapper->rotation, rotation);
                XMStoreFloat3(&wrapper->translation, translation);
            }
            // Existing draws use node.global * actorWorld. Bake that exact
            // transform at the source root, then retain descendant local
            // transforms without mutating/reparenting shared scene nodes.
            wrapper->mesh = source.node->mesh;
            wrapper->children = source.node->children;
            root->children.push_back(std::move(wrapper));
        }
        // Geometry merging is deliberately CPU-only. D3D resource creation on
        // this worker serialized with rendering and made shutdown wait inside
        // the driver. PollSpatialBatchBuild uploads completed meshes on the
        // render thread.
        ComPtr<ID3D12Device> noDevice;
        auto shadowFuture = std::async(std::launch::async,
            [root, noDevice]() {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                return GLBImporter::MergeSceneForDepth(root, noDevice);
            });
        result.colourNode = GLBImporter::MergeSceneByMaterial(root, noDevice);
        result.shadowNode = shadowFuture.get();
        return result;
    }

    static bool UploadMergedNode(const std::shared_ptr<SceneNode>& node,
                                 ID3D12Device* uploadDevice) {
        if (!node) return false;
        if (node->mesh) {
            for (MeshPrimitive& primitive : node->mesh->primitives) {
                // CPU-only merges deliberately have no GPU resources yet.
                // Never trust a raw view value as ownership proof: older/default
                // constructed primitives could contain debug-fill bytes.
                if (primitive.vertexBuffer &&
                    primitive.vbv.BufferLocation != 0) continue;
                if (!GLBImporter::BuildMeshletData(
                        primitive, uploadDevice, false)) return false;
            }
        }
        for (const auto& child : node->children) {
            if (!UploadMergedNode(child, uploadDevice)) return false;
        }
        return true;
    }

    static SpatialCellKey SpatialCellFor(const XMFLOAT3& center) {
        return {
            static_cast<int>(std::floor(center.x / SpatialBatchCellSize)),
            static_cast<int>(std::floor(center.y / SpatialBatchCellSize)),
            static_cast<int>(std::floor(center.z / SpatialBatchCellSize)) };
    }

    static void HashSpatialSource(uint64_t& signature, uint64_t actorId,
                                  uint32_t chunkIndex,
                                  const XMFLOAT4X4& transform) {
        auto mix = [&](uint64_t value) {
            signature ^= value;
            signature *= 1099511628211ull;
        };
        mix(actorId);
        mix(chunkIndex);
        const uint32_t* words = reinterpret_cast<const uint32_t*>(&transform);
        for (size_t i = 0; i < sizeof(transform) / sizeof(uint32_t); ++i)
            mix(words[i]);
    }

    void EnsureActorBatchBounds(ActorRuntime& runtime) {
        if (runtime.batchBoundsValid) return;
        XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
        XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (uint32_t index : runtime.chunks) {
            const Chunk& chunk = chunks[index];
            minimum.x = (std::min)(minimum.x, chunk.minimum.x);
            minimum.y = (std::min)(minimum.y, chunk.minimum.y);
            minimum.z = (std::min)(minimum.z, chunk.minimum.z);
            maximum.x = (std::max)(maximum.x, chunk.maximum.x);
            maximum.y = (std::max)(maximum.y, chunk.maximum.y);
            maximum.z = (std::max)(maximum.z, chunk.maximum.z);
        }
        runtime.batchCenter = {
            (minimum.x + maximum.x) * 0.5f,
            (minimum.y + maximum.y) * 0.5f,
            (minimum.z + maximum.z) * 0.5f };
        runtime.batchRadius = 0.55f * XMVectorGetX(XMVector3Length(
            XMLoadFloat3(&maximum) - XMLoadFloat3(&minimum)));
        runtime.batchBoundsValid = true;
    }

    bool PollBatchBuild() {
        if (!batchBuildInFlight ||
            batchBuildFuture.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) return false;
        BatchBuildResult result = batchBuildFuture.get();
        batchBuildInFlight = false;
        auto actorIt = std::find_if(actors.begin(), actors.end(),
            [&](const std::unique_ptr<ActorRuntime>& runtime) {
                return runtime->renderId == result.actorId;
            });
        if (actorIt == actors.end()) return true;
        ActorRuntime* runtime = actorIt->get();
        if (HashChunks(runtime->chunks) != result.chunkHash) return true;
        if (!result.colourNode || !result.shadowNode) {
            runtime->failedBatchHash = result.chunkHash;
            return true;
        }
        if (!UploadMergedNode(result.colourNode, device) ||
            !UploadMergedNode(result.shadowNode, device)) {
            runtime->failedBatchHash = result.chunkHash;
            return true;
        }

        const UINT retireSlot = g_dx12.frameIndex % FRAME_COUNT;
        const UINT64 retireEpoch = g_dx12.fenceValues[retireSlot];
        if (retiredBatchEpoch[retireSlot] != retireEpoch) {
            retiredBatchNodes[retireSlot].clear();
            retiredBatchEpoch[retireSlot] = retireEpoch;
        }
        BatchCacheEntry& cache = batchCache[runtime];
        if (cache.colourNode)
            retiredBatchNodes[retireSlot].push_back(std::move(cache.colourNode));
        if (cache.shadowNode)
            retiredBatchNodes[retireSlot].push_back(std::move(cache.shadowNode));
        cache = { result.chunkHash, std::move(result.colourNode),
                  std::move(result.shadowNode) };
        runtime->failedBatchHash = 0;
        ++batchGeometryRebuildCount;
        return true;
    }

    bool PollSpatialBatchBuild() {
        if (!spatialBatchBuildInFlight ||
            spatialBatchBuildFuture.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) return false;
        SpatialBatchBuildResult result = spatialBatchBuildFuture.get();
        spatialBatchBuildInFlight = false;
        if (!result.colourNode || !result.shadowNode) return true;
        if (!UploadMergedNode(result.colourNode, device) ||
            !UploadMergedNode(result.shadowNode, device)) return true;

        const UINT retireSlot = g_dx12.frameIndex % FRAME_COUNT;
        const UINT64 retireEpoch = g_dx12.fenceValues[retireSlot];
        if (retiredBatchEpoch[retireSlot] != retireEpoch) {
            retiredBatchNodes[retireSlot].clear();
            retiredBatchEpoch[retireSlot] = retireEpoch;
        }
        SpatialBatchCacheEntry& cache = spatialBatchCache[result.key];
        if (cache.colourNode)
            retiredBatchNodes[retireSlot].push_back(std::move(cache.colourNode));
        if (cache.shadowNode)
            retiredBatchNodes[retireSlot].push_back(std::move(cache.shadowNode));
        cache.signature = result.signature;
        cache.colourNode = std::move(result.colourNode);
        cache.shadowNode = std::move(result.shadowNode);
        cache.center = result.center;
        cache.radius = result.radius;
        cache.chunkCount = result.chunkCount;
        ++batchGeometryRebuildCount;
        return true;
    }

    // Is there anything whose transform could have changed since the last rebuild?
    //
    // An intact house is entirely STATIC -- every chunk is anchored, nothing moves --
    // yet the render items were being rebuilt from scratch every single frame, walking
    // all ~588 chunks to recompute transforms that were identical to last frame's.
    // Nothing is dynamic until you actually shoot something, so the common case was
    // paying full price for no change at all. (PalmTrees already had this early-out;
    // destruction did not.)
    // Box3D puts a body to sleep once it has come to rest, so ASLEEP is the test --
    // not merely "is it a dynamic body". Debris that has finished falling is
    // dynamic forever after, and checking only the flag would mean the rebuild
    // came back permanently the first time the player broke anything.
    bool AnythingMoving() const {
        for (const RagdollPart& part : ragdollParts)
            if (!B3_IS_NULL(part.body) && b3Body_IsAwake(part.body)) return true;
        for (const auto& runtime : actors)
            if (runtime->dynamic && !B3_IS_NULL(runtime->body) && b3Body_IsAwake(runtime->body))
                return true;
        return false;
    }

    void RebuildRenderItems() {
        ++renderItemRebuildCount;
        // Any direct rebuild (a bullet strike, a grenade, init) means the scene just
        // changed. Drop the "settled" latch so Update re-evaluates from scratch
        // rather than assuming its cached items are still good.
        rebuiltWhileStill = false;
        renderItems.clear();
        renderBatches.clear();
        std::unordered_map<const ActorRuntime*, bool> liveActors;
        const UINT retireSlot = g_dx12.frameIndex % FRAME_COUNT;
        const UINT64 retireEpoch = g_dx12.fenceValues[retireSlot];
        if (retiredBatchEpoch[retireSlot] != retireEpoch) {
            retiredBatchNodes[retireSlot].clear();
            retiredBatchEpoch[retireSlot] = retireEpoch;
        }
        auto retireBatch = [&](BatchCacheEntry& entry) {
            if (entry.colourNode)
                retiredBatchNodes[retireSlot].push_back(std::move(entry.colourNode));
            if (entry.shadowNode)
                retiredBatchNodes[retireSlot].push_back(std::move(entry.shadowNode));
        };
        std::unordered_map<SpatialCellKey, SpatialBatchBuild,
            SpatialCellKeyHash> desiredSpatialBatches;
        for (const auto& runtime : actors) {
            liveActors[runtime.get()] = true;
            XMMATRIX transform = XMMatrixIdentity();
            if (!B3_IS_NULL(runtime->body)) transform = BoxTransform(runtime->body, runtime->center);
            XMFLOAT4X4 stored; XMStoreFloat4x4(&stored, transform);
            const uint64_t hash = HashChunks(runtime->chunks);
            auto cacheIt = batchCache.find(runtime.get());
            if (cacheIt != batchCache.end() &&
                (cacheIt->second.chunkHash != hash ||
                 !cacheIt->second.colourNode || !cacheIt->second.shadowNode)) {
                retireBatch(cacheIt->second);
                batchCache.erase(cacheIt);
                cacheIt = batchCache.end();
            }
            if (cacheIt != batchCache.end()) {
                EnsureActorBatchBounds(*runtime);
                XMFLOAT3 worldCenter;
                XMStoreFloat3(&worldCenter, XMVector3Transform(
                    XMLoadFloat3(&runtime->batchCenter), transform));
                renderBatches.push_back({ cacheIt->second.colourNode,
                    cacheIt->second.shadowNode, stored, worldCenter,
                    runtime->batchRadius,
                    static_cast<uint32_t>(runtime->chunks.size()) });
                continue;
            }

            // Split debris has independent physics while moving. Once asleep,
            // bake its final world transform into a spatial/material batch. A
            // changed impact only changes signatures in cells it touched.
            const bool spatiallyStable = !runtime->dynamic ||
                (runtime->debrisCleanupEligible && runtime->chunks.size() == 1);
            const bool settledForSpatialBatch = spatiallyStable &&
                !B3_IS_NULL(runtime->body) && !b3Body_IsAwake(runtime->body);
            if (settledForSpatialBatch) {
                for (uint32_t chunkIndex : runtime->chunks) {
                    const Chunk& chunk = chunks[chunkIndex];
                    const XMVECTOR extent = XMLoadFloat3(&chunk.maximum) -
                        XMLoadFloat3(&chunk.minimum);
                    const float radius = 0.55f *
                        XMVectorGetX(XMVector3Length(extent));
                    XMFLOAT3 worldCenter;
                    XMStoreFloat3(&worldCenter, XMVector3Transform(
                        XMLoadFloat3(&chunk.center), transform));
                    SpatialBatchBuild& build =
                        desiredSpatialBatches[SpatialCellFor(worldCenter)];
                    build.sources.push_back({ chunk.node, stored, worldCenter, radius });
                    ++build.chunkCount;
                    build.minimum.x = (std::min)(build.minimum.x, worldCenter.x - radius);
                    build.minimum.y = (std::min)(build.minimum.y, worldCenter.y - radius);
                    build.minimum.z = (std::min)(build.minimum.z, worldCenter.z - radius);
                    build.maximum.x = (std::max)(build.maximum.x, worldCenter.x + radius);
                    build.maximum.y = (std::max)(build.maximum.y, worldCenter.y + radius);
                    build.maximum.z = (std::max)(build.maximum.z, worldCenter.z + radius);
                    HashSpatialSource(build.signature, runtime->renderId,
                        chunkIndex, stored);
                }
                continue;
            }
            XMFLOAT3 batchMin(FLT_MAX, FLT_MAX, FLT_MAX);
            XMFLOAT3 batchMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            std::vector<DestructionRenderItem> actorItems;
            actorItems.reserve(runtime->chunks.size());
            for (uint32_t chunk : runtime->chunks) {
                // BoxTransform is rigid, so the model-space half-diagonal is the
                // world radius unchanged; +10% guards frustum-edge pop-in.
                const Chunk& c = chunks[chunk];
                const XMVECTOR extent = XMLoadFloat3(&c.maximum) - XMLoadFloat3(&c.minimum);
                const float radius =
                    0.55f * XMVectorGetX(XMVector3Length(extent));
                XMFLOAT3 worldCenter;
                XMStoreFloat3(&worldCenter, XMVector3Transform(XMLoadFloat3(&c.center), transform));
                actorItems.push_back({ c.node, stored, worldCenter, radius });
                batchMin.x = (std::min)(batchMin.x, worldCenter.x - radius);
                batchMin.y = (std::min)(batchMin.y, worldCenter.y - radius);
                batchMin.z = (std::min)(batchMin.z, worldCenter.z - radius);
                batchMax.x = (std::max)(batchMax.x, worldCenter.x + radius);
                batchMax.y = (std::max)(batchMax.y, worldCenter.y + radius);
                batchMax.z = (std::max)(batchMax.z, worldCenter.z + radius);
            }

            // Initial intact structure gets one actor batch. Post-split stable
            // pieces use small spatial cells; huge actor-wide merges cause
            // memory spikes and make fracture transitions visually coarse.
            const bool eligible = initialBatchBuild &&
                runtime->chunks.size() > 1 && runtime->failedBatchHash != hash;
            if (cacheIt == batchCache.end() && eligible) {
                std::vector<std::shared_ptr<SceneNode>> nodes;
                nodes.reserve(runtime->chunks.size());
                for (uint32_t chunk : runtime->chunks)
                    nodes.push_back(chunks[chunk].node);
                if (initialBatchBuild) {
                    BatchBuildResult result = BuildActorBatch(
                        device, runtime->renderId, hash, std::move(nodes));
                    if (result.colourNode && result.shadowNode) {
                        batchCache[runtime.get()] = { hash,
                            std::move(result.colourNode),
                            std::move(result.shadowNode) };
                        ++batchGeometryRebuildCount;
                        cacheIt = batchCache.find(runtime.get());
                    } else {
                        runtime->failedBatchHash = hash;
                    }
                }
            }

            if (cacheIt != batchCache.end() && batchMin.x != FLT_MAX) {
                EnsureActorBatchBounds(*runtime);
                XMFLOAT3 center;
                XMStoreFloat3(&center, XMVector3Transform(
                    XMLoadFloat3(&runtime->batchCenter), transform));
                renderBatches.push_back({ cacheIt->second.colourNode,
                    cacheIt->second.shadowNode, stored, center,
                    runtime->batchRadius,
                    static_cast<uint32_t>(runtime->chunks.size()) });
            } else {
                renderItems.insert(renderItems.end(), actorItems.begin(), actorItems.end());
            }
        }

        const XMFLOAT4X4 identity = [] {
            XMFLOAT4X4 value;
            XMStoreFloat4x4(&value, XMMatrixIdentity());
            return value;
        }();
        for (auto& desired : desiredSpatialBatches) {
            const SpatialCellKey& key = desired.first;
            SpatialBatchBuild& build = desired.second;
            auto cacheIt = spatialBatchCache.find(key);
            if (cacheIt == spatialBatchCache.end() ||
                cacheIt->second.signature != build.signature) {
                if (!spatialBatchBuildInFlight) {
                    const XMFLOAT3 center = {
                        (build.minimum.x + build.maximum.x) * 0.5f,
                        (build.minimum.y + build.maximum.y) * 0.5f,
                        (build.minimum.z + build.maximum.z) * 0.5f };
                    const float radius = 0.55f * XMVectorGetX(XMVector3Length(
                        XMLoadFloat3(&build.maximum) -
                        XMLoadFloat3(&build.minimum)));
                    std::vector<SpatialBatchSource> sources = build.sources;
                    const uint64_t signature = build.signature;
                    const uint32_t chunkCount = build.chunkCount;
                    spatialBatchBuildInFlight = true;
                    spatialBatchBuildFuture = std::async(std::launch::async,
                        [key, signature, sources = std::move(sources),
                         center, radius, chunkCount]() mutable {
                            return BuildSpatialBatch(key, signature,
                                std::move(sources), center, radius,
                                chunkCount);
                        });
                }
            }
            if (cacheIt != spatialBatchCache.end() &&
                cacheIt->second.signature == build.signature) {
                const SpatialBatchCacheEntry& entry = cacheIt->second;
                renderBatches.push_back({ entry.colourNode, entry.shadowNode,
                    identity, entry.center, entry.radius, entry.chunkCount });
            } else {
                // Allocation/build failure must not hide geometry.
                for (const SpatialBatchSource& source : build.sources)
                    renderItems.push_back({ source.node, source.transform,
                        source.center, source.radius });
            }
        }
        for (auto it = spatialBatchCache.begin();
             it != spatialBatchCache.end();) {
            if (desiredSpatialBatches.find(it->first) ==
                desiredSpatialBatches.end()) {
                if (it->second.colourNode)
                    retiredBatchNodes[retireSlot].push_back(
                        std::move(it->second.colourNode));
                if (it->second.shadowNode)
                    retiredBatchNodes[retireSlot].push_back(
                        std::move(it->second.shadowNode));
                it = spatialBatchCache.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = batchCache.begin(); it != batchCache.end();) {
            if (!liveActors.count(it->first)) {
                retireBatch(it->second);
                it = batchCache.erase(it);
            }
            else ++it;
        }
        ragdollRenderItems.clear();
        for (const RagdollPart& part : ragdollParts) {
            // Authored corpses render through their original skinned mesh.
            if (part.authoredId != InvalidIndex) continue;
            const b3Pos p = b3Body_GetPosition(part.body);
            const b3Quat q = b3Body_GetRotation(part.body);
            XMVECTOR rotation = XMVectorSet(q.v.x, q.v.y, q.v.z, q.s);
            const XMMATRIX scale = part.shape == 1
                ? XMMatrixScaling(part.half.x * 4.0f, part.half.y * 2.15f, part.half.z * 4.0f)
                : XMMatrixScaling(part.half.x * 2.08f, part.half.y * 2.08f, part.half.z * 2.08f);
            XMMATRIX transform = scale *
                XMMatrixRotationQuaternion(rotation) * XMMatrixTranslation((float)p.x, (float)p.y, (float)p.z);
            XMFLOAT4X4 stored; XMStoreFloat4x4(&stored, transform);
            // Unit primitives span +-0.5, so the drawn extent is half the scale
            // factor per axis; the sphere takes the largest and inflates 10%.
            const float scaleMax = part.shape == 1
                ? (std::max)({ part.half.x * 4.0f, part.half.y * 2.15f, part.half.z * 4.0f })
                : (std::max)({ part.half.x, part.half.y, part.half.z }) * 2.08f;
            ragdollRenderItems.push_back({ stored, part.color, part.shape,
                XMFLOAT3((float)p.x, (float)p.y, (float)p.z),
                scaleMax * 0.55f * 1.7321f });
        }

        enemyGunRenderItems.clear();
        if (enemyTargetValid) for (const HoverEnemy& enemy : hoverEnemies) {
            if (!enemy.alive || B3_IS_NULL(enemy.torso)) continue;
            const b3Pos bp = b3Body_GetPosition(enemy.torso);
            const XMVECTOR torso = XMVectorSet((float)bp.x, (float)bp.y, (float)bp.z, 1);
            XMVECTOR fwd = XMLoadFloat3(&enemyTarget) - torso;
            if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-5f) fwd = XMVectorSet(0, 0, 1, 0);
            fwd = XMVector3Normalize(fwd);
            const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, fwd));
            XMVECTOR up = XMVector3Normalize(XMVector3Cross(fwd, right));
            const RagdollPart& rightForearm = ragdollParts[enemy.firstPart + 6];
            const RagdollPart& leftForearm = ragdollParts[enemy.firstPart + 4];
            const b3Pos rh = b3Body_GetWorldPoint(rightForearm.body,
                { 0.0f, -rightForearm.half.y, 0.0f });
            const b3Pos lh = b3Body_GetWorldPoint(leftForearm.body,
                { 0.0f, -leftForearm.half.y, 0.0f });
            const XMVECTOR rightHand = XMVectorSet((float)rh.x, (float)rh.y, (float)rh.z, 1);
            const XMVECTOR leftHand = XMVectorSet((float)lh.x, (float)lh.y, (float)lh.z, 1);
            const XMVECTOR gunPos = rightHand - right * 0.10f + fwd * 0.08f;

            // Palms cover capsule tips and visibly contact trigger/foregrip.
            const XMFLOAT3 skin{ 0.62f, 0.39f, 0.27f };
            auto appendHand = [&](XMVECTOR handPos) {
                XMFLOAT4X4 handTransform;
                XMStoreFloat4x4(&handTransform,
                    XMMatrixScaling(0.16f, 0.13f, 0.13f) *
                    XMMatrixTranslationFromVector(handPos));
                ragdollRenderItems.push_back({ handTransform, skin, 2 });
            };
            appendHand(rightHand);
            appendHand(leftHand);
            XMMATRIX basis = XMMatrixIdentity();
            basis.r[0] = XMVectorSetW(right, 0); basis.r[1] = XMVectorSetW(up, 0);
            basis.r[2] = XMVectorSetW(fwd, 0); basis.r[3] = XMVectorSetW(gunPos, 1);
            XMFLOAT4X4 stored;
            XMStoreFloat4x4(&stored, XMMatrixScaling(0.65f, 0.65f, 0.65f) * basis);
            enemyGunRenderItems.push_back({ stored });
        }
    }

    bool FindNearestBreakableCell(const XMFLOAT3& worldPosition,
                                  ActorRuntime*& hitActor,
                                  uint32_t& hitChunk,
                                  bool includeSupport = false,
                                  bool includeSingle = false) {
        hitActor = nullptr;
        hitChunk = InvalidIndex;
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
        if (!includeSupport && chunks[hitChunk].support) return false;
        if (!includeSingle && hitActor->chunks.size() <= 1) return false;
        return true;
    }

    // Sever the bonds of the single non-support cell nearest `worldPosition`
    // so it splits off. Shared by bullet strikes and physics-impact damage.
    // Caller marks the structural graph dirty and rebuilds render items.
    bool BreakNearestCell(const XMFLOAT3& worldPosition,
                          bool includeSupport = false) {
        ActorRuntime* hitActor = nullptr;
        uint32_t hitChunk = InvalidIndex;
        if (!FindNearestBreakableCell(
                worldPosition, hitActor, hitChunk, includeSupport)) return false;
        lastBrokenStructure = chunks[hitChunk].structureId;
        if (hitActor->debrisCleanupEligible) {
            WakeDebris(*hitActor);
        }
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

    ActorRuntime* FindChunkOwner(uint32_t chunkIndex) const {
        for (const auto& runtime : actors) {
            if (std::find(runtime->chunks.begin(), runtime->chunks.end(),
                          chunkIndex) != runtime->chunks.end())
                return runtime.get();
        }
        return nullptr;
    }

    bool CreatePinnedHarpoonJoint(PinnedHarpoonRagdoll& pin) {
        if (pin.partIndex >= ragdollParts.size()) return false;
        RagdollPart& part = ragdollParts[pin.partIndex];
        ActorRuntime* ownerRuntime = pin.chunkIndex == InvalidIndex
            ? nullptr : FindChunkOwner(pin.chunkIndex);
        const b3BodyId targetBody = ownerRuntime
            ? ownerRuntime->body : pin.staticAnchorBody;
        if (B3_IS_NULL(targetBody) || B3_IS_NULL(part.body)) return false;
        if (!B3_IS_NULL(pin.joint) && b3Joint_IsValid(pin.joint)) return true;

        const b3Pos anchor = b3Body_GetWorldPoint(
            part.body, pin.ragdollLocalAnchor);
        b3Body_SetType(part.body, b3_dynamicBody);
        b3Body_SetLinearVelocity(part.body,
            b3Body_GetWorldPointVelocity(targetBody, anchor));
        b3Body_SetAngularVelocity(part.body,
            b3Body_GetAngularVelocity(targetBody));
        // Actor splits replace the target body and move its origin. Rebuild the
        // target frame from the wound's current world position so the pin stays
        // on the same visible chunk without snapping the corpse.
        const bool targetChanged = !B3_IS_NULL(pin.targetBody) &&
            !B3_ID_EQUALS(pin.targetBody, targetBody);
        if (!pin.targetFrameValid || targetChanged) {
            pin.targetLocalAnchor = b3Body_GetLocalPoint(targetBody, anchor);
            const b3Vec3 worldDirection = b3Body_GetWorldVector(
                part.body, pin.ragdollLocalDirection);
            pin.targetLocalDirection = b3Body_GetLocalVector(
                targetBody, worldDirection);
            pin.targetFrameValid = true;
        }
        pin.targetBody = targetBody;

        b3SphericalJointDef point = b3DefaultSphericalJointDef();
        point.base.bodyIdA = targetBody;
        point.base.bodyIdB = part.body;
        point.base.localFrameA.p = pin.targetLocalAnchor;
        point.base.localFrameB.p = pin.ragdollLocalAnchor;
        point.base.collideConnected = false;
        pin.joint = b3CreateSphericalJoint(world, &point);
        return !B3_IS_NULL(pin.joint) && b3Joint_IsValid(pin.joint);
    }

    void RefreshPinnedHarpoonJoints() {
        for (PinnedHarpoonRagdoll& pin : pinnedHarpoonRagdolls) {
            if (!B3_IS_NULL(pin.joint) && b3Joint_IsValid(pin.joint)) continue;
            pin.joint = b3_nullJointId;
            CreatePinnedHarpoonJoint(pin);
        }
    }

    int RagdollSolverSubsteps() const {
        if (!harpoonRagdolls.empty()) return 8;
        for (const PinnedHarpoonRagdoll& pin : pinnedHarpoonRagdolls) {
            if (pin.partIndex < ragdollParts.size() &&
                !B3_IS_NULL(ragdollParts[pin.partIndex].body) &&
                b3Body_IsAwake(ragdollParts[pin.partIndex].body)) return 8;
        }
        for (const RagdollPart& part : ragdollParts) {
            if (part.authoredId == InvalidIndex || B3_IS_NULL(part.body) ||
                !b3Body_IsAwake(part.body)) continue;
            if (!enemyTargetValid) return 6;
            const b3Pos p = b3Body_GetPosition(part.body);
            const float dx = (float)p.x - enemyTarget.x;
            const float dy = (float)p.y - enemyTarget.y;
            const float dz = (float)p.z - enemyTarget.z;
            if (dx*dx + dy*dy + dz*dz <= 25.0f*25.0f) return 6;
        }
        return 4;
    }

    bool ChunkWorldPosition(uint32_t chunkIndex, XMFLOAT3& position) const {
        if (chunkIndex >= chunks.size()) return false;
        const ActorRuntime* runtime = FindChunkOwner(chunkIndex);
        if (!runtime || B3_IS_NULL(runtime->body)) return false;
        XMStoreFloat3(&position, XMVector3TransformCoord(
            XMLoadFloat3(&chunks[chunkIndex].center),
            BoxTransform(runtime->body, runtime->center)));
        return true;
    }

    void IgniteChunk(uint32_t chunkIndex, float life = 3.0f) {
        for (BurningChunk& burning : burningChunks) {
            if (burning.chunkIndex != chunkIndex) continue;
            burning.life = (std::max)(burning.life, life);
            return;
        }
        if (burningChunks.size() >= 72) burningChunks.erase(burningChunks.begin());
        burningChunks.push_back({ chunkIndex, life, 0.25f, 0.45f });
    }

    bool UpdateBurningChunks(float dt) {
        if (burningChunks.empty()) return false;
        std::vector<uint32_t> spread;
        std::vector<XMFLOAT3> damagePoints;
        std::unordered_set<uint32_t> alreadyBurning;
        for (const BurningChunk& burning : burningChunks)
            alreadyBurning.insert(burning.chunkIndex);

        for (BurningChunk& burning : burningChunks) {
            burning.life -= dt;
            burning.spreadCooldown -= dt;
            burning.damageCooldown -= dt;
            XMFLOAT3 source;
            if (burning.life <= 0.0f ||
                !ChunkWorldPosition(burning.chunkIndex, source)) continue;

            if (burning.damageCooldown <= 0.0f) {
                burning.damageCooldown = 0.85f;
                if (damagePoints.size() < 3) damagePoints.push_back(source);
            }
            if (burning.spreadCooldown > 0.0f || spread.size() >= 4) continue;
            burning.spreadCooldown = 0.55f;

            // World-space lookup makes fire carried by moving debris ignite any
            // destructible piece it passes, including pieces from other buildings.
            float closestDistanceSquared = 1.75f * 1.75f;
            uint32_t closest = InvalidIndex;
            for (uint32_t candidate = 0;
                 candidate < static_cast<uint32_t>(chunks.size()); ++candidate) {
                if (candidate == burning.chunkIndex ||
                    alreadyBurning.count(candidate) != 0) continue;
                XMFLOAT3 candidatePosition;
                if (!ChunkWorldPosition(candidate, candidatePosition)) continue;
                const float dx = candidatePosition.x - source.x;
                const float dy = candidatePosition.y - source.y;
                const float dz = candidatePosition.z - source.z;
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (distanceSquared >= closestDistanceSquared) continue;
                closestDistanceSquared = distanceSquared;
                closest = candidate;
            }
            if (closest != InvalidIndex) {
                spread.push_back(closest);
                alreadyBurning.insert(closest);
            }
        }
        for (uint32_t chunkIndex : spread) IgniteChunk(chunkIndex, 3.0f);
        burningChunks.erase(
            std::remove_if(burningChunks.begin(), burningChunks.end(),
                [this](const BurningChunk& burning) {
                    XMFLOAT3 ignored;
                    return burning.life <= 0.0f ||
                        !ChunkWorldPosition(burning.chunkIndex, ignored);
                }),
            burningChunks.end());

        bool broke = false;
        for (const XMFLOAT3& point : damagePoints) {
            if (!BreakNearestCell(point, true)) continue;
            MarkStructureDirty(lastBrokenStructure);
            broke = true;
        }
        return broke;
    }

    void MarkStructureDirty(uint32_t structureId, float delay = 0.15f) {
        if (structureId == InvalidIndex) return;
        const float ready = structuralClock + delay;
        if (dirtyStructureSet.insert(structureId).second) {
            dirtyStructures.push_back({ structureId, ready });
        } else {
            for (DirtyStructure& entry : dirtyStructures) {
                if (entry.id == structureId) {
                    entry.readyTime = (std::max)(entry.readyTime, ready);
                    break;
                }
            }
        }
    }

    // One conservative connectivity relaxation pass. Damage marks the graph
    // dirty; Update runs at most one pass per 15 Hz slice. Stable structures do
    // no solver work, and cascades naturally acquire a short physical delay.
    bool DropUnderConnectedPass(uint32_t structureId) {
        // Drop a piece only once it has NO live bonds left. Using a higher
        // threshold cascades: isolating the struck chunk drops its neighbours to
        // one bond, which would then fall too, chaining across the whole wall.
        constexpr uint32_t kMinBonds = 1;
        if (!asset) return false;
        const uint32_t assetBondCount = asset->getBondCount();
        bool anyMarked = false;
            // damage() defers the fracture until group->process(), so the per-
            // actor break mask and params must outlive this loop. Keep them in
            // stable storage (deque never reallocates its elements).
            std::list<std::vector<uint8_t>> masks;
            std::list<IsolateChunksParams> paramStore;
            std::list<IsolateGroupParams> groupParamStore;
            for (auto& runtime : actors) {
                if (!runtime->actor) continue;
                const NvBlastActor* ll = runtime->actor->getActorLL();
                if (!ll) continue;
                const float* bondHealths = NvBlastActorGetBondHealths(ll, nullptr);
                if (!bondHealths) continue;

                std::vector<uint8_t> owned(chunks.size(), 0);
                for (uint32_t chunkIndex : runtime->chunks)
                    owned[chunkIndex] = 1;
                // Count this actor's live bonds per chunk.
                std::unordered_map<uint32_t, uint32_t> liveBonds;  // chunkIndex(0-based) -> count
                std::unordered_map<int, uint32_t> externalGroupBonds;
                std::unordered_set<int> actorGroups;
                for (uint32_t chunkIndex : runtime->chunks) {
                    const int plankGroup = chunks[chunkIndex].plankGroup;
                    if (plankGroup >= 0) actorGroups.insert(plankGroup);
                }
                for (uint32_t bp = 0; bp < bondPairs.size() && bp < assetBondCount; ++bp) {
                    if (bondHealths[bp] <= 0.0f) continue;
                    const uint32_t ca = bondPairs[bp].a, cb = bondPairs[bp].b;
                    const bool ownsA = ca < owned.size() && owned[ca] != 0;
                    const bool ownsB = cb < owned.size() && owned[cb] != 0;
                    if (ownsA && ownsB) {
                        ++liveBonds[ca]; ++liveBonds[cb];
                        const int groupA = chunks[ca].plankGroup;
                        const int groupB = chunks[cb].plankGroup;
                        if (groupA >= 0 && groupA != groupB) ++externalGroupBonds[groupA];
                        if (groupB >= 0 && groupB != groupA) ++externalGroupBonds[groupB];
                    }
                }

                // A multi-cell board remains internally bonded so it can fall
                // intact. Once fewer than two bonds connect it to the standing
                // structure, sever only those external bonds. This prevents a
                // large roof panel or plank from hovering rigidly by one point.
                for (int plankGroup : actorGroups) {
                    bool hasOtherChunks = false;
                    for (uint32_t chunkIndex : runtime->chunks) {
                        if (chunks[chunkIndex].plankGroup != plankGroup) {
                            hasOtherChunks = true;
                            break;
                        }
                    }
                    if (!hasOtherChunks || externalGroupBonds[plankGroup] >= 2) continue;
                    groupParamStore.push_back({ chunkGroupByAsset.data(),
                        static_cast<uint32_t>(chunkGroupByAsset.size()), plankGroup });
                    const NvBlastDamageProgram groupProgram = { IsolateGroupShader, nullptr };
                    runtime->actor->damage(groupProgram, &groupParamStore.back());
                    anyMarked = true;
                }

                std::vector<uint8_t> mask(chunks.size() + 1, 0);  // asset chunk index; 0 = root
                bool actorMarked = false;
                for (uint32_t chunkIndex : runtime->chunks) {
                    if (chunks[chunkIndex].structureId != structureId) continue;
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
        if (anyMarked)
            group->process();  // next dirty slice evaluates the new islands
        return anyMarked;
    }

    bool UpdateStructuralSolver(float dt) {
        structuralClock += dt;
        if (dirtyStructures.empty()) return false;
        structuralAccumulator += dt;
        if (structuralAccumulator < StructuralSolverStep) return false;
        structuralAccumulator = std::fmod(structuralAccumulator,
            StructuralSolverStep);
        const auto begin = std::chrono::steady_clock::now();
        bool anyBroke = false;
        uint32_t processed = 0;
        const size_t available = dirtyStructures.size();
        for (size_t scanned = 0; scanned < available &&
             processed < MaxStructuresPerSolverSlice; ++scanned) {
            DirtyStructure entry = dirtyStructures.front();
            dirtyStructures.pop_front();
            if (entry.readyTime > structuralClock) {
                dirtyStructures.push_back(entry);
                continue;
            }
            const bool broke = DropUnderConnectedPass(entry.id);
            anyBroke = anyBroke || broke;
            ++processed;
            if (broke) {
                entry.readyTime = structuralClock + StructuralSolverStep;
                dirtyStructures.push_back(entry);
            } else {
                dirtyStructureSet.erase(entry.id);
            }
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();
            if (elapsed >= StructuralSolverBudgetMs) break;
        }
        return anyBroke;
    }

    // Bound active physics cost without deleting aftermath. Old slow debris is
    // put to sleep and later spatially batched, but its geometry remains forever.
    bool EnforceDebrisBudget() {
        std::vector<ActorRuntime*> debris;
        std::vector<ActorRuntime*> slowAwake;
        debris.reserve(actors.size());
        for (const auto& runtime : actors) {
            if (!runtime->debrisCleanupEligible || !runtime->dynamic ||
                B3_IS_NULL(runtime->body)) continue;
            debris.push_back(runtime.get());
            if (!b3Body_IsAwake(runtime->body) || runtime->debrisAge < 1.0f)
                continue;
            const b3Vec3 velocity = b3Body_GetLinearVelocity(runtime->body);
            const float speedSq = velocity.x * velocity.x +
                velocity.y * velocity.y + velocity.z * velocity.z;
            if (speedSq < 1.0f) slowAwake.push_back(runtime.get());
        }

        uint32_t awakeCount = 0;
        for (ActorRuntime* runtime : debris)
            awakeCount += b3Body_IsAwake(runtime->body) ? 1u : 0u;
        std::sort(slowAwake.begin(), slowAwake.end(),
            [](const ActorRuntime* a, const ActorRuntime* b) {
                return a->debrisAge > b->debrisAge;
            });
        bool changed = false;
        for (ActorRuntime* runtime : slowAwake) {
            if (awakeCount <= MaxAwakeDebrisBodies) break;
            b3Body_SetAwake(runtime->body, false);
            runtime->restTime = 0.0f;
            --awakeCount;
            changed = true;
        }

        return changed;
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
    m->initialBatchBuild = false;
    std::cout << "Destruction ready: " << m->chunks.size() << " chunks, "
              << m->asset->getBondCount() << " bonds\n";
    return true;
}

void DestructionDX12::Shutdown() {
    if (!m) return;
    if (m->spatialBatchBuildInFlight) {
        m->spatialBatchBuildFuture.wait();
        m->spatialBatchBuildInFlight = false;
    }
    if (m->batchBuildInFlight) {
        m->batchBuildFuture.wait();
        m->batchBuildInFlight = false;
    }
    if (!B3_IS_NULL(m->world)) b3DestroyWorld(m->world);
    m->world = b3_nullWorldId;
    if (m->terrainHeightField) {
        b3DestroyHeightField(m->terrainHeightField);
        m->terrainHeightField = nullptr;
    }
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
    m->chunks.clear(); m->renderItems.clear(); m->renderBatches.clear();
    m->batchCache.clear(); m->spatialBatchCache.clear(); m->ragdollParts.clear();
    m->barrelBodies.clear(); m->barrelImpactEvents.clear(); m->vortices.clear();
    m->burningChunks.clear(); m->harpoonRagdolls.clear();
    m->pinnedHarpoonRagdolls.clear();
    for (auto& retired : m->retiredBatchNodes) retired.clear();
    m->authoredRagdolls.clear();
    m->ragdollRenderItems.clear(); m->initialized = false;
}

void DestructionDX12::Reset() {
    if (!m || !m->source) return;
    auto source = m->source; ID3D12Device* device = m->device;
    const int x = m->gridX, y = m->gridY, z = m->gridZ;
    Initialize(source, device, x, y, z);
}

bool DestructionDX12::InitializeVehicle(const XMFLOAT3& chassisCenter,
                                        float yawRadians) {
    if (!m || !m->initialized || B3_IS_NULL(m->world) ||
        !B3_IS_NULL(m->vehicleChassis)) return false;

    b3BodyDef chassisDef = b3DefaultBodyDef();
    chassisDef.type = b3_dynamicBody;
    chassisDef.position = {
        chassisCenter.x, chassisCenter.y, chassisCenter.z };
    const b3Quat yawRotation = b3MakeQuatFromAxisAngle(b3Vec3_axisY, yawRadians);
    chassisDef.rotation = yawRotation;
    chassisDef.linearDamping = 0.12f;
    chassisDef.angularDamping = 0.65f;
    m->vehicleChassis = b3CreateBody(m->world, &chassisDef);
    b3ShapeDef chassisShape = b3DefaultShapeDef();
    chassisShape.density = 110.0f;
    chassisShape.baseMaterial.friction = 0.75f;
    chassisShape.baseMaterial.restitution = 0.02f;
    chassisShape.enableHitEvents = true;
    chassisShape.filter.categoryBits = CollisionCategoryVehicle;
    chassisShape.filter.maskBits = UINT64_MAX;
    b3BoxHull chassisHull = b3MakeBoxHull(2.20f, 0.55f, 1.0f);
    b3CreateHullShape(m->vehicleChassis, &chassisShape, &chassisHull.base);

    // A soft parallel constraint resists catastrophic rollovers while still
    // allowing pitch/roll from suspension and terrain.
    if (!B3_IS_NULL(m->ground)) {
        b3ParallelJointDef upright = b3DefaultParallelJointDef();
        upright.base.bodyIdA = m->ground;
        upright.base.bodyIdB = m->vehicleChassis;
        upright.base.localFrameA.q =
            b3ComputeQuatBetweenUnitVectors(b3Vec3_axisZ, b3Vec3_axisY);
        upright.base.localFrameB.q =
            b3ComputeQuatBetweenUnitVectors(b3Vec3_axisZ, b3Vec3_axisY);
        upright.base.collideConnected = true;
        upright.hertz = 0.65f;
        upright.dampingRatio = 1.0f;
        b3CreateParallelJoint(m->world, &upright);
    }

    b3BodyDef wheelBodyDef = b3DefaultBodyDef();
    wheelBodyDef.type = b3_dynamicBody;
    wheelBodyDef.allowFastRotation = true;
    wheelBodyDef.rotation = b3MulQuat(yawRotation,
        b3ComputeQuatBetweenUnitVectors(b3Vec3_axisY, b3Vec3_axisZ));
    b3ShapeDef wheelShape = b3DefaultShapeDef();
    wheelShape.density = 65.0f;
    wheelShape.baseMaterial.friction = 4.0f;
    wheelShape.baseMaterial.restitution = 0.01f;
    wheelShape.filter.categoryBits = CollisionCategoryVehicle;
    wheelShape.filter.maskBits = UINT64_MAX;
    b3Sphere wheelSphere = { b3Vec3_zero, 0.48f };

    b3WheelJointDef joint = b3DefaultWheelJointDef();
    joint.base.bodyIdA = m->vehicleChassis;
    joint.base.localFrameA.q =
        b3ComputeQuatBetweenUnitVectors(b3Vec3_axisX, b3Vec3_axisY);
    joint.base.localFrameB.q =
        b3ComputeQuatBetweenUnitVectors(b3Vec3_axisZ, b3Vec3_axisY);
    joint.enableSuspensionLimit = true;
    joint.lowerSuspensionLimit = -0.34f;
    joint.upperSuspensionLimit = 0.28f;
    joint.enableSuspensionSpring = true;
    joint.suspensionHertz = 3.2f;
    joint.suspensionDampingRatio = 0.92f;
    joint.steeringHertz = 4.0f;
    joint.steeringDampingRatio = 0.9f;
    joint.maxSteeringTorque = 850.0f;
    joint.enableSteeringLimit = true;
    joint.lowerSteeringLimit = -0.58f;
    joint.upperSteeringLimit = 0.58f;

    const XMFLOAT3 wheelOffsets[4] = {
        { 1.55f, -0.58f,  0.92f }, { 1.55f, -0.58f, -0.92f },
        {-1.55f, -0.58f,  0.92f }, {-1.55f, -0.58f, -0.92f },
    };
    for (size_t i = 0; i < m->vehicleWheels.size(); ++i) {
        const XMFLOAT3& offset = wheelOffsets[i];
        const b3Vec3 rotatedOffset = b3RotateVector(yawRotation,
            { offset.x, offset.y, offset.z });
        wheelBodyDef.position = {
            chassisCenter.x + rotatedOffset.x,
            chassisCenter.y + rotatedOffset.y,
            chassisCenter.z + rotatedOffset.z };
        m->vehicleWheels[i] = b3CreateBody(m->world, &wheelBodyDef);
        b3CreateSphereShape(m->vehicleWheels[i], &wheelShape, &wheelSphere);
        joint.base.bodyIdB = m->vehicleWheels[i];
        joint.base.localFrameA.p = { offset.x, offset.y, offset.z };
        joint.enableSteering = i < 2;
        joint.enableSpinMotor = true;
        joint.maxSpinTorque = 520.0f;
        joint.spinSpeed = 0.0f;
        joint.targetSteeringAngle = 0.0f;
        m->vehicleJoints[i] = b3CreateWheelJoint(m->world, &joint);
    }
    return true;
}

void DestructionDX12::SetVehicleInput(float throttle, float steering, bool brake) {
    if (!VehicleReady()) return;
    throttle = (std::max)(-1.0f, (std::min)(1.0f, throttle));
    steering = (std::max)(-1.0f, (std::min)(1.0f, steering));
    b3Body_SetAwake(m->vehicleChassis, true);

    const float steeringAngle = steering * 0.42f;
    for (size_t i = 0; i < 4; ++i) {
        if (i < 2)
            b3WheelJoint_SetTargetSteeringAngle(m->vehicleJoints[i], steeringAngle);
        b3WheelJoint_EnableSpinMotor(m->vehicleJoints[i], true);
        b3WheelJoint_SetSpinMotorSpeed(
            m->vehicleJoints[i], brake ? 0.0f : 20.0f * throttle);
        const float torque = brake ? 1400.0f :
            (std::abs(throttle) > 0.01f ? 520.0f : 95.0f);
        b3WheelJoint_SetMaxSpinTorque(m->vehicleJoints[i], torque);
    }
}

bool DestructionDX12::GetVehicleTransform(XMFLOAT4X4& transform,
                                          XMFLOAT3* position,
                                          XMFLOAT3* forward,
                                          XMFLOAT3* linearVelocity) const {
    if (!VehicleReady()) return false;
    const XMMATRIX world = BoxTransform(m->vehicleChassis, { 0.0f, 0.0f, 0.0f });
    XMStoreFloat4x4(&transform, world);
    const b3Pos p = b3Body_GetPosition(m->vehicleChassis);
    if (position) *position = { (float)p.x, (float)p.y, (float)p.z };
    if (forward) {
        XMVECTOR direction = XMVector3Normalize(XMVector3TransformNormal(
            XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), world));
        XMStoreFloat3(forward, direction);
    }
    if (linearVelocity) {
        const b3Vec3 velocity = b3Body_GetLinearVelocity(m->vehicleChassis);
        *linearVelocity = { velocity.x, velocity.y, velocity.z };
    }
    return true;
}

bool DestructionDX12::VehicleReady() const {
    return m && m->initialized && !B3_IS_NULL(m->vehicleChassis);
}

void DestructionDX12::Update(float dt) {
    if (!m->initialized) return;
    const auto updateBegin = std::chrono::steady_clock::now();
    const bool batchCompleted = m->PollBatchBuild();
    const bool spatialBatchCompleted = m->PollSpatialBatchBuild();
    const bool structuralBroke = m->UpdateStructuralSolver(dt);
    const bool fireBroke = m->UpdateBurningChunks(dt);
    const bool heavyDestructionScene = m->actors.size() > 512;
    const float maintenanceStep = 1.0f / 30.0f;
    bool maintenanceDue = true;
    float maintenanceDt = dt;
    if (heavyDestructionScene) {
        m->maintenanceAccumulator = std::min(
            0.25f, m->maintenanceAccumulator + dt);
        const int maintenanceSteps = static_cast<int>(
            m->maintenanceAccumulator / maintenanceStep);
        maintenanceDue = maintenanceSteps > 0;
        maintenanceDt = maintenanceSteps * maintenanceStep;
        if (maintenanceDue)
            m->maintenanceAccumulator -= maintenanceDt;
    } else {
        m->maintenanceAccumulator = 0.0f;
    }
    for (auto it = m->actors.begin(); maintenanceDue && it != m->actors.end();) {
        Impl::ActorRuntime& runtime = **it;
        const bool renderedAsBatch =
            m->batchCache.find(&runtime) != m->batchCache.end();
        // Age still drives cheaper collision for old debris. Visual geometry is
        // never scaled or expired.
        if (!runtime.dynamic || !runtime.debrisCleanupEligible || renderedAsBatch) {
            if (renderedAsBatch) runtime.debrisAge = 0.0f;
            ++it;
            continue;
        }
        runtime.debrisAge += maintenanceDt;
        if (!runtime.collisionLod &&
            (runtime.debrisAge >= DebrisCollisionLodAge ||
             m->ActorVolume(runtime) <= DebrisCollisionLodVolume))
            m->SetDebrisCollisionLod(runtime, true);
        ++it;
    }
    m->accumulator = std::min(0.25f, m->accumulator + dt);
    // Thousands of settled split actors make a 60 Hz broadphase needlessly
    // expensive. Heavy destruction scenes use a stable 30 Hz fixed step;
    // rendering remains frame-rate independent and awake debris still receives
    // every accumulated step.
    const float step = heavyDestructionScene
        ? (1.0f / 30.0f) : (1.0f / 60.0f);
    bool anyImpactBroke = false;
    bool physicsStepped = false;
    const auto physicsBegin = std::chrono::steady_clock::now();
    while (m->accumulator >= step) {
        physicsStepped = true;
        m->RefreshPinnedHarpoonJoints();
        m->ApplyWaterBuoyancy();
        m->ApplyVortices(step);
        m->ApplyEnemyHover(step);
        m->RelaxAuthoredRagdolls(step);
        m->ApplyAnatomicalResistance();
        m->UpdateHarpoonAttachments(step);
        b3World_Step(m->world, step, m->RagdollSolverSubsteps());
        m->accumulator -= step;
        // Physics impact damage: collisions above the world's hit-event speed
        // threshold (debris slamming the house, pieces crashing onto the
        // ground) fracture the cell they land on, same as a bullet strike.
        const b3ContactEvents events = b3World_GetContactEvents(m->world);
        // Splitting an actor destroys its Box3D body. Copy event points before
        // doing that because Box3D owns the contact-event buffer.
        std::vector<XMFLOAT3> hitPoints;
        hitPoints.reserve(events.hitCount);
        for (int i = 0; i < events.hitCount; ++i) {
            const b3ContactHitEvent& hit = events.hitEvents[i];
            const b3Vec3& point = hit.point;
            const b3Filter filterA = b3Shape_GetFilter(hit.shapeIdA);
            const b3Filter filterB = b3Shape_GetFilter(hit.shapeIdB);
            const uint64_t debrisMask = PhysicsImpactPolicy::FractureDealerMask;
            const bool involvesDebris =
                (filterA.categoryBits & debrisMask) != 0 ||
                (filterB.categoryBits & debrisMask) != 0;
            // Ragdolls remain solid and can hurt enemies, but never enter the
            // Blast bond-damage path. Only explicitly destructive categories
            // contribute fracture points.
            if (PhysicsImpactPolicy::CanFracture(
                    filterA.categoryBits, filterB.categoryBits))
                hitPoints.emplace_back((float)point.x, (float)point.y,
                                       (float)point.z);
            if (involvesDebris && hit.approachSpeed >= 3.0f &&
                m->collisionSoundEvents.size() < 32) {
                m->collisionSoundEvents.push_back({
                    { (float)point.x, (float)point.y, (float)point.z },
                    hit.approachSpeed });
            }
            if (hit.approachSpeed >= 4.0f) {
                const b3BodyId bodyA = b3Shape_GetBody(hit.shapeIdA);
                const b3BodyId bodyB = b3Shape_GetBody(hit.shapeIdB);
                for (const Impl::BarrelRuntime& barrel : m->barrelBodies) {
                    if (!B3_ID_EQUALS(barrel.body, bodyA) &&
                        !B3_ID_EQUALS(barrel.body, bodyB)) continue;
                    if (std::find(m->barrelImpactEvents.begin(),
                                  m->barrelImpactEvents.end(), barrel.handle) ==
                        m->barrelImpactEvents.end())
                        m->barrelImpactEvents.push_back(barrel.handle);
                }
            }
        }
        int budget = 2;  // low impact damage: at most two cells per step
        for (const XMFLOAT3& point : hitPoints) {
            if (budget <= 0) break;
            if (m->BreakNearestCell(point)) {
                --budget;
                anyImpactBroke = true;
                m->MarkStructureDirty(m->lastBrokenStructure);
            }
        }
    }
    const double physicsMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - physicsBegin).count();
    // Dense debris piles can jitter below visible motion forever and Box3D then
    // keeps thousands of tiny actors awake. Promote genuinely low-energy pieces
    // to sleep so they enter spatial render batches. Any later contact/impulse
    // wakes the body normally and removes it from its cached cell next rebuild.
    bool sleepStateChanged = false;
    for (const auto& runtime : m->actors) {
        if (!maintenanceDue) break;
        if (!runtime->dynamic || B3_IS_NULL(runtime->body)) {
            runtime->restTime = 0.0f;
            continue;
        }
        if (runtime->frozen) continue;
        if (!b3Body_IsAwake(runtime->body)) {
            runtime->restTime = 0.0f;
            runtime->settledTime += maintenanceDt;
            if (runtime->settledTime >= SettledPileFreezeSeconds) {
                // Static bodies do not form solver islands with one another.
                // Existing spatial render batching merges their visible pile.
                b3Body_SetType(runtime->body, b3_staticBody);
                runtime->frozen = true;
                m->SetDebrisCollisionLod(*runtime, true);
                sleepStateChanged = true;
            }
            continue;
        }
        runtime->settledTime = 0.0f;
        const b3Pos position = b3Body_GetPosition(runtime->body);
        if (m->waterEnabled && m->InWaterColumn(position)) {
            runtime->restTime = 0.0f;
            continue;
        }
        const b3Vec3 linear = b3Body_GetLinearVelocity(runtime->body);
        const b3Vec3 angular = b3Body_GetAngularVelocity(runtime->body);
        const float linearSq = linear.x * linear.x + linear.y * linear.y +
            linear.z * linear.z;
        const float angularSq = angular.x * angular.x + angular.y * angular.y +
            angular.z * angular.z;
        if (linearSq < 0.01f && angularSq < 0.04f) {
            runtime->restTime += maintenanceDt;
            if (runtime->restTime >= 2.0f) {
                b3Body_SetAwake(runtime->body, false);
                runtime->restTime = 0.0f;
                sleepStateChanged = true;
            }
        } else {
            runtime->restTime = 0.0f;
        }
    }
    const bool debrisBudgetChanged = maintenanceDue &&
        m->EnforceDebrisBudget();
    m->UpdateEnemyFire(dt);

    // Only rebuild when something could actually have moved. An intact house is
    // fully static, so this walk over ~588 chunks was pure waste on every frame
    // before the player breaks anything.
    //
    // The `rebuiltWhileStill` latch is what makes it safe: when the last dynamic
    // piece settles back to static, AnythingMoving() flips to false, and without
    // one final rebuild the render items would be frozen at wherever the debris was
    // a frame before it came to rest.
    const bool motionStateChanged = physicsStepped || sleepStateChanged ||
        anyImpactBroke || structuralBroke || fireBroke || debrisBudgetChanged;
    const bool moving = motionStateChanged
        ? m->AnythingMoving() : !m->rebuiltWhileStill;
    const bool movingGeometryChanged = physicsStepped && moving;
    const bool finalSettleRebuild = motionStateChanged && !moving &&
        !m->rebuiltWhileStill;
    if (movingGeometryChanged || finalSettleRebuild || anyImpactBroke ||
        structuralBroke || fireBroke || debrisBudgetChanged ||
        batchCompleted || spatialBatchCompleted) {
        const auto rebuildBegin = std::chrono::steady_clock::now();
        m->RebuildRenderItems();
        const double rebuildMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - rebuildBegin).count();
        if (m->stressStats.running)
            m->stressStats.peakRenderRebuildMilliseconds = (std::max)(
                m->stressStats.peakRenderRebuildMilliseconds, rebuildMilliseconds);
        m->rebuiltWhileStill = !moving;
    }
    if (m->stressStats.running) {
        const double updateMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - updateBegin).count();
        m->stressStats.elapsedSeconds += dt;
        ++m->stressStats.sampledFrames;
        const double frameMilliseconds = static_cast<double>(dt) * 1000.0;
        m->stressStats.averageFrameMilliseconds =
            static_cast<double>(m->stressStats.elapsedSeconds) * 1000.0 /
            static_cast<double>(m->stressStats.sampledFrames);
        m->stressStats.peakFrameMilliseconds = (std::max)(
            m->stressStats.peakFrameMilliseconds, frameMilliseconds);
        m->stressUpdateTotalMs += updateMilliseconds;
        m->stressStats.averageUpdateMilliseconds = m->stressUpdateTotalMs /
            static_cast<double>(m->stressStats.sampledFrames);
        m->stressStats.peakUpdateMilliseconds = (std::max)(
            m->stressStats.peakUpdateMilliseconds, updateMilliseconds);
        m->stressStats.peakPhysicsMilliseconds = (std::max)(
            m->stressStats.peakPhysicsMilliseconds, physicsMilliseconds);
        m->stressStats.peakActors = (std::max)(m->stressStats.peakActors,
            static_cast<uint32_t>(m->actors.size()));
        uint32_t awake = 0, lod = 0, frozen = 0;
        for (const auto& runtime : m->actors) {
            awake += runtime->dynamic && !B3_IS_NULL(runtime->body) &&
                b3Body_IsAwake(runtime->body) ? 1u : 0u;
            lod += runtime->collisionLod ? 1u : 0u;
            frozen += runtime->frozen ? 1u : 0u;
        }
        m->stressStats.peakAwakeActors = (std::max)(
            m->stressStats.peakAwakeActors, awake);
        m->stressStats.collisionLodBodies = lod;
        m->stressStats.frozenBodies = frozen;
        m->stressStats.renderRebuilds = m->renderItemRebuildCount -
            m->stressStartRebuilds;
        if (m->stressStats.elapsedSeconds >= 8.0f ||
            (m->stressStats.elapsedSeconds >= 1.0f && awake == 0))
            m->stressStats.running = false;
    }
}

void DestructionDX12::SetEnemyTarget(const XMFLOAT3& target) {
    if (!m) return;
    m->enemyTarget = target;
    m->enemyTargetValid = true;
}

std::vector<EnemyShot> DestructionDX12::DrainEnemyShots() {
    if (!m) return {};
    std::vector<EnemyShot> result;
    result.swap(m->pendingEnemyShots);
    return result;
}

uint32_t DestructionDX12::SpawnAuthoredRagdoll(
    const std::vector<AuthoredRagdollBody>& bodies,
    const std::vector<RagdollConstraintSpec>& constraints,
    const RagdollImpact& impact) {
    if (!m || !m->initialized || B3_IS_NULL(m->world) || bodies.empty()) return InvalidIndex;
    const uint32_t ragdollId = m->nextAuthoredRagdollId++;
    const size_t base = m->ragdollParts.size();
    std::unordered_map<std::string, size_t> indices;
    const XMFLOAT3 cloth{ 0.08f, 0.09f, 0.075f };
    for (size_t i = 0; i < bodies.size(); ++i) {
        const AuthoredRagdollBody& src = bodies[i];
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = { src.position.x, src.position.y, src.position.z };
        bd.rotation = ToB3Quat(src.rotation);
        bd.linearVelocity = { src.linearVelocity.x, src.linearVelocity.y,
                              src.linearVelocity.z };
        bd.angularVelocity = { src.angularVelocity.x, src.angularVelocity.y,
                               src.angularVelocity.z };
        bd.linearDamping = 0.08f;
        bd.angularDamping = 0.35f;
        bd.sleepThreshold = 0.08f;
        bd.enableContactRecycling = false;
        const b3BodyId body = b3CreateBody(m->world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = 1000.0f;
        sd.baseMaterial.friction = 0.65f;
        sd.baseMaterial.restitution = 0.0f;
        sd.filter.categoryBits = CollisionCategoryRagdoll;
        sd.filter.maskBits = UINT64_MAX;
        sd.filter.groupIndex = PhysicsImpactPolicy::RagdollCollisionGroup(
            ragdollId);
        sd.enableHitEvents = true;
        for (const RagdollShapeSpec& shape : src.shapes) {
            const b3Quat shapeRotation = ToB3Quat(shape.rotation);
            const b3Vec3 center = { shape.center.x, shape.center.y, shape.center.z };
            if (shape.type == RagdollShapeType::Capsule) {
                const b3Vec3 axis = b3RotateVector(shapeRotation, { 0,1,0 });
                b3Capsule capsule = {};
                capsule.center1 = b3MulSub(center, shape.length * 0.5f, axis);
                capsule.center2 = b3MulAdd(center, shape.length * 0.5f, axis);
                capsule.radius = shape.radius;
                b3CreateCapsuleShape(body, &sd, &capsule);
            } else if (shape.type == RagdollShapeType::Sphere) {
                const b3Sphere sphere = { center, shape.radius };
                b3CreateSphereShape(body, &sd, &sphere);
            } else {
                const b3Transform transform = { center, shapeRotation };
                b3BoxHull box = b3MakeTransformedBoxHull(
                    shape.halfExtent.x, shape.halfExtent.y,
                    shape.halfExtent.z, transform);
                b3CreateHullShape(body, &sd, &box.base);
            }
        }
        b3MassData mass = b3Body_GetMassData(body);
        if (mass.mass > 1e-5f) {
            const float scale = std::max(0.05f, src.targetMass) / mass.mass;
            mass.mass *= scale;
            mass.inertia.cx = b3MulSV(scale, mass.inertia.cx);
            mass.inertia.cy = b3MulSV(scale, mass.inertia.cy);
            mass.inertia.cz = b3MulSV(scale, mass.inertia.cz);
            b3Body_SetMassData(body, mass);
        }
        m->ragdollParts.push_back({ body, RagdollBodyHalfExtent(src), cloth, 1,
                                    false, ragdollId, src.name,
                                    impact.lethalHazard });
        indices[src.name] = i;
    }
    Impl::AuthoredRagdollRuntime runtime;
    runtime.id = ragdollId;
    runtime.muscleTime = 0.20f;
    runtime.joints.reserve(constraints.size());
    for (const RagdollConstraintSpec& link : constraints) {
        auto a = indices.find(link.boneA), b = indices.find(link.boneB);
        if (a == indices.end() || b == indices.end()) continue;
        const b3BodyId bodyA = m->ragdollParts[base + a->second].body;
        const b3BodyId bodyB = m->ragdollParts[base + b->second].body;
        const b3Transform frameA = {
            { link.frameA.position.x, link.frameA.position.y, link.frameA.position.z },
            RagdollFrameRotation(link.frameA, link.jointType, link.hingeAxis) };
        const b3Transform frameB = {
            { link.frameB.position.x, link.frameB.position.y, link.frameB.position.z },
            RagdollFrameRotation(link.frameB, link.jointType, link.hingeAxis) };
        const float passiveTorque = PassiveJointTorque(link.boneA);
        if (link.jointType == RagdollJointType::Hinge) {
            b3RevoluteJointDef jd = b3DefaultRevoluteJointDef();
            jd.base.bodyIdA = bodyA; jd.base.bodyIdB = bodyB;
            jd.base.localFrameA = frameA; jd.base.localFrameB = frameB;
            jd.base.collideConnected = false;
            jd.enableLimit = true;
            jd.lowerAngle = link.lowerHingeAngle;
            jd.upperAngle = link.upperHingeAngle;
            jd.enableSpring = true;
            jd.hertz = 4.0f;
            jd.dampingRatio = 1.0f;
            const b3JointId joint = b3CreateRevoluteJoint(m->world, &jd);
            if (!B3_IS_NULL(joint)) {
                b3RevoluteJoint_SetTargetAngle(joint,
                    b3RevoluteJoint_GetAngle(joint));
                runtime.joints.push_back({ joint, link.jointType, passiveTorque });
            }
        } else {
            // T3D often omits one side's orientation frame. Center Box3D's
            // angular limits on the actual death pose so the cone solver cannot
            // snap thighs (or other limbs) toward an unrelated bind-pose frame.
            const b3Quat worldA = b3MulQuat(
                b3Body_GetRotation(bodyA), frameA.q);
            const b3Quat worldB = b3MulQuat(
                b3Body_GetRotation(bodyB), frameB.q);
            const b3Quat spawnRelative = b3NormalizeQuat(
                b3InvMulQuat(worldA, worldB));
            b3Transform centeredFrameB = frameB;
            centeredFrameB.q = b3NormalizeQuat(b3MulQuat(
                frameB.q, b3Conjugate(spawnRelative)));

            b3SphericalJointDef jd = b3DefaultSphericalJointDef();
            jd.base.bodyIdA = bodyA; jd.base.bodyIdB = bodyB;
            jd.base.localFrameA = frameA;
            jd.base.localFrameB = centeredFrameB;
            jd.base.collideConnected = false;
            jd.enableConeLimit = link.swing1Motion != RagdollMotion::Free ||
                                 link.swing2Motion != RagdollMotion::Free;
            jd.coneAngle = std::max(0.035f,
                std::max(link.swing1Angle, link.swing2Angle));
            jd.enableTwistLimit = link.twistMotion != RagdollMotion::Free;
            jd.lowerTwistAngle = link.lowerTwistAngle;
            jd.upperTwistAngle = link.upperTwistAngle;
            jd.enableSpring = true;
            jd.hertz = 4.0f;
            jd.dampingRatio = 1.0f;
            jd.targetRotation = b3Quat_identity;
            const b3JointId joint = b3CreateSphericalJoint(m->world, &jd);
            if (!B3_IS_NULL(joint)) {
                runtime.joints.push_back({ joint, link.jointType, passiveTorque });
                runtime.softLimits.push_back({ bodyA, bodyB, frameA.q,
                    centeredFrameB.q,
                    std::max(0.035f, link.swing1Angle),
                    std::max(0.035f, link.swing2Angle) });
            }
        }
    }
    m->authoredRagdolls.push_back(std::move(runtime));
    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&impact.direction));
    if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-5f) dir = XMVectorSet(0, 0.2f, 1, 0);
    size_t nearest = 0;
    float nearestDistanceSq = FLT_MAX;
    for (size_t i = 0; i < bodies.size(); ++i) {
        if (!impact.bodyName.empty() && bodies[i].name == impact.bodyName) {
            nearest = i;
            nearestDistanceSq = 0.0f;
            break;
        }
        const float dx = bodies[i].position.x - impact.position.x;
        const float dy = bodies[i].position.y - impact.position.y;
        const float dz = bodies[i].position.z - impact.position.z;
        const float distanceSq = dx*dx + dy*dy + dz*dz;
        if (distanceSq < nearestDistanceSq) {
            nearestDistanceSq = distanceSq;
            nearest = i;
        }
    }
    const b3BodyId hitBody = m->ragdollParts[base + nearest].body;
    const float hitMass = std::max(0.05f, b3Body_GetMass(hitBody));
    const float impulseScale = std::max(0.0f, impact.impulseMultiplier);
    XMFLOAT3 impulse;
    XMStoreFloat3(&impulse, dir * (hitMass * impulseScale));
    b3Body_ApplyLinearImpulse(hitBody,
        { impulse.x, impulse.y + hitMass * 0.06f * impulseScale, impulse.z },
        { impact.position.x, impact.position.y, impact.position.z }, true);
    m->RebuildRenderItems();
    return ragdollId;
}

bool DestructionDX12::GetAuthoredRagdollPose(
    uint32_t ragdollId, std::vector<AuthoredRagdollPose>& pose) const {
    pose.clear();
    if (!m || ragdollId == InvalidIndex) return false;
    for (const Impl::RagdollPart& part : m->ragdollParts) {
        if (part.authoredId != ragdollId || B3_IS_NULL(part.body)) continue;
        const b3Pos p = b3Body_GetPosition(part.body);
        const b3Quat q = b3Body_GetRotation(part.body);
        XMFLOAT4X4 transform;
        XMStoreFloat4x4(&transform,
            XMMatrixRotationQuaternion(XMVectorSet(q.v.x, q.v.y, q.v.z, q.s)) *
            XMMatrixTranslation((float)p.x, (float)p.y, (float)p.z));
        pose.push_back({ part.authoredBone, transform });
    }
    return !pose.empty();
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
                                     float radius, XMFLOAT3& hitPosition,
                                     uint32_t ignoredHarpoonId) const {
    if (!m->initialized) return false;
    m->lastRagdollHit = -1;
    m->lastHitChunk = InvalidIndex;
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
                closest = t;
                hit = true;
                m->lastHitChunk = index;
            }
        }
    }
    for (size_t i = 0; i < m->ragdollParts.size(); ++i) {
        if (ignoredHarpoonId != 0) {
            bool carriedByThisHarpoon = false;
            for (const Impl::HarpoonRagdollAttachment& attachment :
                 m->harpoonRagdolls) {
                if (attachment.harpoonId == ignoredHarpoonId &&
                    attachment.ragdollId == m->ragdollParts[i].authoredId) {
                    carriedByThisHarpoon = true;
                    break;
                }
            }
            if (carriedByThisHarpoon) continue;
        }
        const Impl::RagdollPart& part = m->ragdollParts[i];
        const b3Vec3 a = b3Body_GetLocalPoint(part.body,
            { worldStart.x, worldStart.y, worldStart.z });
        const b3Vec3 b = b3Body_GetLocalPoint(part.body,
            { worldEnd.x, worldEnd.y, worldEnd.z });
        float t = 0.0f;
        if (SegmentAabb({ a.x, a.y, a.z }, { b.x, b.y, b.z }, radius,
                        { -part.half.x, -part.half.y, -part.half.z },
                        {  part.half.x,  part.half.y,  part.half.z }, t) && t < closest) {
            closest = t;
            hit = true;
            m->lastRagdollHit = (int)i;
            m->lastHitChunk = InvalidIndex;
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
    // Ordinary bullets chip a cell first, then a second hit severs it. High-energy
    // rounds (sniper and laser) cut the nearest cell immediately.
    Impl::ActorRuntime* hitActor = nullptr;
    uint32_t hitChunk = InvalidIndex;
    if (!m->FindNearestBreakableCell(worldPosition, hitActor, hitChunk)) return;
    const bool highEnergy = damage >= 2.0f;
    if (!highEnergy && m->bulletWeakenedChunks.insert(hitChunk).second) {
        std::cout << "Blast hit: chunk weakened\n";
        return;
    }
    m->bulletWeakenedChunks.erase(hitChunk);
    const uint32_t actorsBefore = (uint32_t)m->actors.size();
    m->BreakNearestCell(worldPosition);
    m->MarkStructureDirty(m->lastBrokenStructure);
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
    std::unordered_set<uint32_t> damagedStructures;
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
                damagedStructures.insert(chunk.structureId);
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
    for (uint32_t structureId : damagedStructures)
        m->MarkStructureDirty(structureId);
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
        if (runtime->debrisCleanupEligible) m->WakeDebris(*runtime);
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
    std::cout << "Grenade: actors " << actorsBefore << " -> " << m->actors.size() << "\n";
}

void DestructionDX12::DestroyChunkAt(const XMFLOAT3& worldPosition, float radius) {
    if (!m->initialized || m->lastRagdollHit >= 0) return;
    m->lastDamagePosition = worldPosition;
    m->lastDamageRadius = radius;

    Impl::ActorRuntime* hitActor = nullptr;
    uint32_t hitChunk = InvalidIndex;
    if (!m->FindNearestBreakableCell(
            worldPosition, hitActor, hitChunk, true, true)) return;
    m->bulletWeakenedChunks.erase(hitChunk);
    const uint32_t structureId = m->chunks[hitChunk].structureId;

    if (hitActor->chunks.size() == 1) {
        const auto runtime = std::find_if(
            m->actors.begin(), m->actors.end(),
            [hitActor](const std::unique_ptr<Impl::ActorRuntime>& value) {
                return value.get() == hitActor;
            });
        if (runtime == m->actors.end()) return;
        if (!B3_IS_NULL((*runtime)->body)) b3DestroyBody((*runtime)->body);
        if ((*runtime)->actor) {
            (*runtime)->actor->removeFromGroup();
            (*runtime)->actor->release();
        }
        m->actors.erase(runtime);
    } else {
        m->BreakNearestCell(worldPosition, true);
    }
    m->MarkStructureDirty(structureId);
    m->RebuildRenderItems();
}

void DestructionDX12::IgniteChunkAt(const XMFLOAT3& worldPosition) {
    if (!m->initialized || m->lastRagdollHit >= 0) return;
    Impl::ActorRuntime* actor = nullptr;
    uint32_t chunkIndex = InvalidIndex;
    if (!m->FindNearestBreakableCell(
            worldPosition, actor, chunkIndex, true, true)) return;
    m->IgniteChunk(chunkIndex);
}

std::vector<DestructionBurningPoint>
DestructionDX12::GetBurningChunkPoints() const {
    std::vector<DestructionBurningPoint> points;
    if (!m || !m->initialized) return points;
    points.reserve(m->burningChunks.size());
    for (const Impl::BurningChunk& burning : m->burningChunks) {
        XMFLOAT3 position;
        if (!m->ChunkWorldPosition(burning.chunkIndex, position)) continue;
        const float fade = (std::min)(1.0f, burning.life / 0.75f);
        points.push_back({ position, 1.55f, fade });
    }
    return points;
}

void DestructionDX12::StartVortex(const XMFLOAT3& worldPosition, float radius,
                                  float duration) {
    if (!m->initialized || radius <= 0.0f || duration <= 0.0f) return;
    m->lastDamagePosition = worldPosition;
    m->lastDamageRadius = radius;

    // Vortex damage is absolute: every intersecting cell loses all bonds.
    // Supports are intentionally demoted so foundation blocks inside the sphere
    // become physical debris instead of remaining pinned to the world.
    std::list<std::vector<uint8_t>> masks;
    std::list<IsolateChunksParams> paramStore;
    std::unordered_set<uint32_t> damagedStructures;
    const NvBlastDamageProgram isolate = { IsolateGraphShader, nullptr };
    bool anyMarked = false;
    for (auto& runtime : m->actors) {
        if (!runtime->actor || B3_IS_NULL(runtime->body)) continue;
        const b3Vec3 local = b3Body_GetLocalPoint(runtime->body,
            { worldPosition.x, worldPosition.y, worldPosition.z });
        const XMFLOAT3 modelCenter(local.x + runtime->center.x,
                                   local.y + runtime->center.y,
                                   local.z + runtime->center.z);
        std::vector<uint8_t> mask(m->chunks.size() + 1, 0);
        bool actorMarked = false;
        for (uint32_t chunkIndex : runtime->chunks) {
            Impl::Chunk& chunk = m->chunks[chunkIndex];
            if (!SphereAabb(modelCenter, radius,
                            chunk.minimum, chunk.maximum)) continue;
            mask[chunkIndex + 1] = 1;
            chunk.support = false;
            actorMarked = true;
            anyMarked = true;
            damagedStructures.insert(chunk.structureId);
        }
        if (!actorMarked) continue;
        masks.push_back(std::move(mask));
        paramStore.push_back({ masks.back().data(),
                               (uint32_t)masks.back().size() });
        runtime->actor->damage(isolate, &paramStore.back());
    }

    const uint32_t actorsBefore = (uint32_t)m->actors.size();
    if (anyMarked) {
        m->group->process();
        for (uint32_t structureId : damagedStructures)
            m->MarkStructureDirty(structureId);
        m->RebuildRenderItems();
    }

    if (m->vortices.size() >= 4) m->vortices.erase(m->vortices.begin());
    XMFLOAT3 orbitCenter = worldPosition;
    orbitCenter.y += radius * 0.35f;
    m->vortices.push_back({ orbitCenter, radius, 0.0f, duration, {} });
    std::cout << "Vortex: actors " << actorsBefore << " -> "
              << m->actors.size() << ", duration " << duration << "s\n";
}

uint32_t DestructionDX12::CreateExplosiveBarrelBody(
    const XMFLOAT3& worldPosition) {
    if (!m || !m->initialized || B3_IS_NULL(m->world)) return 0;

    b3BodyDef bodyDef = b3DefaultBodyDef();
    bodyDef.type = b3_dynamicBody;
    bodyDef.position = {
        worldPosition.x, worldPosition.y, worldPosition.z };
    bodyDef.linearDamping = 0.18f;
    bodyDef.angularDamping = 0.12f;
    bodyDef.allowFastRotation = true;
    const b3BodyId body = b3CreateBody(m->world, &bodyDef);

    b3ShapeDef shapeDef = b3DefaultShapeDef();
    shapeDef.density = 115.0f;
    shapeDef.baseMaterial.friction = 0.65f;
    shapeDef.baseMaterial.restitution = 0.12f;
    shapeDef.enableHitEvents = true;
    shapeDef.filter.categoryBits = CollisionCategoryBarrel;
    shapeDef.filter.maskBits = B3_DEFAULT_MASK_BITS;
    const b3Capsule capsule = {
        { 0.0f, -0.34f, 0.0f }, { 0.0f, 0.34f, 0.0f }, 0.44f };
    b3CreateCapsuleShape(body, &shapeDef, &capsule);

    uint32_t handle = m->nextBarrelHandle++;
    if (handle == 0) handle = m->nextBarrelHandle++;
    m->barrelBodies.push_back({ handle, body });
    return handle;
}

bool DestructionDX12::GetExplosiveBarrelPose(
    uint32_t handle, DestructionBodyPose& pose) const {
    if (!m || handle == 0) return false;
    const auto it = std::find_if(
        m->barrelBodies.begin(), m->barrelBodies.end(),
        [handle](const Impl::BarrelRuntime& barrel) {
            return barrel.handle == handle;
        });
    if (it == m->barrelBodies.end() || B3_IS_NULL(it->body)) return false;
    const b3Pos p = b3Body_GetPosition(it->body);
    const b3Quat q = b3Body_GetRotation(it->body);
    const b3Vec3 v = b3Body_GetLinearVelocity(it->body);
    pose.position = { (float)p.x, (float)p.y, (float)p.z };
    pose.rotation = { q.v.x, q.v.y, q.v.z, q.s };
    pose.linearVelocity = { v.x, v.y, v.z };
    return true;
}

bool DestructionDX12::SetExplosiveBarrelVelocity(
    uint32_t handle, const XMFLOAT3& linearVelocity,
    const XMFLOAT3& angularVelocity) {
    if (!m || handle == 0) return false;
    const auto it = std::find_if(
        m->barrelBodies.begin(), m->barrelBodies.end(),
        [handle](const Impl::BarrelRuntime& barrel) {
            return barrel.handle == handle;
        });
    if (it == m->barrelBodies.end() || B3_IS_NULL(it->body)) return false;
    b3Body_SetLinearVelocity(it->body,
        { linearVelocity.x, linearVelocity.y, linearVelocity.z });
    b3Body_SetAngularVelocity(it->body,
        { angularVelocity.x, angularVelocity.y, angularVelocity.z });
    b3Body_SetAwake(it->body, true);
    return true;
}

void DestructionDX12::DestroyExplosiveBarrelBody(uint32_t handle) {
    if (!m || handle == 0) return;
    const auto it = std::find_if(
        m->barrelBodies.begin(), m->barrelBodies.end(),
        [handle](const Impl::BarrelRuntime& barrel) {
            return barrel.handle == handle;
        });
    if (it == m->barrelBodies.end()) return;
    if (!B3_IS_NULL(it->body)) b3DestroyBody(it->body);
    m->barrelBodies.erase(it);
    m->barrelImpactEvents.erase(
        std::remove(m->barrelImpactEvents.begin(),
                    m->barrelImpactEvents.end(), handle),
        m->barrelImpactEvents.end());
}

std::vector<uint32_t> DestructionDX12::DrainExplosiveBarrelImpactEvents() {
    if (!m) return {};
    std::vector<uint32_t> events;
    events.swap(m->barrelImpactEvents);
    return events;
}

void DestructionDX12::ApplyRagdollExplosion(
    const XMFLOAT3& worldPosition, float radius, float impulse) {
    if (!m->initialized || radius <= 0.0f || impulse <= 0.0f) return;
    const XMVECTOR center = XMLoadFloat3(&worldPosition);
    // Enemy-only blast pressure. Debris keeps its original independent settings.
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
        for (Impl::HoverEnemy& enemy : m->hoverEnemies) {
            if ((size_t)m->lastRagdollHit >= enemy.firstPart &&
                (size_t)m->lastRagdollHit < enemy.firstPart + 11 && enemy.alive) {
                enemy.health -= 25.0f;
                if (enemy.health <= 0.0f) {
                    enemy.alive = false; // controller off: corpse falls
                    enemy.respawnTimer = 5.0f;
                }
                break;
            }
        }
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
        if (runtime->debrisCleanupEligible) m->WakeDebris(*runtime);
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

bool DestructionDX12::ApplyHarpoonPull(const XMFLOAT3& worldPosition,
                                       const XMFLOAT3& target,
                                       float impulseStrength, float hitRadius) {
    if (!m->initialized || impulseStrength <= 0.0f || hitRadius <= 0.0f)
        return false;
    const XMVECTOR impact = XMLoadFloat3(&worldPosition);
    const XMVECTOR pullTarget = XMLoadFloat3(&target);
    const float radiusSquared = hitRadius * hitRadius;
    bool applied = false;

    const auto bodyWithinHitRadius = [&](b3BodyId body) {
        if (B3_IS_NULL(body)) return false;
        const b3Pos bodyPosition = b3Body_GetPosition(body);
        const XMVECTOR position = XMVectorSet(
            (float)bodyPosition.x, (float)bodyPosition.y,
            (float)bodyPosition.z, 0.0f);
        return XMVectorGetX(XMVector3LengthSq(position - impact)) <=
               radiusSquared;
    };

    const auto pullBody = [&](b3BodyId body, float maximumDeltaVelocity) {
        if (!bodyWithinHitRadius(body)) return;
        const b3Pos bodyPosition = b3Body_GetPosition(body);
        const XMVECTOR position = XMVectorSet(
            (float)bodyPosition.x, (float)bodyPosition.y,
            (float)bodyPosition.z, 0.0f);
        const float hitDistanceSquared =
            XMVectorGetX(XMVector3LengthSq(position - impact));
        if (hitDistanceSquared > radiusSquared) return;
        XMVECTOR direction = pullTarget - position;
        if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-5f)
            direction = XMVectorSet(0.0f, 0.25f, 1.0f, 0.0f);
        direction = XMVector3Normalize(direction + XMVectorSet(0.0f, 0.10f, 0.0f, 0.0f));
        const float falloff = (std::max)(0.25f,
            1.0f - std::sqrt(hitDistanceSquared) / hitRadius);
        const float mass = (std::max)(0.05f, b3Body_GetMass(body));
        const float magnitude = (std::min)(
            impulseStrength * falloff, maximumDeltaVelocity * mass);
        XMFLOAT3 impulse;
        XMStoreFloat3(&impulse, direction * magnitude);
        b3Body_ApplyLinearImpulseToCenter(
            body, { impulse.x, impulse.y, impulse.z }, true);
        applied = true;
    };

    for (auto& runtime : m->actors) {
        if (!runtime->dynamic ||
            !bodyWithinHitRadius(runtime->body)) continue;
        if (runtime->debrisCleanupEligible) m->WakeDebris(*runtime);
        pullBody(runtime->body, 18.0f);
    }
    for (const Impl::RagdollPart& part : m->ragdollParts)
        pullBody(part.body, 20.0f);
    for (const Impl::BarrelRuntime& barrel : m->barrelBodies)
        pullBody(barrel.body, 16.0f);

    if (applied) {
        m->lastRagdollHit = -1;
        m->rebuiltWhileStill = false;
    }
    return applied;
}

bool DestructionDX12::AttachRagdollToHarpoon(
    uint32_t ragdollId, uint32_t harpoonId,
    const XMFLOAT3& impactPosition, float shaftOffset,
    const std::string& struckBone) {
    if (!m->initialized || ragdollId == InvalidIndex || harpoonId == 0)
        return false;
    for (const Impl::HarpoonRagdollAttachment& attachment :
         m->harpoonRagdolls)
        if (attachment.ragdollId == ragdollId) return false;

    Impl::HarpoonRagdollAttachment attachment;
    attachment.harpoonId = harpoonId;
    attachment.ragdollId = ragdollId;
    attachment.shaftOffset = (std::max)(0.0f, shaftOffset);

    // Exact live-shape hit selects the body. Nearest-body fallback supports old
    // callers and malformed assets without returning to whole-body dragging.
    size_t hitPartIndex = SIZE_MAX;
    float hitDistanceSq = FLT_MAX;
    for (size_t partIndex = 0;
         partIndex < m->ragdollParts.size(); ++partIndex) {
        Impl::RagdollPart& part = m->ragdollParts[partIndex];
        if (part.authoredId != ragdollId || B3_IS_NULL(part.body)) continue;
        if (!struckBone.empty() && part.authoredBone == struckBone) {
            hitPartIndex = partIndex;
            hitDistanceSq = 0.0f;
            break;
        }
        const b3Pos position = b3Body_GetPosition(part.body);
        const float dx = (float)position.x - impactPosition.x;
        const float dy = (float)position.y - impactPosition.y;
        const float dz = (float)position.z - impactPosition.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq >= hitDistanceSq) continue;
        hitDistanceSq = distanceSq;
        hitPartIndex = partIndex;
    }
    if (hitPartIndex == SIZE_MAX) return false;

    Impl::RagdollPart& hitPart = m->ragdollParts[hitPartIndex];
    Impl::HarpoonRagdollPart carried;
    carried.partIndex = hitPartIndex;
    carried.ragdollLocalAnchor = b3Body_GetLocalPoint(hitPart.body,
        { impactPosition.x, impactPosition.y, impactPosition.z });
    carried.desiredAnchor = impactPosition;
    b3BodyDef anchorDef = b3DefaultBodyDef();
    anchorDef.type = b3_kinematicBody;
    anchorDef.position = { impactPosition.x, impactPosition.y, impactPosition.z };
    anchorDef.enableSleep = false;
    carried.anchorBody = b3CreateBody(m->world, &anchorDef);
    if (B3_IS_NULL(carried.anchorBody)) return false;
    b3SphericalJointDef point = b3DefaultSphericalJointDef();
    point.base.bodyIdA = carried.anchorBody;
    point.base.bodyIdB = hitPart.body;
    point.base.localFrameA.p = { 0,0,0 };
    point.base.localFrameB.p = carried.ragdollLocalAnchor;
    point.base.collideConnected = false;
    carried.joint = b3CreateSphericalJoint(m->world, &point);
    if (B3_IS_NULL(carried.joint)) {
        b3DestroyBody(carried.anchorBody);
        return false;
    }

    attachment.parts.push_back(carried);
    m->harpoonRagdolls.push_back(std::move(attachment));
    m->rebuiltWhileStill = false;
    return true;
}

void DestructionDX12::MoveHarpoonRagdolls(
    uint32_t harpoonId, const XMFLOAT3& harpoonPosition,
    const XMFLOAT3& direction) {
    if (!m->initialized || harpoonId == 0) return;
    for (Impl::HarpoonRagdollAttachment& attachment :
         m->harpoonRagdolls) {
        if (attachment.harpoonId != harpoonId) continue;
        const XMFLOAT3 anchor = {
            harpoonPosition.x - direction.x * (0.45f + attachment.shaftOffset),
            harpoonPosition.y - direction.y * (0.45f + attachment.shaftOffset),
            harpoonPosition.z - direction.z * (0.45f + attachment.shaftOffset) };
        for (Impl::HarpoonRagdollPart& attachedPart : attachment.parts)
            attachedPart.desiredAnchor = anchor;
    }
    m->rebuiltWhileStill = false;
}

void DestructionDX12::PinHarpoonRagdolls(
    uint32_t harpoonId, const XMFLOAT3& impactPosition,
    const XMFLOAT3& direction, bool attachToLastDestructible) {
    if (!m->initialized || harpoonId == 0) return;
    MoveHarpoonRagdolls(harpoonId, impactPosition, direction);
    const uint32_t targetChunk = attachToLastDestructible
        ? m->lastHitChunk : InvalidIndex;
    for (Impl::HarpoonRagdollAttachment& attachment :
         m->harpoonRagdolls) {
        if (attachment.harpoonId != harpoonId) continue;
        for (Impl::HarpoonRagdollPart& attachedPart : attachment.parts) {
            if (attachedPart.partIndex >= m->ragdollParts.size()) continue;
            const b3BodyId body = m->ragdollParts[attachedPart.partIndex].body;
            if (B3_IS_NULL(body)) continue;
            if (!B3_IS_NULL(attachedPart.joint) &&
                b3Joint_IsValid(attachedPart.joint))
                b3DestroyJoint(attachedPart.joint, true);
            if (!B3_IS_NULL(attachedPart.anchorBody))
                b3DestroyBody(attachedPart.anchorBody);

            Impl::PinnedHarpoonRagdoll pin;
            pin.harpoonId = harpoonId;
            pin.chunkIndex = targetChunk;
            pin.partIndex = attachedPart.partIndex;
            pin.ragdollLocalAnchor = attachedPart.ragdollLocalAnchor;
            pin.ragdollLocalDirection = b3Body_GetLocalVector(body, {
                direction.x, direction.y, direction.z });
            if (targetChunk != InvalidIndex) {
                Impl::ActorRuntime* owner = m->FindChunkOwner(targetChunk);
                if (owner && !B3_IS_NULL(owner->body)) {
                    pin.targetBody = owner->body;
                    pin.targetLocalAnchor = b3Body_GetLocalPoint(owner->body, {
                        impactPosition.x, impactPosition.y, impactPosition.z });
                    pin.targetLocalDirection = b3Body_GetLocalVector(owner->body, {
                        direction.x, direction.y, direction.z });
                    pin.targetFrameValid = true;
                }
            }
            if (!pin.targetFrameValid) {
                b3BodyDef staticDef = b3DefaultBodyDef();
                staticDef.type = b3_staticBody;
                staticDef.position = { impactPosition.x, impactPosition.y,
                                       impactPosition.z };
                pin.staticAnchorBody = b3CreateBody(m->world, &staticDef);
                pin.targetBody = pin.staticAnchorBody;
                pin.targetLocalAnchor = { 0,0,0 };
                pin.targetLocalDirection = { direction.x, direction.y, direction.z };
                pin.targetFrameValid = !B3_IS_NULL(pin.staticAnchorBody);
                pin.chunkIndex = InvalidIndex;
            }
            m->pinnedHarpoonRagdolls.push_back(pin);
            if (!m->CreatePinnedHarpoonJoint(
                    m->pinnedHarpoonRagdolls.back())) {
                if (!B3_IS_NULL(m->pinnedHarpoonRagdolls.back().staticAnchorBody))
                    b3DestroyBody(m->pinnedHarpoonRagdolls.back().staticAnchorBody);
                m->pinnedHarpoonRagdolls.pop_back();
            }
        }
    }
    m->harpoonRagdolls.erase(
        std::remove_if(m->harpoonRagdolls.begin(), m->harpoonRagdolls.end(),
            [harpoonId](const Impl::HarpoonRagdollAttachment& attachment) {
                return attachment.harpoonId == harpoonId;
            }),
        m->harpoonRagdolls.end());
    m->lastHitChunk = InvalidIndex;
    m->RebuildRenderItems();
}

bool DestructionDX12::GetPinnedHarpoonPose(
    uint32_t harpoonId, XMFLOAT3& position, XMFLOAT3& direction) const {
    if (!m || !m->initialized || harpoonId == 0) return false;
    for (const Impl::PinnedHarpoonRagdoll& pin :
         m->pinnedHarpoonRagdolls) {
        if (pin.harpoonId != harpoonId ||
            pin.partIndex >= m->ragdollParts.size()) continue;
        const Impl::ActorRuntime* owner = pin.chunkIndex == InvalidIndex
            ? nullptr : m->FindChunkOwner(pin.chunkIndex);
        const b3BodyId body = owner ? owner->body : pin.staticAnchorBody;
        if (B3_IS_NULL(body) || !pin.targetFrameValid) continue;
        const b3Pos worldPosition = b3Body_GetWorldPoint(
            body, pin.targetLocalAnchor);
        const b3Vec3 worldDirection = b3Body_GetWorldVector(
            body, pin.targetLocalDirection);
        position = { (float)worldPosition.x, (float)worldPosition.y,
                     (float)worldPosition.z };
        XMVECTOR normalized = XMVectorSet(
            worldDirection.x, worldDirection.y, worldDirection.z, 0.0f);
        normalized = XMVectorGetX(XMVector3LengthSq(normalized)) < 1e-5f
            ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
            : XMVector3Normalize(normalized);
        XMStoreFloat3(&direction, normalized);
        return true;
    }
    return false;
}

void DestructionDX12::ReleaseHarpoonRagdolls(
    uint32_t harpoonId, const XMFLOAT3& direction, float speed) {
    if (!m->initialized || harpoonId == 0) return;
    XMVECTOR releaseDirection = XMLoadFloat3(&direction);
    if (XMVectorGetX(XMVector3LengthSq(releaseDirection)) < 1e-5f)
        releaseDirection = XMVectorSet(0.0f, 0.15f, 1.0f, 0.0f);
    releaseDirection = XMVector3Normalize(releaseDirection);
    XMFLOAT3 velocity;
    XMStoreFloat3(&velocity, releaseDirection * (std::max)(0.0f, speed));
    for (Impl::HarpoonRagdollAttachment& attachment :
         m->harpoonRagdolls) {
        if (attachment.harpoonId != harpoonId) continue;
        for (const Impl::HarpoonRagdollPart& attachedPart : attachment.parts) {
            if (attachedPart.partIndex >= m->ragdollParts.size()) continue;
            const b3BodyId body = m->ragdollParts[attachedPart.partIndex].body;
            if (B3_IS_NULL(body)) continue;
            b3Body_SetType(body, b3_dynamicBody);
            b3Body_SetLinearVelocity(
                body, { velocity.x, velocity.y, velocity.z });
            if (!B3_IS_NULL(attachedPart.joint) &&
                b3Joint_IsValid(attachedPart.joint))
                b3DestroyJoint(attachedPart.joint, true);
            if (!B3_IS_NULL(attachedPart.anchorBody))
                b3DestroyBody(attachedPart.anchorBody);
        }
    }
    m->harpoonRagdolls.erase(
        std::remove_if(m->harpoonRagdolls.begin(), m->harpoonRagdolls.end(),
            [harpoonId](const Impl::HarpoonRagdollAttachment& attachment) {
                return attachment.harpoonId == harpoonId;
            }),
        m->harpoonRagdolls.end());
    m->rebuiltWhileStill = false;
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

std::vector<DestructionCollisionSoundEvent>
DestructionDX12::DrainCollisionSoundEvents() {
    if (!m) return {};
    std::vector<DestructionCollisionSoundEvent> out =
        std::move(m->collisionSoundEvents);
    m->collisionSoundEvents.clear();
    return out;
}

std::vector<TinyDebrisParticle> DestructionDX12::DrainTinyDebrisParticles() {
    if (!m) return {};
    std::vector<TinyDebrisParticle> out = std::move(m->tinyDebrisParticles);
    m->tinyDebrisParticles.clear();
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
        m->EnsureActorBatchBounds(*runtime);
        XMFLOAT3 actorCenter;
        XMStoreFloat3(&actorCenter, XMVector3Transform(
            XMLoadFloat3(&runtime->batchCenter), transform));
        const float reach = runtime->batchRadius + radius + 0.5f;
        if (std::abs(actorCenter.x - eyePosition.x) > reach ||
            std::abs(actorCenter.z - eyePosition.z) > reach ||
            actorCenter.y + runtime->batchRadius < feet - kStepHeight ||
            actorCenter.y - runtime->batchRadius > eyePosition.y) continue;
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
        if (!split) continue;

        // userData belongs to an actor that Blast has just replaced. A later
        // split event may still contain that old pointer, so never dereference
        // it until ownership is confirmed against the live runtime list.
        auto* candidate = static_cast<Impl::ActorRuntime*>(split->parentData.userData);
        auto parentIt = std::find_if(m->actors.begin(), m->actors.end(),
            [candidate](const std::unique_ptr<Impl::ActorRuntime>& runtime) {
                return runtime.get() == candidate;
            });
        Impl::ActorRuntime* parent = parentIt != m->actors.end() ? parentIt->get() : nullptr;
        Impl::BodySeed seed;
        if (parent && !B3_IS_NULL(parent->body)) {
            seed.valid = true;
            seed.modelCenter = parent->center;
            seed.position = b3Body_GetPosition(parent->body);
            seed.rotation = b3Body_GetRotation(parent->body);
            seed.linearVelocity = b3Body_GetLinearVelocity(parent->body);
            seed.angularVelocity = b3Body_GetAngularVelocity(parent->body);
        }
        if (parentIt != m->actors.end()) {
            if (!B3_IS_NULL((*parentIt)->body)) b3DestroyBody((*parentIt)->body);
            m->actors.erase(parentIt);
        }
        for (uint32_t childIndex = 0; childIndex < split->numChildren; ++childIndex) {
            TkActor* child = split->children[childIndex];
            if (!child) continue;
            child->userData = nullptr;
            auto runtime = std::make_unique<Impl::ActorRuntime>();
            runtime->actor = child;
            runtime->renderId = m->nextActorRenderId++;
            // Destroyed actors never enter batch construction. Rendering remains
            // immediately available through their individual chunk geometry.
            const uint32_t visibleCount = child->getVisibleChunkCount();
            std::vector<uint32_t> visible(visibleCount);
            child->getVisibleChunkIndices(visible.data(), visibleCount);
            for (uint32_t assetChunk : visible) {
                if (assetChunk > 0 && assetChunk <= m->chunks.size())
                    runtime->chunks.push_back(assetChunk - 1);
            }
            if (runtime->chunks.empty()) continue;
            runtime->structureId = m->chunks[runtime->chunks.front()].structureId;
            for (uint32_t chunkIndex : runtime->chunks) {
                runtime->center.x += m->chunks[chunkIndex].center.x;
                runtime->center.y += m->chunks[chunkIndex].center.y;
                runtime->center.z += m->chunks[chunkIndex].center.z;
            }
            const float centerInv = 1.0f / static_cast<float>(runtime->chunks.size());
            runtime->center.x *= centerInv;
            runtime->center.y *= centerInv;
            runtime->center.z *= centerInv;
            child->userData = runtime.get();
            // An island still containing an anchored support chunk is part of
            // the standing structure -> keep it static. Islands with no support
            // have been structurally freed -> simulate them dynamically.
            bool islandSupported = false;
            for (uint32_t chunkIndex : runtime->chunks) {
                if (m->chunks[chunkIndex].support) { islandSupported = true; break; }
            }
            if (!islandSupported && runtime->chunks.size() == 1 &&
                m->ActorMaxExtent(*runtime) <= TinyDebrisMaxExtent) {
                child->userData = nullptr;
                child->removeFromGroup();
                m->EmitTinyDebris(*runtime, &seed);
                if (m->stressStats.running) m->stressStats.tinyParticles += 3;
                continue;
            }
            m->CreateBody(*runtime, !islandSupported, islandSupported ? nullptr : &seed);
            // This is the zero-health transition. Start a fresh inactivity
            // timer; later impacts reset it instead of inheriting old age.
            runtime->debrisCleanupEligible = runtime->dynamic;
            runtime->debrisAge = 0.0f;
            // A freed (unsupported) island is a real break -> mark its spot for
            // a puff of smoke at the fracture.
            if (!islandSupported && !B3_IS_NULL(runtime->body)) {
                const b3Pos bp = b3Body_GetPosition(runtime->body);
                m->breakPoints.push_back({ (float)bp.x, (float)bp.y, (float)bp.z });
            }
            m->actors.push_back(std::move(runtime));
        }
    }
    m->RefreshPinnedHarpoonJoints();
}

std::vector<DestructionDebrisHazard> DestructionDX12::GetDangerousDebris(
    float minimumSpeed) const {
    std::vector<DestructionDebrisHazard> hazards;
    if (!m || !m->initialized) return hazards;
    const float minimumSpeedSq = minimumSpeed * minimumSpeed;

    for (const auto& runtime : m->actors) {
        if (!runtime->dynamic || B3_IS_NULL(runtime->body) ||
            !b3Body_IsAwake(runtime->body)) continue;

        const XMMATRIX transform = BoxTransform(runtime->body, runtime->center);
        const float chunkMass = std::max(0.05f, b3Body_GetMass(runtime->body)) /
                                std::max<size_t>(1, runtime->chunks.size());
        for (uint32_t chunkIndex : runtime->chunks) {
            const Impl::Chunk& chunk = m->chunks[chunkIndex];
            XMFLOAT3 worldMin(FLT_MAX, FLT_MAX, FLT_MAX);
            XMFLOAT3 worldMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            for (float x : { chunk.minimum.x, chunk.maximum.x })
            for (float y : { chunk.minimum.y, chunk.maximum.y })
            for (float z : { chunk.minimum.z, chunk.maximum.z }) {
                XMFLOAT3 point;
                XMStoreFloat3(&point, XMVector3TransformCoord(
                    XMVectorSet(x, y, z, 1.0f), transform));
                worldMin.x = std::min(worldMin.x, point.x);
                worldMin.y = std::min(worldMin.y, point.y);
                worldMin.z = std::min(worldMin.z, point.z);
                worldMax.x = std::max(worldMax.x, point.x);
                worldMax.y = std::max(worldMax.y, point.y);
                worldMax.z = std::max(worldMax.z, point.z);
            }

            XMFLOAT3 center;
            XMStoreFloat3(&center, XMVector3TransformCoord(
                XMLoadFloat3(&chunk.center), transform));
            const b3Vec3 velocity = b3Body_GetWorldPointVelocity(
                runtime->body, { center.x, center.y, center.z });
            const float speedSq = velocity.x * velocity.x +
                                  velocity.y * velocity.y +
                                  velocity.z * velocity.z;
            if (speedSq < minimumSpeedSq) continue;
            hazards.push_back({ worldMin, worldMax, center,
                { velocity.x, velocity.y, velocity.z }, chunkMass });
        }
    }

    // Thrown authored ragdolls become gameplay hazards too. Their real Box3D
    // velocity, mass, orientation, and extents feed the same enemy-impact path
    // as destructible chunks, so a corpse can bowl another enemy over or kill it.
    for (const Impl::RagdollPart& part : m->ragdollParts) {
        if (part.authoredId == InvalidIndex || B3_IS_NULL(part.body) ||
            !b3Body_IsAwake(part.body)) continue;
        const b3Pos bodyPosition = b3Body_GetPosition(part.body);
        const b3Vec3 velocity = b3Body_GetWorldPointVelocity(
            part.body, bodyPosition);
        const float speedSq = velocity.x * velocity.x +
                              velocity.y * velocity.y +
                              velocity.z * velocity.z;
        if (speedSq < minimumSpeedSq) continue;

        const XMMATRIX transform = BoxTransform(part.body, { 0.0f, 0.0f, 0.0f });
        const XMFLOAT3 half = {
            (std::max)(0.08f, part.half.x),
            (std::max)(0.08f, part.half.y),
            (std::max)(0.08f, part.half.z) };
        XMFLOAT3 worldMin(FLT_MAX, FLT_MAX, FLT_MAX);
        XMFLOAT3 worldMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (float x : { -half.x, half.x })
        for (float y : { -half.y, half.y })
        for (float z : { -half.z, half.z }) {
            XMFLOAT3 point;
            XMStoreFloat3(&point, XMVector3TransformCoord(
                XMVectorSet(x, y, z, 1.0f), transform));
            worldMin.x = (std::min)(worldMin.x, point.x);
            worldMin.y = (std::min)(worldMin.y, point.y);
            worldMin.z = (std::min)(worldMin.z, point.z);
            worldMax.x = (std::max)(worldMax.x, point.x);
            worldMax.y = (std::max)(worldMax.y, point.y);
            worldMax.z = (std::max)(worldMax.z, point.z);
        }
        const XMFLOAT3 center = {
            static_cast<float>(bodyPosition.x),
            static_cast<float>(bodyPosition.y),
            static_cast<float>(bodyPosition.z) };
        hazards.push_back({ worldMin, worldMax, center,
            { velocity.x, velocity.y, velocity.z },
            (std::max)(0.05f, b3Body_GetMass(part.body)),
            part.lethalHazard });
    }
    return hazards;
}

void DestructionDX12::StartCollapseStressBenchmark() {
    if (!m || !m->initialized || m->chunks.empty()) return;
    m->stressStats = {};
    m->stressStats.running = true;
    m->stressUpdateTotalMs = 0.0;
    m->stressStartRebuilds = m->renderItemRebuildCount;
    XMFLOAT3 lo{ FLT_MAX, FLT_MAX, FLT_MAX };
    XMFLOAT3 hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const Impl::Chunk& chunk : m->chunks) {
        lo.x = (std::min)(lo.x, chunk.minimum.x);
        lo.y = (std::min)(lo.y, chunk.minimum.y);
        lo.z = (std::min)(lo.z, chunk.minimum.z);
        hi.x = (std::max)(hi.x, chunk.maximum.x);
        hi.y = (std::max)(hi.y, chunk.maximum.y);
        hi.z = (std::max)(hi.z, chunk.maximum.z);
    }
    const XMFLOAT3 center{ (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                           (lo.z + hi.z) * 0.5f };
    const float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
    const float radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.55f + 1.0f;
    const auto begin = std::chrono::steady_clock::now();
    ApplyExplosion(center, radius, 1000.0f, 35.0f);
    m->stressStats.triggerMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
}

DestructionStressStats DestructionDX12::GetStressStats() const {
    return m ? m->stressStats : DestructionStressStats{};
}

bool DestructionDX12::IsInitialized() const { return m && m->initialized; }
uint32_t DestructionDX12::GetChunkCount() const { return m ? (uint32_t)m->chunks.size() : 0; }
uint32_t DestructionDX12::GetActorCount() const { return m ? (uint32_t)m->actors.size() : 0; }
uint64_t DestructionDX12::GetRenderItemRebuildCount() const {
    return m ? m->renderItemRebuildCount : 0;
}
uint64_t DestructionDX12::GetBatchGeometryRebuildCount() const {
    return m ? m->batchGeometryRebuildCount : 0;
}
uint32_t DestructionDX12::GetAwakeActorCount() const {
    if (!m) return 0;
    uint32_t count = 0;
    for (const auto& runtime : m->actors)
        if (runtime->dynamic && !B3_IS_NULL(runtime->body) &&
            b3Body_IsAwake(runtime->body)) ++count;
    return count;
}
uint32_t DestructionDX12::GetLowMotionActorCount() const {
    if (!m) return 0;
    uint32_t count = 0;
    for (const auto& runtime : m->actors) {
        if (!runtime->dynamic || B3_IS_NULL(runtime->body) ||
            !b3Body_IsAwake(runtime->body)) continue;
        const b3Vec3 linear = b3Body_GetLinearVelocity(runtime->body);
        const b3Vec3 angular = b3Body_GetAngularVelocity(runtime->body);
        const float linearSq = linear.x * linear.x + linear.y * linear.y +
            linear.z * linear.z;
        const float angularSq = angular.x * angular.x + angular.y * angular.y +
            angular.z * angular.z;
        if (linearSq < 0.04f && angularSq < 0.09f) ++count;
    }
    return count;
}
uint32_t DestructionDX12::GetSpatialBatchCount() const {
    return m ? static_cast<uint32_t>(m->spatialBatchCache.size()) : 0;
}
uint32_t DestructionDX12::GetCollisionLodActorCount() const {
    if (!m) return 0;
    uint32_t count = 0;
    for (const auto& runtime : m->actors) count += runtime->collisionLod ? 1u : 0u;
    return count;
}
uint32_t DestructionDX12::GetFrozenActorCount() const {
    if (!m) return 0;
    uint32_t count = 0;
    for (const auto& runtime : m->actors) count += runtime->frozen ? 1u : 0u;
    return count;
}
bool DestructionDX12::IsBatchBuildPending() const {
    return m && (m->batchBuildInFlight || m->spatialBatchBuildInFlight);
}
const std::vector<DestructionRenderItem>& DestructionDX12::GetRenderItems() const { return m->renderItems; }
const std::vector<DestructionRenderBatch>& DestructionDX12::GetRenderBatches() const { return m->renderBatches; }
const std::vector<RagdollRenderItem>& DestructionDX12::GetRagdollRenderItems() const { return m->ragdollRenderItems; }
const std::vector<EnemyGunRenderItem>& DestructionDX12::GetEnemyGunRenderItems() const { return m->enemyGunRenderItems; }

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
