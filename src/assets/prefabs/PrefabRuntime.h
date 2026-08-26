#ifndef PREFAB_RUNTIME_H
#define PREFAB_RUNTIME_H

#include "CollisionMesh.h"
#include "PrefabColliders.h"
#include "SceneGraph.h"
#include <DirectXMath.h>
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
    std::vector<PrefabDestructibleInstance> destructibles;
    std::unordered_map<uint64_t, float> health;

    void ClearDerived() {
        renderBatches.clear();
        colliders.clear();
        meshColliders.clear();
        lights.clear();
        audioEmitters.clear();
        spawnPoints.clear();
        destructibles.clear();
    }

    void ResetGameplayState() {
        health.clear();
    }
};

#endif
