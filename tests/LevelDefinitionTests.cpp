#include "LevelDefinition.h"
#include "BanditWeapon.h"

#include <algorithm>
#include <cmath>
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
    level.deploymentRadius = 137.5f;
    level.patrolBoatEnabled = false;

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
    CHECK(loaded.level.deploymentRadius == 137.5f);
    CHECK(!loaded.level.patrolBoatEnabled);
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
    LevelDefinition bakedTerrain = level;
    bakedTerrain.terrainSculpt.assign(1, heightmapStamp);
    bakedTerrain.terrainSculpt[0].radius = 512.0f;
    bakedTerrain.terrainSculpt[0].replace = 1.0f;
    CHECK(ValidateLevel(bakedTerrain).ok);
    const auto bakedTerrainPath = root / "baked-terrain.json";
    CHECK(SaveLevel(bakedTerrain, bakedTerrainPath).ok);
    const LevelLoadResult bakedTerrainLoaded = LoadLevel(bakedTerrainPath);
    CHECK(bakedTerrainLoaded.ok);
    CHECK(bakedTerrainLoaded.level.terrainSculpt.size() == 1);
    CHECK(bakedTerrainLoaded.level.terrainSculpt[0].radius == 512.0f);
    LevelDefinition oversizedBrush = level;
    oversizedBrush.terrainSculpt[0].radius = 512.0f;
    CHECK(!ValidateLevel(oversizedBrush).ok);
    LevelDefinition oversizedBake = bakedTerrain;
    oversizedBake.terrainSculpt[0].radius =
        kMaxBakedTerrainStampRadius + 1.0f;
    CHECK(!ValidateLevel(oversizedBake).ok);
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
    LevelDefinition invalidDeployment = level;
    invalidDeployment.deploymentRadius = kMaxDeploymentRadius + 1.0f;
    CHECK(!ValidateLevel(invalidDeployment).ok);

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
    CHECK(legacyLoaded.level.deploymentRadius == kDefaultDeploymentRadius);
    CHECK(legacyLoaded.level.patrolBoatEnabled);

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

    // Per-spawner enemy loadout. Stored in the entity's `overrides` blob, so it
    // has to survive a save/load round trip like any other override.
    {
        const auto loadoutPath = root / "loadout.json";
        { std::ofstream stream(loadoutPath); stream << R"({
          "schemaVersion":1,"name":"Loadout",
          "terrain":{"heightScale":5},
          "entities":[{"id":1,"type":"player_spawn","name":"Player",
          "transform":{"position":[0,1.7,0],"rotation":[0,0,0],
          "scale":[1,1,1]}},
          {"id":2,"type":"enemy_spawn","name":"Sniper Post",
          "overrides":{"enemyWeapon":"sniper"},
          "transform":{"position":[4,0,4],"rotation":[0,0,0],
          "scale":[1,1,1]}}]})"; }
        LevelLoadResult loaded = LoadLevel(loadoutPath);
        CHECK(loaded.ok);
        CHECK(loaded.level.entities.size() == 2);
        const LevelEntity& spawner = loaded.level.entities[1];
        CHECK(spawner.type == LevelEntityType::EnemySpawn);
        CHECK(spawner.overrides.value("enemyWeapon", std::string()) == "sniper");

        // Round trip: saving and reloading must not drop the override.
        const auto resaved = root / "loadout-resaved.json";
        CHECK(SaveLevel(loaded.level, resaved).ok);
        LevelLoadResult reloaded = LoadLevel(resaved);
        CHECK(reloaded.ok);
        CHECK(reloaded.level.entities.size() == 2);
        CHECK(reloaded.level.entities[1].overrides.value(
                  "enemyWeapon", std::string()) == "sniper");

        // A spawner with no authored loadout keeps an empty overrides object,
        // which is what makes "Random" cost nothing in the level file.
        CHECK(loaded.level.entities[0].overrides.empty());
    }

    // The editor writes these ids as plain strings (it deliberately does not
    // include the gameplay headers), so the two lists can only be kept in step
    // by pinning them here. If a name changes on one side, this fails rather
    // than silently turning authored snipers back into riflemen.
    {
        BanditWeapon parsed = BanditWeapon::Rifle;
        CHECK(ParseBanditWeapon("rifle", parsed) &&
              parsed == BanditWeapon::Rifle);
        CHECK(ParseBanditWeapon("shotgun", parsed) &&
              parsed == BanditWeapon::Shotgun);
        CHECK(ParseBanditWeapon("sniper", parsed) &&
              parsed == BanditWeapon::Sniper);
        CHECK(!ParseBanditWeapon("machinegun", parsed));
        CHECK(std::string(BanditWeaponName(BanditWeapon::Rifle)) == "rifle");
        CHECK(std::string(BanditWeaponName(BanditWeapon::Shotgun)) == "shotgun");
        CHECK(std::string(BanditWeaponName(BanditWeapon::Sniper)) == "sniper");
    }

    // Spline runs. The chain-link fence prefab starts at its origin and measures
    // 3.108 m along +X, so fitted pieces must share endpoints all the way around
    // a run instead of leaving a remainder gap at its end or closed seam.
    {
        LevelSplinePath spline;
        spline.id = 5001;
        spline.name = "Fence Run";
        spline.prefabId = "props/fence";
        spline.spacing = 3.108f;
        spline.conformToTerrain = false;
        spline.pitchToSlope = false;
        spline.points.push_back({ { 0.0f, 0.0f, 0.0f } });
        spline.points.push_back({ { 30.0f, 0.0f, 0.0f } });
        const std::vector<SplineSegmentPlacement> segments =
            EvaluateSplineSegments(spline, nullptr);
        CHECK(segments.size() == 10);
        CHECK(std::fabs(segments.front().position[0]) < 1e-3f);
        // Modelled along +X, so a run heading +X needs no yaw.
        CHECK(std::fabs(segments.front().yawDegrees) < 0.5f);
        const auto segmentEnd = [&](const SplineSegmentPlacement& segment) {
            const float yaw = segment.yawDegrees * 0.0174532925199433f;
            const float pitch = segment.pitchDegrees * 0.0174532925199433f;
            const float length = spline.spacing * segment.lengthScale;
            const float horizontal = length * std::cos(pitch);
            return std::array<float, 3>{
                segment.position[0] + horizontal * std::cos(yaw),
                segment.position[1] - length * std::sin(pitch),
                segment.position[2] - horizontal * std::sin(yaw)
            };
        };
        float worst = 0.0f;
        for (size_t i = 1; i < segments.size(); ++i) {
            const std::array<float, 3> end = segmentEnd(segments[i - 1]);
            const float dx = segments[i].position[0] - end[0];
            const float dz = segments[i].position[2] - end[2];
            worst = std::max(worst, std::sqrt(dx * dx + dz * dz));
        }
        CHECK(worst < 1e-3f);
        CHECK(std::fabs(segmentEnd(segments.back())[0] - 30.0f) < 1e-3f);

        // Arc-length stepping must hold spacing around a corner too: sampling
        // the curve at uniform t instead bunches segments on the turn.
        LevelSplinePath corner = spline;
        corner.id = 5002;
        corner.points.clear();
        corner.points.push_back({ { 0.0f, 0.0f, 0.0f } });
        corner.points.push_back({ { 20.0f, 0.0f, 0.0f } });
        corner.points.push_back({ { 20.0f, 0.0f, 20.0f } });
        const std::vector<SplineSegmentPlacement> turned =
            EvaluateSplineSegments(corner, nullptr);
        CHECK(turned.size() > 10);
        worst = 0.0f;
        for (size_t i = 1; i < turned.size(); ++i) {
            const std::array<float, 3> end = segmentEnd(turned[i - 1]);
            const float dx = turned[i].position[0] - end[0];
            const float dz = turned[i].position[2] - end[2];
            worst = std::max(worst, std::sqrt(dx * dx + dz * dz));
        }
        CHECK(worst < 1e-3f);
        const std::array<float, 3> cornerEnd = segmentEnd(turned.back());
        CHECK(std::fabs(cornerEnd[0] - 20.0f) < 1e-3f);
        CHECK(std::fabs(cornerEnd[2] - 20.0f) < 1e-3f);

        LevelSplinePath loop = spline;
        loop.id = 5003;
        loop.closed = true;
        loop.points.clear();
        loop.points.push_back({ { 0.0f, 0.0f, 0.0f } });
        loop.points.push_back({ { 15.0f, 0.0f, 0.0f } });
        loop.points.push_back({ { 15.0f, 0.0f, 15.0f } });
        loop.points.push_back({ { 0.0f, 0.0f, 15.0f } });
        const std::vector<SplineSegmentPlacement> closed =
            EvaluateSplineSegments(loop, nullptr);
        CHECK(!closed.empty());
        worst = 0.0f;
        for (size_t i = 0; i < closed.size(); ++i) {
            const std::array<float, 3> end = segmentEnd(closed[i]);
            const SplineSegmentPlacement& next = closed[(i + 1) % closed.size()];
            const float dx = next.position[0] - end[0];
            const float dz = next.position[2] - end[2];
            worst = std::max(worst, std::sqrt(dx * dx + dz * dz));
        }
        CHECK(worst < 1e-3f);

        // Degenerate input must not hang or emit nonsense.
        LevelSplinePath lone;
        lone.points.push_back({ { 0.0f, 0.0f, 0.0f } });
        CHECK(EvaluateSplineSegments(lone, nullptr).empty());
        LevelSplinePath zeroSpacing = spline;
        zeroSpacing.spacing = 0.0f;
        CHECK(EvaluateSplineSegments(zeroSpacing, nullptr).empty());
        LevelSplinePath duplicate = spline;
        duplicate.points.clear();
        duplicate.points.push_back({ { 5.0f, 1.0f, 5.0f } });
        duplicate.points.push_back({ { 5.0f, 1.0f, 5.0f } });
        CHECK(EvaluateSplineSegments(duplicate, nullptr).size() <= 1);
    }

    // Splines round-trip as control points, and their baked segments are
    // regenerated rather than saved -- otherwise every save/load would
    // duplicate the whole run.
    {
        LevelDefinition splined = MakeFlatLevelTemplate();
        LevelSplinePath spline;
        spline.id = 6001;
        spline.name = "Perimeter";
        spline.prefabId = "props/fence";
        spline.conformToTerrain = false;
        spline.pitchToSlope = false;
        spline.points.push_back({ { 0.0f, 0.0f, 0.0f } });
        spline.points.push_back({ { 12.0f, 0.0f, 0.0f } });
        splined.splines.push_back(spline);
        CHECK(ValidateLevel(splined).ok);

        uint64_t nextId = 9000;
        const size_t authored = splined.entities.size();
        BakeSplineEntities(splined, nextId, nullptr);
        const size_t baked = splined.entities.size() - authored;
        CHECK(baked > 0);
        CHECK(splined.entities[authored].transform.scale[0] < 1.0f);
        // Re-baking replaces its own segments instead of stacking them, and
        // leaves the hand-placed player spawn alone.
        BakeSplineEntities(splined, nextId, nullptr);
        CHECK(splined.entities.size() == authored + baked);

        const std::filesystem::path splinePath = root / "spline.json";
        CHECK(SaveLevel(splined, splinePath).ok);
        const LevelLoadResult reloaded = LoadLevel(splinePath);
        CHECK(reloaded.ok);
        CHECK(reloaded.level.splines.size() == 1);
        CHECK(reloaded.level.splines.front().id == 6001);
        CHECK(reloaded.level.splines.front().prefabId == "props/fence");
        CHECK(reloaded.level.splines.front().points.size() == 2);
        // Segments were not persisted, so the reloaded level holds only the
        // authored entities until it is baked again.
        CHECK(reloaded.level.entities.size() == authored);
    }

    // A level saved before splines existed must still load.
    {
        const std::filesystem::path legacyPath = root / "legacy.json";
        CHECK(SaveLevel(MakeFlatLevelTemplate(), legacyPath).ok);
        std::ifstream legacyIn(legacyPath);
        const std::string text((std::istreambuf_iterator<char>(legacyIn)),
                               std::istreambuf_iterator<char>());
        legacyIn.close();
        // No splines authored, so the key must not even be written.
        CHECK(text.find("splines") == std::string::npos);
        const LevelLoadResult legacy = LoadLevel(legacyPath);
        CHECK(legacy.ok);
        CHECK(legacy.level.splines.empty());
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
