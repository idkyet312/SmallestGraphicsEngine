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
        crate.collision = "box";
        crate.light.enabled = true;
        crate.light.intensity = 3.5f;
        crate.components["customComponent"] = {{"kept", 42}};
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
            CHECK(loaded->collision == "box");
            CHECK(loaded->schemaVersion == 2);
            CHECK(loaded->light.enabled);
            CHECK(loaded->light.intensity == 3.5f);
            CHECK(loaded->components.at("customComponent").at("kept") == 42);
        }
        CHECK(registry.Find("model/models/loose.fbx") != nullptr);
        CHECK(!registry.Refresh());
        CHECK(registry.Revision() == firstRevision);

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

        WriteText("prefabs/missing_model.json", R"({
          "schemaVersion":2,"id":"test/missing","components":{
            "staticMesh":{"path":"models/not_here.glb"}}})");
        CHECK(registry.Refresh());
        CHECK(registry.Find("test/missing") == nullptr);

        std::filesystem::path importedPrefab;
        CHECK(PrefabRegistry::ImportModel("models/loose.fbx", importedPrefab).ok);
        CHECK(std::filesystem::exists(importedPrefab));
        CHECK(std::filesystem::exists("models/Imported/loose/loose.fbx"));

        PrefabAsset unsafe = crate;
        unsafe.id = "unsafe";
        unsafe.modelPath = "../outside.glb";
        CHECK(!PrefabRegistry::Save(unsafe, "prefabs/unsafe.json").ok);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
