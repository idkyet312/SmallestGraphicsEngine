#include "LevelDefinition.h"

#include <filesystem>
#include <fstream>
#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    // The flat template must be a valid, spawnable level on its own: it is what
    // the editor's "New Flat" button drops the user into.
    {
        const LevelDefinition flat = MakeFlatLevelTemplate();
        CHECK(ValidateLevel(flat).ok);
        CHECK(flat.terrainFlat);
        CHECK(flat.entities.size() == 1);
        CHECK(flat.entities.front().type == LevelEntityType::PlayerSpawn);
        CHECK(flat.terrainSculpt.empty());
    }
    // Procedural levels must stay procedural: terrainFlat defaults off, so a
    // level saved before the flag existed still loads as its island.
    CHECK(!MakeLevelOneTemplate().terrainFlat);

    LevelDefinition level = MakeLevelOneTemplate();
    CHECK(ValidateLevel(level).ok);
    CHECK(level.entities.size() == 27);
    CHECK(level.entities.front().type == LevelEntityType::PlayerSpawn);
    LevelEntity grass;
    grass.id = 1001;
    grass.type = LevelEntityType::GrassPatch;
    grass.name = "Painted Grass";
    grass.transform.scale[0] = grass.transform.scale[2] = 3.0f;
    grass.transform.scale[1] = 1.5f;
    level.entities.push_back(grass);
    LevelEntity dandelion;
    dandelion.id = 1002;
    dandelion.type = LevelEntityType::Dandelion;
    dandelion.name = "Painted Dandelion";
    level.entities.push_back(dandelion);
    LevelEntity rock;
    rock.id = 1003;
    rock.type = LevelEntityType::Rock;
    rock.name = "Editor Rock";
    level.entities.push_back(rock);
    LevelEntity prefabEntity;
    prefabEntity.id = 1004;
    prefabEntity.type = LevelEntityType::Prefab;
    prefabEntity.name = "Data Driven Crate";
    prefabEntity.prefabId = "test/crate";
    prefabEntity.overrides = { {"staticMesh", {{"castShadow", false}}},
                               {"light", {{"intensity", 4.0f}}} };
    level.entities.push_back(prefabEntity);
    level.terrainSculpt.push_back({ 3.0f, -4.0f, 2.5f,
        TerrainSculptOperation::Add, 0.75f, 1.0f });
    level.terrainSculpt.push_back({ 0.0f, 1.0f, 4.0f,
        TerrainSculptOperation::Flatten, 2.5f, 0.6f });
    TerrainSculptStamp heightmapStamp;
    heightmapStamp.x = -7.0f;
    heightmapStamp.z = 9.0f;
    heightmapStamp.radius = 12.0f;
    heightmapStamp.operation = TerrainSculptOperation::Heightmap;
    heightmapStamp.value = 5.0f;
    heightmapStamp.texture = "HM_Craters_01_Ex.PNG";
    heightmapStamp.rotation = 35.0f;
    heightmapStamp.replace = 0.75f;
    heightmapStamp.baseHeight = 3.25f;
    level.terrainSculpt.push_back(heightmapStamp);
    // Flip off the default so the round-trip below actually proves the flag is
    // written and read back, rather than comparing false to false.
    level.terrainFlat = true;
    level.dxrDDGI.enabled = true;
    level.dxrDDGI.surfaceSpacing = 2.5f;
    level.dxrDDGI.surfaceOffset = 0.4f;
    level.dxrDDGI.maxProbes = 1024;
    level.dxrDDGI.raysPerProbe = 32;
    level.dxrDDGI.probesPerFrame = 8;
    level.dxrDDGI.maxRayDistance = 42.0f;
    level.dxrDDGI.hysteresis = 0.9f;
    level.insertionMode = LevelInsertionMode::PlayerChoice;

    const auto root = std::filesystem::temp_directory_path() /
                      "smallest-graphics-engine-level-tests";
    const auto good = root / "roundtrip.json";
    CHECK(SaveLevel(level, good).ok);
    LevelLoadResult loaded = LoadLevel(good);
    CHECK(loaded.ok);
    CHECK(loaded.level.entities.size() == level.entities.size());
    CHECK(loaded.level.entities[4].id == level.entities[4].id);
    CHECK(loaded.level.entities[27].type == LevelEntityType::GrassPatch);
    CHECK(loaded.level.entities[28].type == LevelEntityType::Dandelion);
    CHECK(loaded.level.entities[29].type == LevelEntityType::Rock);
    CHECK(std::string(LevelEntityTypeName(loaded.level.entities[29].type)) == "rock");
    CHECK(loaded.level.entities[30].type == LevelEntityType::Prefab);
    CHECK(loaded.level.entities[30].prefabId == "test/crate");
    CHECK(loaded.level.entities[30].overrides == prefabEntity.overrides);
    LevelEntityType legacyFoliage = LevelEntityType::GrassPatch;
    CHECK(ParseLevelEntityType("fern", legacyFoliage));
    CHECK(legacyFoliage == LevelEntityType::Dandelion);
    CHECK(std::string(LevelEntityTypeName(legacyFoliage)) == "dandelion");
    CHECK(loaded.level.terrainSculpt.size() == 3);
    CHECK(loaded.level.terrainSculpt[1].operation ==
          TerrainSculptOperation::Flatten);
    CHECK(loaded.level.terrainSculpt[2].operation ==
          TerrainSculptOperation::Heightmap);
    CHECK(loaded.level.terrainSculpt[2].texture == "HM_Craters_01_Ex.PNG");
    CHECK(loaded.level.terrainSculpt[2].rotation == 35.0f);
    CHECK(loaded.level.terrainFlat);
    CHECK(loaded.level.terrainSculpt[2].replace == 0.75f);
    CHECK(loaded.level.terrainSculpt[2].baseHeight == 3.25f);
    // Additive is the default, so a stamp saved before replace mode existed
    // must still load as the additive stamp it was authored as.
    CHECK(loaded.level.terrainSculpt[0].replace == 0.0f);
    CHECK(loaded.level.dxrDDGI.enabled);
    CHECK(loaded.level.dxrDDGI.surfaceSpacing == 2.5f);
    CHECK(loaded.level.dxrDDGI.surfaceOffset == 0.4f);
    CHECK(loaded.level.dxrDDGI.maxProbes == 1024);
    CHECK(loaded.level.dxrDDGI.raysPerProbe == 32);
    CHECK(loaded.level.dxrDDGI.probesPerFrame == 8);
    CHECK(loaded.level.dxrDDGI.maxRayDistance == 42.0f);
    CHECK(loaded.level.dxrDDGI.hysteresis == 0.9f);
    CHECK(loaded.level.insertionMode == LevelInsertionMode::PlayerChoice);
    LevelInsertionMode insertion = LevelInsertionMode::Helicopter;
    CHECK(ParseLevelInsertionMode("boat", insertion));
    CHECK(insertion == LevelInsertionMode::Boat);
    CHECK(std::string(LevelInsertionModeName(insertion)) == "boat");
    CHECK(ParseLevelInsertionMode("fast_rappel", insertion));
    CHECK(insertion == LevelInsertionMode::FastRappel);
    CHECK(std::string(LevelInsertionModeName(insertion)) == "fast_rappel");
    CHECK(!ParseLevelInsertionMode("submarine", insertion));
    // Unchanged by the failed parse.
    CHECK(insertion == LevelInsertionMode::FastRappel);

    LevelDefinition duplicate = level;
    duplicate.entities[1].id = duplicate.entities[0].id;
    CHECK(!ValidateLevel(duplicate).ok);
    LevelDefinition missingPlayer = level;
    missingPlayer.entities[0].enabled = false;
    CHECK(!ValidateLevel(missingPlayer).ok);
    LevelDefinition unknown = level;
    unknown.entities[1].type = static_cast<LevelEntityType>(999);
    CHECK(!ValidateLevel(unknown).ok);
    LevelDefinition invalidSculpt = level;
    invalidSculpt.terrainSculpt[0].radius = 0.0f;
    CHECK(!ValidateLevel(invalidSculpt).ok);
    LevelDefinition missingStampTexture = level;
    missingStampTexture.terrainSculpt[2].texture.clear();
    CHECK(!ValidateLevel(missingStampTexture).ok);
    LevelDefinition missingPrefabId = level;
    missingPrefabId.entities.back().prefabId.clear();
    CHECK(!ValidateLevel(missingPrefabId).ok);
    // Keyed to the cap rather than a literal: the previous 4096 became a valid
    // probe count when the ceiling rose, so the case silently stopped testing
    // rejection and started asserting the opposite of what it claimed.
    LevelDefinition invalidGI = level;
    invalidGI.dxrDDGI.maxProbes = kMaxDDGIProbes + 1u;
    CHECK(!ValidateLevel(invalidGI).ok);
    LevelDefinition boundaryGI = level;
    boundaryGI.dxrDDGI.maxProbes = kMaxDDGIProbes;
    CHECK(ValidateLevel(boundaryGI).ok);

    std::filesystem::create_directories(root);
    const auto malformed = root / "malformed.json";
    { std::ofstream stream(malformed); stream << "{not json"; }
    CHECK(!LoadLevel(malformed).ok);
    const auto future = root / "future.json";
    { std::ofstream stream(future); stream << R"({
      "schemaVersion":2,"name":"Future","terrain":{"heightScale":5},
      "entities":[]})"; }
    CHECK(!LoadLevel(future).ok);
    const auto legacy = root / "legacy.json";
    { std::ofstream stream(legacy); stream << R"({
      "schemaVersion":1,"name":"Legacy","terrain":{"heightScale":5},
      "entities":[{"id":1,"type":"player_spawn","name":"Player",
      "transform":{"position":[0,1.7,0],"rotation":[0,0,0],
      "scale":[1,1,1]}}]})"; }
    LevelLoadResult legacyLoaded = LoadLevel(legacy);
    CHECK(legacyLoaded.ok);
    CHECK(!legacyLoaded.level.dxrDDGI.enabled);
    // Files saved before insertion modes existed all arrived by helicopter.
    CHECK(legacyLoaded.level.insertionMode == LevelInsertionMode::Helicopter);

    // An unknown mode is rejected rather than silently falling back, so a typo
    // in a hand-edited level is caught at load.
    const auto badInsertion = root / "bad-insertion.json";
    { std::ofstream stream(badInsertion); stream << R"({
      "schemaVersion":1,"name":"Bad","insertionMode":"submarine",
      "terrain":{"heightScale":5},
      "entities":[{"id":1,"type":"player_spawn","name":"Player",
      "transform":{"position":[0,1.7,0],"rotation":[0,0,0],
      "scale":[1,1,1]}}]})"; }
    CHECK(!LoadLevel(badInsertion).ok);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
