#include "RuntimeWorld.h"

#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    RuntimeWorld world;
    const uint64_t initialLevelRevision = world.LevelRevision();

    LevelDefinition authored = MakeLevelOneTemplate();
    authored.name = "Runtime Test";
    authored.terrainSculpt.push_back(
        { 2.0f, 3.0f, 4.0f, TerrainSculptOperation::Add, 0.5f, 1.0f });
    world.ReplaceLevel(authored);
    CHECK(world.Level().name == "Runtime Test");
    CHECK(world.TerrainSculpt().size() == authored.terrainSculpt.size());
    CHECK(world.LevelRevision() == initialLevelRevision + 1);

    LevelDefinition edited = world.Level();
    edited.entities.front().transform.position[0] = 42.0f;
    edited.entities.front().enabled = false;
    const uint64_t transformRevision = world.TransformRevision();
    CHECK(world.SynchronizeEditorTransforms(edited));
    CHECK(world.Level().entities.front().transform.position[0] == 42.0f);
    CHECK(!world.Level().entities.front().enabled);
    CHECK(world.TransformRevision() == transformRevision + 1);

    LevelDefinition structuralEdit = edited;
    structuralEdit.entities.pop_back();
    CHECK(!world.SynchronizeEditorTransforms(structuralEdit));
    CHECK(world.Level().entities.size() == edited.entities.size());

    LevelDefinition reordered = edited;
    std::swap(reordered.entities[0], reordered.entities[1]);
    CHECK(!world.SynchronizeEditorTransforms(reordered));
    CHECK(world.Level().entities.front().id == edited.entities.front().id);

    for (size_t i = 0; i < kMaxTerrainSculptStamps + 8; ++i) {
        TerrainSculptStamp stamp;
        stamp.x = static_cast<float>(i);
        world.AddRuntimeTerrainStamp(stamp);
    }
    CHECK(world.TerrainSculpt().size() == kMaxTerrainSculptStamps);
    CHECK(world.TerrainSculpt().back().x ==
          static_cast<float>(kMaxTerrainSculptStamps + 7));
    const size_t transientTerrainCount = world.TerrainSculpt().size();
    LevelDefinition settingsOnly = world.Level();
    settingsOnly.dxrDDGI.intensity = 0.75f;
    world.ReplaceLevel(settingsOnly, RuntimeTerrainSource::Preserve);
    CHECK(world.TerrainSculpt().size() == transientTerrainCount);

    world.Prefabs().health.emplace(7, 100.0f);
    world.Prefabs().renderBatches.emplace_back();
    world.Prefabs().ResetGameplayState();
    CHECK(world.Prefabs().health.empty());
    CHECK(world.Prefabs().renderBatches.size() == 1);
    world.Prefabs().ClearDerived();
    CHECK(world.Prefabs().renderBatches.empty());

    world.ReplaceLevel(MakeLevelOneTemplate(), RuntimeTerrainSource::Empty);
    CHECK(world.TerrainSculpt().empty());

    return failures ? 1 : 0;
}
