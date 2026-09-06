#ifndef PREFAB_RUNTIME_H
#define PREFAB_RUNTIME_H

#include "CollisionMesh.h"
#include "PrefabColliders.h"
#include "SceneGraph.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct PrefabRenderBatch {
    struct LodModel {
        float distance = 0.0f;
        std::shared_ptr<SceneNode> model;
    };
    std::string prefabId;
    std::shared_ptr<SceneNode> model;
    std::shared_ptr<SceneNode> baseModel;
    std::vector<DirectX::XMMATRIX> baseTransforms;
    std::vector<DirectX::XMMATRIX> transforms;
    std::vector<uint64_t> entityIds;
    std::vector<LodModel> lods;
    bool automaticLod = false;
    DirectX::XMFLOAT3 lodCenter{};
    float lodRadius = 0.0f;
    bool castShadow = true;
};

struct PrefabLightInstance {
    uint64_t entityId = 0;
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    float radius = 5.0f;
};

struct PrefabAudioEmitter {
    uint64_t entityId = 0;
    DirectX::XMFLOAT3 position{};
    std::string path;
    bool loop = false;
    float radius = 15.0f;
};

struct PrefabSpawnPoint {
    uint64_t entityId = 0;
    DirectX::XMFLOAT3 position{};
    float yawRadians = 0.0f;
    std::string enemyType = "bandit";
    uint32_t count = 1;
};

// A placed armory counter. The stock is not stored here: the shop always sells
// the live WeaponCustomization tables, so a weapon or attachment added to that
// data appears on the counter without anyone editing a prefab. What a placement
// carries is where it stands and how close the player has to be to browse it.
struct PrefabArmoryShop {
    uint64_t entityId = 0;
    DirectX::XMFLOAT3 position{};
    float yawRadians = 0.0f;
    // Counter-depth reach, measured from the prefab origin. The player browses
    // from the customer side rather than walking into the table, so this has to
    // clear the table's own half depth or the shop can never be opened.
    float radius = 3.5f;
    // Same vertical band the weapon pickups use: the origin sits on the floor
    // while the camera is at eye height, so the two never share a Y.
    float verticalRange = 3.0f;
    std::string displayName = "ARMORY";
};

// A placed boarding point: walk up, press E, pick an island to fly to. Like the
// armory counter this stores only where it stands and how close the player has
// to be -- the destination list is the shipped island table, so a map added
// there appears on every helicopter without a prefab edit.
struct PrefabTravelPoint {
    uint64_t entityId = 0;
    DirectX::XMFLOAT3 position{};
    float yawRadians = 0.0f;
    float radius = 6.0f;
    // Aircraft are tall, and the prefab origin sits at the skids while the
    // camera is at eye height. A taller band than the armory's keeps the prompt
    // alive when the player stands beside a helicopter on raised ground.
    float verticalRange = 4.0f;
    std::string displayName = "BOARD HELICOPTER";
};

struct PrefabDestructibleInstance {
    uint64_t entityId = 0;
    DirectX::XMFLOAT3 position{};
    float health = 100.0f;
};

// Runtime-owned result of compiling prefab definitions into game-facing data.
// Renderers receive the render batches explicitly; gameplay systems access the
// remaining collections through RuntimeWorld. No subsystem owns free globals.
struct PrefabRuntimeState {
    std::vector<PrefabRenderBatch> renderBatches;
    std::vector<PrefabCollider> colliders;
    // Per-triangle colliders, emitted alongside the bounds box rather than
    // instead of it: consumers that deliberately keep the conservative box
    // (fire spread, navmesh) then need no changes at all, and prefabs without
    // mesh collision behave exactly as before.
    std::vector<CollisionMeshInstance> meshColliders;
    std::vector<PrefabLightInstance> lights;
    std::vector<PrefabAudioEmitter> audioEmitters;
    std::vector<PrefabSpawnPoint> spawnPoints;
    std::vector<PrefabArmoryShop> armoryShops;
    std::vector<PrefabTravelPoint> travelPoints;
    std::vector<PrefabDestructibleInstance> destructibles;
    std::unordered_map<uint64_t, float> health;

    void ClearDerived() {
        renderBatches.clear();
        colliders.clear();
        meshColliders.clear();
        lights.clear();
        audioEmitters.clear();
        spawnPoints.clear();
        armoryShops.clear();
        travelPoints.clear();
        destructibles.clear();
    }

    void ResetGameplayState() {
        health.clear();
    }
};

