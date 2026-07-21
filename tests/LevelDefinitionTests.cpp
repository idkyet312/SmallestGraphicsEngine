#include "LevelDefinition.h"

#include <filesystem>
#include <fstream>
#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
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
    LevelEntity fern;
    fern.id = 1002;
    fern.type = LevelEntityType::Fern;
    fern.name = "Painted Fern";
    level.entities.push_back(fern);

    const auto root = std::filesystem::temp_directory_path() /
                      "smallest-graphics-engine-level-tests";
    const auto good = root / "roundtrip.json";
    CHECK(SaveLevel(level, good).ok);
    LevelLoadResult loaded = LoadLevel(good);
    CHECK(loaded.ok);
    CHECK(loaded.level.entities.size() == level.entities.size());
    CHECK(loaded.level.entities[4].id == level.entities[4].id);
    CHECK(loaded.level.entities[27].type == LevelEntityType::GrassPatch);
    CHECK(loaded.level.entities[28].type == LevelEntityType::Fern);

    LevelDefinition duplicate = level;
    duplicate.entities[1].id = duplicate.entities[0].id;
    CHECK(!ValidateLevel(duplicate).ok);
    LevelDefinition missingPlayer = level;
    missingPlayer.entities[0].enabled = false;
    CHECK(!ValidateLevel(missingPlayer).ok);
    LevelDefinition unknown = level;
    unknown.entities[1].type = static_cast<LevelEntityType>(999);
    CHECK(!ValidateLevel(unknown).ok);

    std::filesystem::create_directories(root);
    const auto malformed = root / "malformed.json";
    { std::ofstream stream(malformed); stream << "{not json"; }
    CHECK(!LoadLevel(malformed).ok);
    const auto future = root / "future.json";
    { std::ofstream stream(future); stream << R"({
      "schemaVersion":2,"name":"Future","terrain":{"heightScale":5},
      "entities":[]})"; }
    CHECK(!LoadLevel(future).ok);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
