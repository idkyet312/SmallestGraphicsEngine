#include "RuntimeWorld.h"

#include <cmath>
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

    PrefabRuntimeState transformed;
    PrefabRenderBatch batch;
    batch.entityIds = { 17, 17 };
    batch.baseTransforms = {
        DirectX::XMMatrixTranslation(10.0f, 0.0f, 0.0f),
        DirectX::XMMatrixTranslation(12.0f, 0.0f, 0.0f)
    };
    batch.transforms = batch.baseTransforms;
    transformed.renderBatches.push_back(std::move(batch));
    transformed.colliders.push_back(
        { 17, "nested", { 12.0f, 1.0f, 0.0f }, { 2.0f, 1.0f, 3.0f }, 0.0f });
    transformed.lights.push_back(
        { 17, { 12.0f, 3.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, 2.0f, 8.0f });
    transformed.audioEmitters.push_back(
        { 17, { 11.0f, 0.0f, 0.0f }, "test.wav", true, 10.0f });
    transformed.spawnPoints.push_back(
        { 17, { 13.0f, 0.0f, 0.0f }, 0.0f, "bandit", 1 });
    transformed.destructibles.push_back(
        { 17, { 14.0f, 0.0f, 0.0f }, 100.0f });
    const PrefabTransformUpdateResult moved = ApplyPrefabEntityTransformDelta(
        transformed, 17, DirectX::XMMatrixTranslation(10.0f, 0.0f, 0.0f));
    DirectX::XMFLOAT4X4 rootMatrix;
    DirectX::XMFLOAT4X4 childMatrix;
    DirectX::XMStoreFloat4x4(
        &rootMatrix, transformed.renderBatches[0].baseTransforms[0]);
    DirectX::XMStoreFloat4x4(
        &childMatrix, transformed.renderBatches[0].baseTransforms[1]);
    CHECK(std::abs(rootMatrix._41 - 20.0f) < 1e-5f);
    CHECK(std::abs(childMatrix._41 - 22.0f) < 1e-5f);
    CHECK(std::abs((childMatrix._41 - rootMatrix._41) - 2.0f) < 1e-5f);
    CHECK(std::abs(transformed.colliders[0].center.x - 22.0f) < 1e-5f);
    CHECK(std::abs(transformed.lights[0].position.x - 22.0f) < 1e-5f);
    CHECK(std::abs(transformed.audioEmitters[0].position.x - 21.0f) < 1e-5f);
    CHECK(std::abs(transformed.spawnPoints[0].position.x - 23.0f) < 1e-5f);
    CHECK(std::abs(transformed.destructibles[0].position.x - 24.0f) < 1e-5f);
    CHECK(moved.renderInstances == 2);
    CHECK(moved.colliders == 1);
    CHECK(moved.lights == 1);

    PrefabRuntimeState visualOnly;
    visualOnly.renderBatches = transformed.renderBatches;
    visualOnly.colliders.push_back(
        { 29, "preview", { 4.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, 0.0f });
    visualOnly.lights.push_back(
        { 29, { 4.0f, 2.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, 1.0f, 4.0f });
    visualOnly.renderBatches[0].entityIds[0] = 29;
    const PrefabTransformUpdateResult previewMoved =
        ApplyPrefabEntityTransformDelta(visualOnly, 29,
            DirectX::XMMatrixTranslation(3.0f, 0.0f, 0.0f),
            PrefabTransformUpdateScope::VisualsOnly);
    CHECK(previewMoved.renderInstances == 1);
    CHECK(previewMoved.lights == 1);
    CHECK(previewMoved.colliders == 0);
    CHECK(std::abs(visualOnly.lights[0].position.x - 7.0f) < 1e-5f);
    CHECK(std::abs(visualOnly.colliders[0].center.x - 4.0f) < 1e-5f);

    world.ReplaceLevel(MakeLevelOneTemplate(), RuntimeTerrainSource::Empty);
    CHECK(world.TerrainSculpt().empty());

    return failures ? 1 : 0;
}
