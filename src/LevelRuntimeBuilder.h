#ifndef LEVEL_RUNTIME_BUILDER_H
#define LEVEL_RUNTIME_BUILDER_H

#include "LevelDefinition.h"

#include <optional>
#include <vector>

struct RuntimeLevelPlan {
    float terrainHeightScale = 5.0f;
    uint32_t terrainTilesX = 16;
    uint32_t terrainTilesZ = 16;
    float terrainIslandScaleX = 1.0f;
    float terrainIslandScaleZ = 1.0f;
    int32_t terrainOriginTileX = 0;
    int32_t terrainOriginTileZ = 0;
    LevelDXRDDGISettings dxrDDGI;
    std::optional<Transform> playerSpawn;
    std::optional<Transform> humveeSpawn;
    std::optional<Transform> helicopterSpawn;
    std::vector<Transform> explosiveBarrels;
};

class LevelRuntimeBuilder {
public:
    static RuntimeLevelPlan Build(const LevelDefinition& level) {
        RuntimeLevelPlan plan;
        plan.terrainHeightScale = level.terrainHeightScale;
        plan.terrainTilesX = level.terrainTilesX;
        plan.terrainTilesZ = level.terrainTilesZ;
        plan.terrainIslandScaleX = level.terrainIslandScaleX;
        plan.terrainIslandScaleZ = level.terrainIslandScaleZ;
        plan.terrainOriginTileX = level.terrainOriginTileX;
        plan.terrainOriginTileZ = level.terrainOriginTileZ;
        plan.dxrDDGI = level.dxrDDGI;

        for (const LevelEntity& entity : level.entities) {
            if (!entity.enabled) continue;
            switch (entity.type) {
            case LevelEntityType::PlayerSpawn:
                if (!plan.playerSpawn) plan.playerSpawn = entity.transform;
                break;
            case LevelEntityType::Humvee:
                if (!plan.humveeSpawn) plan.humveeSpawn = entity.transform;
                break;
            case LevelEntityType::Helicopter:
                if (!plan.helicopterSpawn)
                    plan.helicopterSpawn = entity.transform;
                break;
            case LevelEntityType::ExplosiveBarrel:
                plan.explosiveBarrels.push_back(entity.transform);
                break;
            default:
                break;
            }
        }
        return plan;
    }
};

#endif
