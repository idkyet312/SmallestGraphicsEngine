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
        CHECK(!crate.automaticLod);
        crate.automaticLod = true;
        crate.targetSize = 1.5f;
        crate.useMaterials = false;
        crate.forceDoubleSided = true;
        crate.transparencyPass = "afterWater";
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
            CHECK(loaded->automaticLod);
            CHECK(!loaded->useMaterials);
            CHECK(loaded->forceDoubleSided);
            CHECK(loaded->components.at("staticMesh")
                .at("forceDoubleSided") == true);
            CHECK(loaded->transparencyPass == "afterWater");
            CHECK(loaded->components.at("staticMesh")
                .at("transparencyPass") == "afterWater");
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

        // Rigid-body props. The component is optional and off by default, so a
        // prefab that never mentions it keeps standing still; when present, an
        // absent density falls back to the empty-container figure rather than
        // to zero, which would give Box3D a massless body.
        WriteText("prefabs/heavy.json", R"({
          "schemaVersion":2,"id":"test/heavy","name":"Heavy",
          "components":{"staticMesh":{"path":"models/crate.glb"},
            "collision":{"shape":"box"},"rigidBody":{"density":250.0}}
        })");
        WriteText("prefabs/defaultmass.json", R"({
          "schemaVersion":2,"id":"test/defaultmass","name":"Default Mass",
          "components":{"staticMesh":{"path":"models/crate.glb"},
            "collision":{"shape":"box"},"rigidBody":{}}
        })");
        // A non-positive density has to be rejected at load: it reaches Box3D as
        // a zero-mass dynamic body and takes the solver's inertia tensor with it.
        WriteText("prefabs/weightless.json", R"({
          "schemaVersion":2,"id":"test/weightless","name":"Weightless",
          "components":{"staticMesh":{"path":"models/crate.glb"},
            "collision":{"shape":"box"},"rigidBody":{"density":0.0}}
        })");
        CHECK(registry.Refresh());
        const PrefabAsset* heavy = registry.Find("test/heavy");
        CHECK(heavy != nullptr);
        if (heavy) {
            CHECK(heavy->error.empty());
            CHECK(heavy->rigidBody.enabled);
            CHECK(heavy->rigidBody.density == 250.0f);
        }
        const PrefabAsset* defaultMass = registry.Find("test/defaultmass");
        CHECK(defaultMass != nullptr);
        if (defaultMass) {
            CHECK(defaultMass->error.empty());
            CHECK(defaultMass->rigidBody.enabled);
            CHECK(defaultMass->rigidBody.density == 64.0f);
        }
        // A rejected prefab is not registered under its id, so it is found by
        // scanning the asset list for the failure, the way the malformed-JSON
        // case above does.
        bool sawWeightlessError = false;
        for (const PrefabAsset& asset : registry.Assets())
            sawWeightlessError = sawWeightlessError ||
                (!asset.error.empty() &&
                 asset.definitionPath.filename() == "weightless.json");
        CHECK(sawWeightlessError);
        // The crate never asked for simulation, so it must not have acquired it.
        const PrefabAsset* stillStatic = registry.Find("test/crate");
        CHECK(stillStatic != nullptr);
        if (stillStatic) CHECK(!stillStatic->rigidBody.enabled);

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
            CHECK(variant->transparencyPass == "afterWater");
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

        // The chain-link fence is the spline tool default. It keeps real-world
        // scale (targetSize 0) because the run pitch is its measured 3.108 m
        // footprint. Collision is the bounds box: the panel measures
        // 3.108 x 2.9 x 0.6 m, so the box is already a thin slab that hugs the
        // mesh, and a run of them costs one collider each instead of a
        // per-triangle tree.
        const PrefabAsset* fence = shipped.Find("props/fence");
        CHECK(fence != nullptr);
        if (fence) {
            CHECK(fence->error.empty());
            CHECK(fence->collision == "box");
            CHECK(fence->targetSize == 0.0f);
            // Fence panels are authored-piece NvBlast structures. Generic
            // prefab health would give explosions a second destruction owner.
            CHECK(!fence->destructible.enabled);
            CHECK(std::filesystem::exists(fence->modelPath));
        }

        // The second fence asset breaks exactly like props/fence: one authored
        // panel cut into halves at runtime. It must match that prefab's
        // settings, or it would fall back to the generic destructible path.
        const PrefabAsset* fencePanel = shipped.Find("props/fence_panel");
        CHECK(fencePanel != nullptr);
        if (fencePanel) {
            CHECK(fencePanel->error.empty());
            CHECK(fencePanel->collision == "box");
            CHECK(fencePanel->targetSize == 0.0f);
            CHECK(!fencePanel->destructible.enabled);
            CHECK(std::filesystem::exists(fencePanel->modelPath));
        }

        // The shipping container is a sealed steel box, and the measured mesh
        // says so: 2.4 x 2.4 x 6.2 m with the geometry reaching the bounds on
        // every axis, so the box hugs it with no doorway or overhang to seal
        // off. Its 15995 triangles bought nothing but a per-triangle tree per
        // placement, and levels drop ten of them at a time.
        const PrefabAsset* container = shipped.Find("props/container");
        CHECK(container != nullptr);
        if (container) {
            CHECK(container->error.empty());
            CHECK(container->collision == "box");
            // Simulated, not static: the yard is meant to be shoved around.
            // 64 kg/m^3 over the measured 35.7 m^3 bounds is about 2286 kg,
            // an empty 20ft container.
            CHECK(container->rigidBody.enabled);
            CHECK(container->rigidBody.density == 64.0f);
            CHECK(container->targetSize == 0.0f);
            CHECK(std::filesystem::exists(container->modelPath));
        }

        const PrefabAsset* watchtower = shipped.Find("props/watchtower");
        CHECK(watchtower != nullptr);
        if (watchtower) {
            CHECK(watchtower->error.empty());
            CHECK(watchtower->collision == "mesh");
            // The watchtower is fixed level architecture: it is deliberately
            // kept out of the destruction model, and registering generic prefab
            // health would make explosions able to remove it anyway.
            CHECK(!watchtower->destructible.enabled);
            CHECK(std::filesystem::exists(watchtower->modelPath));
        }

        const PrefabAsset* fuelSilo = shipped.Find("props/fuel_silo");
        CHECK(fuelSilo != nullptr);
        if (fuelSilo) {
            CHECK(fuelSilo->error.empty());
            CHECK(fuelSilo->collision == "mesh");
            CHECK(fuelSilo->targetSize == 0.0f);
            CHECK(std::filesystem::exists(fuelSilo->modelPath));
        }

        // The barrack is a building the player walks into, so it keeps its
        // authored real-world size (measured 5.9 x 4.7 x 19.7 m) and needs
        // per-triangle collision: a box would seal the doorway shut.
        const PrefabAsset* barrack = shipped.Find("props/metal_barrack");
        CHECK(barrack != nullptr);
        if (barrack) {
            CHECK(barrack->error.empty());
            CHECK(barrack->collision == "mesh");
            CHECK(barrack->targetSize == 0.0f);
            CHECK(std::filesystem::exists(barrack->modelPath));
        }

        // Same reasoning as the barrack: a walk-in structure measuring
        // 15.2 x 5.1 x 10.3 m, so it keeps real-world scale and takes
        // per-triangle collision rather than a box across its doorways.
        const PrefabAsset* milBuilding =
            shipped.Find("props/military_building_1");
        CHECK(milBuilding != nullptr);
        if (milBuilding) {
            CHECK(milBuilding->error.empty());
            CHECK(milBuilding->collision == "mesh");
            CHECK(milBuilding->targetSize == 0.0f);
            CHECK(std::filesystem::exists(milBuilding->modelPath));
        }

        // Sandbags are a solid 3.25 x 1.03 x 0.70 m mound with no opening to
        // preserve, so the bounds box already hugs the mesh and costs one
        // collider instead of a per-triangle tree along a defensive line.
        const PrefabAsset* sandbags = shipped.Find("props/sandbag_barrier");
        CHECK(sandbags != nullptr);
        if (sandbags) {
            CHECK(sandbags->error.empty());
            CHECK(sandbags->collision == "box");
            CHECK(sandbags->targetSize == 0.0f);
            CHECK(std::filesystem::exists(sandbags->modelPath));
        }

        // The remaining three keep per-triangle collision: the helideck is a
        // 36 m platform walked on and under, the pipe stack is hollow bores,
        // and the gate's whole point is the opening a box would seal.
        for (const char* id : { "props/helipad", "props/drain_pipes",
                                "props/building_gate" }) {
            const PrefabAsset* prefab = shipped.Find(id);
            CHECK(prefab != nullptr);
            if (prefab) {
                CHECK(prefab->error.empty());
                CHECK(prefab->collision == "mesh");
                CHECK(prefab->targetSize == 0.0f);
                CHECK(std::filesystem::exists(prefab->modelPath));
            }
        }

        // The car park is a surface the player walks and drives on top of
        // (measured 28.2 x 1.0 x 52.1 m, 2142 triangles), so it keeps its
        // authored size and takes per-triangle collision: a box would bury the
        // deck under its own bounding volume. It is also an open shell -- 1574
        // up-facing triangles and no underside at all -- so it needs
        // forceDoubleSided; without it the no-cull forward pass shades the slab
        // inside-out.
        const PrefabAsset* carpark = shipped.Find("props/carpark_asphalt");
        CHECK(carpark != nullptr);
        if (carpark) {
            CHECK(carpark->error.empty());
            CHECK(carpark->collision == "mesh");
            CHECK(carpark->targetSize == 0.0f);
            CHECK(carpark->forceDoubleSided);
            CHECK(std::filesystem::exists(carpark->modelPath));
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
