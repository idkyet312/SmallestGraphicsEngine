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
    AllySpawn,
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

// Hard cap on live sculpt stamps. Bounds the GPU upload buffer
// (kMaxTerrainSculptStamps * 32B) and, more importantly, the per-sample cost:
// both TerrainRendererDX12::HeightAt and terrain_ms.hlsl's TerrainHeight loop
// every stamp for every height query, with no spatial acceleration. Raising
// this scales that loop linearly -- 1024 stamps is ~8x the Level-1 budget and
// still only a 32 KB buffer.
inline constexpr size_t kMaxTerrainSculptStamps = 1024;

// Upper bound on the DDGI probe count a level may request.
//
// This is a budget ceiling, not a hardware or format limit: DXRDDGIRenderer
// sizes the irradiance and visibility atlases from the ACTUAL probe count
// (CreateAtlases lays probes out in a ceil(sqrt(n)) grid of 10x10 and 18x18
// texel tiles), so nothing in the GPU path assumes a maximum.
//
// It was 2048, which is under one probe per 8m tile on a 128x128m island at the
// default 3.0m spacing -- so large outdoor levels silently ran out of probes,
// most of the ground fell outside the layout, and SampleSparseDDGI returned zero
// there. Those pixels then fell through to one stochastic GI ray each, which is
// far noisier and more expensive than the probes would have been.
//
// 8192 costs roughly 35 MB of atlas across both double-buffered targets, which
// is in line with what the visibility buffer already owns. Raise it further only
// with that arithmetic redone -- the atlases grow linearly in probe count.
inline constexpr uint32_t kMaxDDGIProbes = 8192;

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

// How the player arrives at the start of a level.
enum class LevelInsertionMode : uint32_t {
    // Flown in by the BlackHawk. The historical behaviour, and the default for
    // levels saved before this field existed.
    Helicopter = 0,
    // Brought in by the insertion boat instead. For maps that start on water.
    Boat = 1,
    // A double-speed BlackHawk run that holds above the spawn while the player
    // rappels down, instead of landing.
    FastRappel = 2,
    // The player picks between both helicopter runs and the boat before the
    // level starts. Only maps that set this offer the choice.
    PlayerChoice = 3
};

struct LevelDefinition {
    uint32_t schemaVersion = 1;
    std::string name = "Untitled Level";
    // Which insertion delivers the player. PlayerChoice puts the map's arrival
    // up to the player; the other modes settle it in the level file.
    LevelInsertionMode insertionMode = LevelInsertionMode::Helicopter;
    float terrainHeightScale = 3.057f;
    // Island builder: terrain drawn extent (tile grid) and coastline scale. The
    // ocean is procedurally ringed around the land, so growing tiles + island
    // scale together makes a bigger island with more open water around it.
    uint32_t terrainTilesX = 16;
    uint32_t terrainTilesZ = 16;
    // Per-axis island size: the land coastline stretches independently along X
    // and Z, so the island can be a wide oval or a long strip, not just a
    // uniform disc. 1.0 = the original Level 1 radius on that axis.
    float terrainIslandScaleX = 1.0f;
    float terrainIslandScaleZ = 1.0f;
    // Grid min-corner offset in tiles from the origin. Zero keeps the grid
    // centered (legacy behaviour: origin = -tiles/2 .. +tiles/2). Extending an
    // edge in the editor grows one dimension and shifts this so the new tiles
    // appear on the clicked side instead of forcing symmetric growth.
    int32_t terrainOriginTileX = 0;
    int32_t terrainOriginTileZ = 0;
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
const char* LevelInsertionModeName(LevelInsertionMode mode);
bool ParseLevelInsertionMode(const std::string& text, LevelInsertionMode& mode);
LevelDefinition MakeLevelOneTemplate();
LevelValidationResult ValidateLevel(const LevelDefinition& level);
LevelLoadResult LoadLevel(const std::filesystem::path& path);
LevelSaveResult SaveLevel(const LevelDefinition& level,
                          const std::filesystem::path& path);

#endif
