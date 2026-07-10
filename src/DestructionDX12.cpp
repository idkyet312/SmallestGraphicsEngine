#define NOMINMAX
#include "DestructionDX12.h"

#include "GLBImporter.h"
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
#include <iostream>
#include <list>
#include <unordered_map>

using namespace DirectX;
using namespace Nv::Blast;

namespace {
constexpr uint32_t InvalidIndex = 0xFFFFFFFFu;

struct RadialDamageParams {
    float position[3];
    float radius;
    float damage;
};

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
            if (dx * dx + dy * dy + dz * dz > radiusSquared ||
                commands->bondFractureCount >= capacity) continue;
            NvBlastBondFractureData& fracture =
                commands->bondFractures[commands->bondFractureCount++];
            fracture.userdata = bond.userData;
            fracture.nodeIndex0 = node;
            fracture.nodeIndex1 = other;
            fracture.health = params.damage;
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
        int x = 0, y = 0, z = 0;
    };

    struct ActorRuntime {
        TkActor* actor = nullptr;
        std::vector<uint32_t> chunks;
        b3BodyId body = b3_nullBodyId;
        XMFLOAT3 center = {};
        bool dynamic = false;
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
    std::vector<Chunk> chunks;
    std::list<std::unique_ptr<ActorRuntime>> actors;
    std::vector<DestructionRenderItem> renderItems;
    TkFramework* framework = nullptr;
    TkAsset* asset = nullptr;
    TkFamily* family = nullptr;
    TkGroup* group = nullptr;
    b3WorldId world = b3_nullWorldId;
    b3BodyId ground = b3_nullBodyId;
    float accumulator = 0.0f;
    XMFLOAT3 lastDamagePosition = {};
    bool initialized = false;

    bool BuildChunks() {
        if (!source || !source->mesh || !device) return false;
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

        auto axisCell = [](float value, float lo, float size, int count) {
            if (size <= 0.00001f) return 0;
            return std::max(0, std::min(count - 1, (int)((value - lo) / size)));
        };
        for (size_t material = 0; material < source->mesh->primitives.size(); ++material) {
            const MeshPrimitive& src = source->mesh->primitives[material];
            for (size_t tri = 0; tri + 2 < src.indices.size(); tri += 3) {
                const UINT i0 = src.indices[tri], i1 = src.indices[tri + 1], i2 = src.indices[tri + 2];
                if ((size_t)std::max({ i0, i1, i2 }) * 12 + 11 >= src.vertices.size()) continue;
                const float cx = (src.vertices[(size_t)i0 * 12] + src.vertices[(size_t)i1 * 12] + src.vertices[(size_t)i2 * 12]) / 3.0f;
                const float cy = (src.vertices[(size_t)i0 * 12 + 1] + src.vertices[(size_t)i1 * 12 + 1] + src.vertices[(size_t)i2 * 12 + 1]) / 3.0f;
                const float cz = (src.vertices[(size_t)i0 * 12 + 2] + src.vertices[(size_t)i1 * 12 + 2] + src.vertices[(size_t)i2 * 12 + 2]) / 3.0f;
                const int x = axisCell(cx, sceneMin.x, cellSize.x, gridX);
                const int y = axisCell(cy, sceneMin.y, cellSize.y, gridY);
                const int z = axisCell(cz, sceneMin.z, cellSize.z, gridZ);
                CellBuild& cell = cells[(size_t)(z * gridY + y) * gridX + x];
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
        framework = NvBlastTkFrameworkCreate();
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
        for (uint32_t i = 0; i < chunks.size(); ++i) {
            const Chunk& chunk = chunks[i];
            NvBlastChunkDesc& desc = chunkDescs[i + 1];
            desc.centroid[0] = chunk.center.x; desc.centroid[1] = chunk.center.y; desc.centroid[2] = chunk.center.z;
            desc.volume = std::max(0.001f, (chunk.maximum.x - chunk.minimum.x) *
                (chunk.maximum.y - chunk.minimum.y) * (chunk.maximum.z - chunk.minimum.z));
            desc.parentChunkDescIndex = 0;
            desc.flags = NvBlastChunkDesc::SupportFlag;
            desc.userData = i;
        }

        std::vector<NvBlastBondDesc> bonds;
        for (uint32_t a = 0; a < chunks.size(); ++a) for (uint32_t b = a + 1; b < chunks.size(); ++b) {
            const int dx = chunks[b].x - chunks[a].x;
            const int dy = chunks[b].y - chunks[a].y;
            const int dz = chunks[b].z - chunks[a].z;
            if (std::abs(dx) + std::abs(dy) + std::abs(dz) != 1) continue;
            NvBlastBondDesc bond = {};
            bond.chunkIndices[0] = a + 1; bond.chunkIndices[1] = b + 1;
            bond.bond.normal[0] = (float)dx; bond.bond.normal[1] = (float)dy; bond.bond.normal[2] = (float)dz;
            bond.bond.centroid[0] = (chunks[a].center.x + chunks[b].center.x) * 0.5f;
            bond.bond.centroid[1] = (chunks[a].center.y + chunks[b].center.y) * 0.5f;
            bond.bond.centroid[2] = (chunks[a].center.z + chunks[b].center.z) * 0.5f;
            const XMFLOAT3 size(chunks[a].maximum.x - chunks[a].minimum.x,
                                chunks[a].maximum.y - chunks[a].minimum.y,
                                chunks[a].maximum.z - chunks[a].minimum.z);
            bond.bond.area = dx ? size.y * size.z : (dy ? size.x * size.z : size.x * size.y);
            bond.bond.userData = (uint32_t)bonds.size();
            bonds.push_back(bond);
        }

        TkAssetDesc assetDesc;
        assetDesc.chunkCount = (uint32_t)chunkDescs.size(); assetDesc.chunkDescs = chunkDescs.data();
        assetDesc.bondCount = (uint32_t)bonds.size(); assetDesc.bondDescs = bonds.data();
        asset = framework->createAsset(assetDesc);
        if (!asset) return false;
        TkGroupDesc groupDesc = {}; groupDesc.workerCount = 1;
        group = framework->createGroup(groupDesc);
        if (!group) return false;
        TkActorDesc actorDesc(asset);
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
        world = b3CreateWorld(&worldDef);
        if (B3_IS_NULL(world)) return false;
        b3BodyDef groundDef = b3DefaultBodyDef();
        groundDef.position = { 0.0f, -0.5f, 0.0f };
        ground = b3CreateBody(world, &groundDef);
        b3BoxHull groundHull = b3MakeBoxHull(60.0f, 0.5f, 60.0f);
        b3ShapeDef groundShape = b3DefaultShapeDef();
        groundShape.baseMaterial.friction = 0.8f;
        b3CreateHullShape(ground, &groundShape, &groundHull.base);
        CreateBody(*actors.front(), false, nullptr);
        return true;
    }

    void CreateBody(ActorRuntime& runtime, bool forceDynamic, const BodySeed* seed) {
        if (!B3_IS_NULL(runtime.body)) b3DestroyBody(runtime.body);
        runtime.center = {};
        bool grounded = false;
        for (uint32_t index : runtime.chunks) {
            runtime.center.x += chunks[index].center.x;
            runtime.center.y += chunks[index].center.y;
            runtime.center.z += chunks[index].center.z;
            grounded |= chunks[index].y == 0;
        }
        const float inv = 1.0f / std::max<size_t>(1, runtime.chunks.size());
        runtime.center.x *= inv; runtime.center.y *= inv; runtime.center.z *= inv;
        runtime.dynamic = forceDynamic && !grounded;
        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = runtime.dynamic ? b3_dynamicBody : b3_staticBody;
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
        shapeDef.density = runtime.dynamic ? 180.0f : 0.0f;
        shapeDef.baseMaterial.friction = 0.65f;
        shapeDef.baseMaterial.restitution = 0.05f;
        for (uint32_t index : runtime.chunks) {
            const Chunk& chunk = chunks[index];
            const float hx = std::max(0.05f, (chunk.maximum.x - chunk.minimum.x) * 0.48f);
            const float hy = std::max(0.05f, (chunk.maximum.y - chunk.minimum.y) * 0.48f);
            const float hz = std::max(0.05f, (chunk.maximum.z - chunk.minimum.z) * 0.48f);
            const b3Vec3 offset = { chunk.center.x - runtime.center.x,
                                    chunk.center.y - runtime.center.y,
                                    chunk.center.z - runtime.center.z };
            b3BoxHull box = b3MakeOffsetBoxHull(hx, hy, hz, offset);
            b3CreateHullShape(runtime.body, &shapeDef, &box.base);
        }
        if (runtime.dynamic) {
            const b3Pos bodyPosition = b3Body_GetPosition(runtime.body);
            const XMFLOAT3 worldCenter((float)bodyPosition.x, (float)bodyPosition.y, (float)bodyPosition.z);
            XMVECTOR impulse = XMLoadFloat3(&worldCenter) - XMLoadFloat3(&lastDamagePosition);
            impulse = XMVector3Normalize(impulse + XMVectorSet(0.0f, 0.35f, 0.0f, 0.0f));
            XMFLOAT3 v; XMStoreFloat3(&v, impulse * 3.0f);
            b3Body_SetLinearVelocity(runtime.body, { v.x, v.y, v.z });
        }
    }

    void RebuildRenderItems() {
        renderItems.clear();
        for (const auto& runtime : actors) {
            XMMATRIX transform = XMMatrixIdentity();
            if (!B3_IS_NULL(runtime->body)) transform = BoxTransform(runtime->body, runtime->center);
            XMFLOAT4X4 stored; XMStoreFloat4x4(&stored, transform);
            for (uint32_t chunk : runtime->chunks) renderItems.push_back({ chunks[chunk].node, stored });
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
    if (m->framework) m->framework->release();
    m->group = nullptr; m->asset = nullptr; m->framework = nullptr;
    m->chunks.clear(); m->renderItems.clear(); m->initialized = false;
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
    while (m->accumulator >= step) { b3World_Step(m->world, step, 4); m->accumulator -= step; }
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
    if (hit) {
        hitPosition = { worldStart.x + (worldEnd.x - worldStart.x) * closest,
                        worldStart.y + (worldEnd.y - worldStart.y) * closest,
                        worldStart.z + (worldEnd.z - worldStart.z) * closest };
    }
    return hit;
}

void DestructionDX12::ApplyRadialDamage(const XMFLOAT3& worldPosition, float radius, float damage) {
    if (!m->initialized) return;
    m->lastDamagePosition = worldPosition;
    const NvBlastDamageProgram program = { RadialGraphShader, nullptr };
    std::vector<RadialDamageParams> params;
    params.reserve(m->actors.size());
    for (auto& runtime : m->actors) {
        const b3Vec3 local = b3Body_GetLocalPoint(runtime->body,
            { worldPosition.x, worldPosition.y, worldPosition.z });
        params.push_back({ { local.x + runtime->center.x, local.y + runtime->center.y,
                             local.z + runtime->center.z }, radius, damage });
        runtime->actor->damage(program, &params.back());
    }
    m->group->process();
    m->RebuildRenderItems();
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
            m->CreateBody(*runtime, true, &seed);
            m->actors.push_back(std::move(runtime));
        }
    }
}

bool DestructionDX12::IsInitialized() const { return m && m->initialized; }
uint32_t DestructionDX12::GetChunkCount() const { return m ? (uint32_t)m->chunks.size() : 0; }
uint32_t DestructionDX12::GetActorCount() const { return m ? (uint32_t)m->actors.size() : 0; }
const std::vector<DestructionRenderItem>& DestructionDX12::GetRenderItems() const { return m->renderItems; }
