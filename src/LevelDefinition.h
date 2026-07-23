#ifndef LEVEL_DEFINITION_H
#define LEVEL_DEFINITION_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

enum class LevelEntityType {
    PlayerSpawn,
    WoodHouse,
    MetalHouse,
    Palm,
    ExplosiveBarrel,
    EnemySpawn,
    Humvee,
    Helicopter,
    GrassPatch,
    Dandelion,
    Rock,
    Prefab
};

struct Transform {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
};

struct LevelEntity {
    uint64_t id = 0;
    LevelEntityType type = LevelEntityType::EnemySpawn;
    std::string name;
    // Registry ID for generic data-driven entities. Built-in entity types leave
    // this empty; Prefab entities resolve it through PrefabRegistry.
    std::string prefabId;
    // Per-instance component values. Stored verbatim and recursively merged on
    // top of prefab defaults at runtime.
    nlohmann::json overrides = nlohmann::json::object();
    Transform transform;
    bool enabled = true;
};

enum class TerrainSculptOperation : uint32_t {
    Add = 0,
    Flatten = 1
};

struct TerrainSculptStamp {
    float x = 0.0f;
    float z = 0.0f;
    float radius = 2.0f;
    TerrainSculptOperation operation = TerrainSculptOperation::Add;
    float value = 0.0f;
    float strength = 1.0f;
};

struct LevelDXRDDGISettings {
    bool enabled = false;
    float surfaceSpacing = 3.0f;
    float surfaceOffset = 0.35f;
    uint32_t maxProbes = 2048;
    uint32_t raysPerProbe = 64;
    uint32_t probesPerFrame = 16;
    float maxRayDistance = 24.0f;
    float intensity = 0.45f;
    float normalBias = 0.18f;
    float viewBias = 0.05f;
    float hysteresis = 0.95f;
    float multiBounceStrength = 0.35f;
    bool showProbes = false;
};

struct LevelDefinition {
    uint32_t schemaVersion = 1;
    std::string name = "Untitled Level";
    float terrainHeightScale = 5.0f;
    std::vector<TerrainSculptStamp> terrainSculpt;
    LevelDXRDDGISettings dxrDDGI;
    std::vector<LevelEntity> entities;
};

struct LevelValidationResult {
    bool ok = false;
    std::vector<std::string> errors;
};

struct LevelLoadResult {
    bool ok = false;
    LevelDefinition level;
    std::string error;
};

struct LevelSaveResult {
    bool ok = false;
    std::string error;
};

const char* LevelEntityTypeName(LevelEntityType type);
bool ParseLevelEntityType(const std::string& text, LevelEntityType& type);
LevelDefinition MakeLevelOneTemplate();
LevelValidationResult ValidateLevel(const LevelDefinition& level);
LevelLoadResult LoadLevel(const std::filesystem::path& path);
LevelSaveResult SaveLevel(const LevelDefinition& level,
                          const std::filesystem::path& path);

#endif
