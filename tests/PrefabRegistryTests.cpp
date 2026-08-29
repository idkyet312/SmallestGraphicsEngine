#include "PrefabRegistry.h"
#include "PrefabColliders.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

struct ScopedCurrentPath {
    std::filesystem::path previous = std::filesystem::current_path();
    explicit ScopedCurrentPath(const std::filesystem::path& path) {
        std::filesystem::current_path(path);
    }
    ~ScopedCurrentPath() { std::filesystem::current_path(previous); }
};

static void WriteText(const std::filesystem::path& path, const std::string& text) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

int main() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("smallest-graphics-engine-prefab-tests-" + unique);
    std::filesystem::create_directories(root / "models");
    {
        ScopedCurrentPath current(root);
        WriteText("models/crate.glb", "fake glb");
        WriteText("models/loose.fbx", "fake fbx");
        WriteText("models/crate_lod.glb", "fake lod");
        WriteText("models/crate.png", "fake texture");

        PrefabAsset crate;
        crate.id = "test/crate";
        crate.name = "Crate";
        crate.modelPath = "models/crate.glb";
        crate.defaultScale[0] = crate.defaultScale[1] = crate.defaultScale[2] = 2.0f;
        crate.targetSize = 1.5f;
        crate.useMaterials = false;
        crate.forceDoubleSided = true;
        crate.materialAmbientScale = 1.25f;
        crate.materialViewFillStrength = 0.1f;
        crate.collision = "box";
        crate.light.enabled = true;
        crate.light.intensity = 3.5f;
        crate.components["customComponent"] = {{"kept", 42}};
        crate.components["staticMesh"]["futureMeshField"] = 17;
        crate.components["light"]["futureLightField"] = "kept";
        crate.document["futureRootField"] = { {"kept", true} };
        CHECK(PrefabRegistry::Save(crate, "prefabs/crate.json").ok);

        PrefabRegistry registry;
        CHECK(registry.Refresh());
        const uint64_t firstRevision = registry.Revision();
        CHECK(firstRevision > 0);
        const PrefabAsset* loaded = registry.Find("test/crate");
        CHECK(loaded != nullptr);
        if (loaded) {
            CHECK(loaded->modelPath == std::filesystem::path("models/crate.glb"));
            CHECK(loaded->targetSize == 1.5f);
            CHECK(!loaded->useMaterials);
            CHECK(loaded->forceDoubleSided);
            CHECK(loaded->components.at("staticMesh")
                .at("forceDoubleSided") == true);
            CHECK(loaded->materialAmbientScale == 1.25f);
            CHECK(loaded->materialViewFillStrength == 0.1f);
            CHECK(loaded->collision == "box");
            CHECK(loaded->schemaVersion == 2);
            CHECK(loaded->light.enabled);
            CHECK(loaded->light.intensity == 3.5f);
            CHECK(loaded->components.at("customComponent").at("kept") == 42);
            CHECK(loaded->components.at("staticMesh").at("futureMeshField") == 17);
            CHECK(loaded->components.at("light").at("futureLightField") == "kept");
            CHECK(loaded->document.at("futureRootField").at("kept") == true);
        }
        CHECK(registry.Find("model/models/loose.fbx") != nullptr);
        CHECK(!registry.Refresh());
        CHECK(registry.Revision() == firstRevision);

        PrefabAsset edited = *registry.Find("test/crate");
        edited.name = "Crate Hot Reloaded";
        CHECK(PrefabRegistry::Save(edited, "prefabs/crate.json").ok);
        CHECK(registry.Refresh());
        CHECK(registry.Revision() > firstRevision);
        const PrefabAsset* hotReloaded = registry.Find("test/crate");
        CHECK(hotReloaded != nullptr);
        if (hotReloaded) {
            CHECK(hotReloaded->name == "Crate Hot Reloaded");
            CHECK(hotReloaded->components.at("staticMesh")
                .at("futureMeshField") == 17);
            CHECK(hotReloaded->document.at("futureRootField").at("kept") == true);
        }

        WriteText("prefabs/bad.json", "{not json");
        CHECK(registry.Refresh());
        bool sawInvalid = false;
        for (const PrefabAsset& asset : registry.Assets())
            sawInvalid = sawInvalid || !asset.error.empty();
        CHECK(sawInvalid);

        WriteText("prefabs/duplicate.json", R"({
          "schemaVersion":1,"id":"test/crate","name":"Duplicate",
          "components":{"staticMesh":{"path":"models/crate.glb"}}
        })");
        CHECK(registry.Refresh());
        size_t duplicateCount = 0;
        size_t duplicateErrors = 0;
        for (const PrefabAsset& asset : registry.Assets()) {
            if (asset.id != "test/crate") continue;
            ++duplicateCount;
            if (!asset.error.empty()) ++duplicateErrors;
        }
        CHECK(duplicateCount == 2);
        CHECK(duplicateErrors == 1);

        WriteText("prefabs/legacy.json", R"({
          "schemaVersion":1,"id":"test/legacy","name":"Legacy",
          "components":{"staticMesh":{"path":"models/crate.glb"},
            "collision":{"shape":"none"},"future":{"value":7}}
        })");
        CHECK(registry.Refresh());
        const PrefabAsset* legacy = registry.Find("test/legacy");
        CHECK(legacy != nullptr);
        if (legacy) {
            CHECK(legacy->schemaVersion == 2);
            CHECK(legacy->components.at("future").at("value") == 7);
            const auto merged = MergePrefabComponents(legacy->components,
                {{"staticMesh", {{"castShadow", false}}},
                 {"light", {{"intensity", 9.0f}}}});
            CHECK(merged.at("staticMesh").at("castShadow") == false);
            CHECK(merged.at("light").at("intensity") == 9.0f);
        }

        WriteText("prefabs/variant.json", R"({
          "schemaVersion":2,"id":"test/variant","name":"Crate Variant",
          "extends":"test/crate","components":{"collision":{"shape":"mesh"}},
          "children":[{"prefab":"test/legacy","position":[1,0,0]}]
        })");
        CHECK(registry.Refresh());
        const PrefabAsset* variant = registry.Find("test/variant");
        CHECK(variant != nullptr);
        if (variant) {
            CHECK(variant->modelPath == std::filesystem::path("models/crate.glb"));
            CHECK(variant->collision == "mesh");
            CHECK(variant->forceDoubleSided);
            CHECK(variant->light.enabled);
            CHECK(variant->children.size() == 1);
            CHECK(variant->children[0].prefabId == "test/legacy");
        }

        PrefabCollider collider;
        collider.center = { 0.0f, 1.0f, 0.0f };
        collider.halfExtents = { 1.0f, 1.0f, 0.5f };
        collider.yawRadians = 0.5f;
        DirectX::XMFLOAT3 hit;
        CHECK(PrefabColliderIntersectsSegment(collider, {-3, 1, 0}, {3, 1, 0},
                                                0.1f, &hit));
        CHECK(!PrefabColliderIntersectsSegment(collider, {-3, 4, 0}, {3, 4, 0},
                                                 0.1f));

        WriteText("models/renamed.glb", "renamed model");
        WriteText("assetcache/registry.json", R"({
          "schemaVersion":3,"assets":[{"guid":"stable-model-guid",
          "path":"models/renamed.glb","kind":"Models"}]})");
        WriteText("prefabs/guid_reference.json", R"({
          "schemaVersion":2,"id":"test/guid-reference","components":{
            "staticMesh":{"path":"models/old_name.glb",
            "assetGuid":"stable-model-guid"}}})");
        CHECK(registry.Refresh());
        const PrefabAsset* guidReference = registry.Find("test/guid-reference");
        CHECK(guidReference != nullptr);
        if (guidReference)
            CHECK(guidReference->modelPath ==
                  std::filesystem::path("models/renamed.glb"));

        WriteText("prefabs/missing_model.json", R"({
          "schemaVersion":2,"id":"test/missing","components":{
            "staticMesh":{"path":"models/not_here.glb"}}})");
        CHECK(registry.Refresh());
        CHECK(registry.Find("test/missing") == nullptr);

        std::filesystem::path importedPrefab;
        CHECK(PrefabRegistry::ImportModel("models/loose.fbx", importedPrefab).ok);
        CHECK(std::filesystem::exists(importedPrefab));
        CHECK(std::filesystem::exists(
            "Content/Models/Imported/loose/loose.fbx"));

        PrefabAsset unsafe = crate;
        unsafe.id = "unsafe";
        unsafe.modelPath = "../outside.glb";
        CHECK(!PrefabRegistry::Save(unsafe, "prefabs/unsafe.json").ok);
    }

    // The shipped prefabs, loaded from the real content tree. A prefab whose
    // model file is missing is dropped silently (see test/missing above), so a
    // typo in a path or a moved asset would otherwise only show up as a prop
    // that quietly fails to appear in the level.
    if (const char* sourceDir = SGE_SOURCE_DIR) {
        ScopedCurrentPath current(sourceDir);
        PrefabRegistry shipped;
        shipped.Refresh("Content/Prefabs", "Content/Models");

        const PrefabAsset* tower = shipped.Find("props/comm_tower");
        CHECK(tower != nullptr);
        if (tower) {
            CHECK(tower->error.empty());
            CHECK(tower->collision != "none");        // else bullets pass through
            CHECK(tower->destructible.enabled);
            CHECK(tower->destructible.health > 0.0f);
            CHECK(tower->targetSize > 0.0f);
            CHECK(std::filesystem::exists(tower->modelPath));
        }

        const PrefabAsset* fuelSilo = shipped.Find("props/fuel_silo");
        CHECK(fuelSilo != nullptr);
        if (fuelSilo) {
            CHECK(fuelSilo->error.empty());
            CHECK(fuelSilo->collision == "mesh");
            CHECK(fuelSilo->targetSize == 0.0f);
            CHECK(std::filesystem::exists(fuelSilo->modelPath));
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
