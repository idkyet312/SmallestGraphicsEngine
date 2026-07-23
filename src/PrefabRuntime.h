#ifndef PREFAB_RUNTIME_H
#define PREFAB_RUNTIME_H

#include "SceneGraph.h"
#include <DirectXMath.h>
#include <memory>
#include <string>
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

extern std::vector<PrefabRenderBatch> g_prefabRenderBatches;
extern std::vector<PrefabLightInstance> g_prefabLightInstances;
extern std::vector<PrefabAudioEmitter> g_prefabAudioEmitters;
extern std::vector<PrefabSpawnPoint> g_prefabSpawnPoints;
extern std::vector<PrefabDestructibleInstance> g_prefabDestructibles;

#endif