struct PrefabTransformUpdateResult {
    size_t renderInstances = 0;
    size_t colliders = 0;
    size_t lights = 0;
    size_t audioEmitters = 0;
    size_t spawnPoints = 0;
    size_t armoryShops = 0;
    size_t travelPoints = 0;
    size_t destructibles = 0;
};

enum class PrefabTransformUpdateScope {
    VisualsOnly,
    AllDerived
};

// Applies a root-entity transform delta to every compiled child owned by that
// entity. Child prefab instances deliberately reuse their root entity id, so
// multiplying their existing world matrices preserves the authored child-local
// offsets that replacing them with the new root matrix would destroy.
inline PrefabTransformUpdateResult ApplyPrefabEntityTransformDelta(
        PrefabRuntimeState& state, uint64_t entityId,
        DirectX::FXMMATRIX delta,
        PrefabTransformUpdateScope scope =
            PrefabTransformUpdateScope::AllDerived) {
    using namespace DirectX;
    PrefabTransformUpdateResult result;

    for (PrefabRenderBatch& batch : state.renderBatches) {
        for (size_t index = 0; index < batch.entityIds.size(); ++index) {
            if (batch.entityIds[index] != entityId) continue;
            if (index < batch.baseTransforms.size())
                batch.baseTransforms[index] = batch.baseTransforms[index] * delta;
            if (index < batch.transforms.size())
                batch.transforms[index] = batch.transforms[index] * delta;
            ++result.renderInstances;
        }
    }

    const auto transformPoint = [&](XMFLOAT3& point) {
        XMStoreFloat3(&point, XMVector3TransformCoord(XMLoadFloat3(&point), delta));
    };
    const auto transformYaw = [&](float& yaw) {
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, XMVector3TransformNormal(
            XMVectorSet(std::cos(yaw), 0.0f, std::sin(yaw), 0.0f), delta));
        if (direction.x * direction.x + direction.z * direction.z > 1e-12f)
            yaw = std::atan2(direction.z, direction.x);
    };

    for (PrefabLightInstance& light : state.lights) {
        if (light.entityId != entityId) continue;
        transformPoint(light.position);
        ++result.lights;
    }

    if (scope == PrefabTransformUpdateScope::VisualsOnly)
        return result;

    for (PrefabCollider& collider : state.colliders) {
        if (collider.entityId != entityId) continue;
        const float cosine = std::cos(collider.yawRadians);
        const float sine = std::sin(collider.yawRadians);
        const XMVECTOR worldX = XMVectorSet(cosine, 0.0f, sine, 0.0f);
        const XMVECTOR worldY = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR worldZ = XMVectorSet(-sine, 0.0f, cosine, 0.0f);
        const XMVECTOR movedX = XMVector3TransformNormal(worldX, delta);
        const XMVECTOR movedY = XMVector3TransformNormal(worldY, delta);
        const XMVECTOR movedZ = XMVector3TransformNormal(worldZ, delta);
        collider.halfExtents.x *= XMVectorGetX(XMVector3Length(movedX));
        collider.halfExtents.y *= XMVectorGetX(XMVector3Length(movedY));
        collider.halfExtents.z *= XMVectorGetX(XMVector3Length(movedZ));
        transformPoint(collider.center);
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, movedX);
        if (direction.x * direction.x + direction.z * direction.z > 1e-12f)
            collider.yawRadians = std::atan2(direction.z, direction.x);
        ++result.colliders;
    }

    for (PrefabAudioEmitter& emitter : state.audioEmitters) {
        if (emitter.entityId != entityId) continue;
        transformPoint(emitter.position);
        ++result.audioEmitters;
    }
    for (PrefabSpawnPoint& spawn : state.spawnPoints) {
        if (spawn.entityId != entityId) continue;
        transformPoint(spawn.position);
        transformYaw(spawn.yawRadians);
        ++result.spawnPoints;
    }
    for (PrefabArmoryShop& shop : state.armoryShops) {
        if (shop.entityId != entityId) continue;
        transformPoint(shop.position);
        transformYaw(shop.yawRadians);
        ++result.armoryShops;
    }
    for (PrefabTravelPoint& travel : state.travelPoints) {
        if (travel.entityId != entityId) continue;
        transformPoint(travel.position);
        transformYaw(travel.yawRadians);
        ++result.travelPoints;
    }
    for (PrefabDestructibleInstance& destructible : state.destructibles) {
        if (destructible.entityId != entityId) continue;
        transformPoint(destructible.position);
        ++result.destructibles;
    }
    return result;
}

#endif
