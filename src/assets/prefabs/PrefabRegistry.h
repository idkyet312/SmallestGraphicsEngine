#ifndef PREFAB_REGISTRY_H
#define PREFAB_REGISTRY_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct PrefabLightComponent {
    bool enabled = false;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float intensity = 1.0f;
    float radius = 5.0f;
};

struct PrefabAudioComponent {
    bool enabled = false;
    std::filesystem::path path;
    bool loop = false;
    float radius = 15.0f;
};

struct PrefabDestructibleComponent {
    bool enabled = false;
    float health = 100.0f;
};

// A prop that simulates as a rigid body instead of standing still. Only
// meaningful with box collision: the simulated hull is the prefab's measured
// bounds, which is the same shape the static box collider uses.
struct PrefabRigidBodyComponent {
    bool enabled = false;
    // kg/m^3. The default is the measured density of an empty 6.1 m shipping
    // container: about 2300 kg over its 35.7 m^3 bounding volume. Steel is far
    // denser, but the box is mostly air, and it is the bounding volume the
    // hull actually occupies.
    float density = 64.0f;
};

struct PrefabSpawnerComponent {
    bool enabled = false;
    std::string enemyType = "bandit";
    uint32_t count = 1;
};

enum class PrefabPropertyType { Boolean, Number, Integer, String, Color3, Path };

struct PrefabPropertyDescriptor {
    const char* component;
    const char* field;
    PrefabPropertyType type;
    float minimum = 0.0f;
    float maximum = 0.0f;
};

struct PrefabChildAsset {
    std::string prefabId;
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
};

struct PrefabMaterialOverride {
    std::string mesh;
    std::filesystem::path texture;
};

struct PrefabLodAsset {
    std::filesystem::path path;
    std::string assetGuid;
    float distance = 0.0f;
};

const std::vector<PrefabPropertyDescriptor>& PrefabPropertyMetadata();

struct PrefabAsset {
    uint32_t schemaVersion = 1;
    std::string id;
    std::string name;
    std::string basePrefabId;
    std::vector<PrefabChildAsset> children;
    std::vector<PrefabMaterialOverride> materialOverrides;
    std::vector<PrefabLodAsset> lods;
    std::vector<std::string> warnings;
    std::filesystem::path definitionPath;
    std::filesystem::path modelPath;
    std::string modelGuid;
    float defaultScale[3] = { 1.0f, 1.0f, 1.0f };
    // Optional import normalization. Largest model dimension becomes this many
    // metres and model is grounded at local Y=0.
    float targetSize = 0.0f;
    bool castShadow = true;
    bool useMaterials = true;
    // Opt-in import override. Open-shell models (single-sided sheet geometry
    // with no wall thickness) disappear when back-face culled from the inside,
    // so they can request double-sided rendering regardless of the material
    // flags the source file shipped with.
    bool forceDoubleSided = false;
    // auto | beforeWater | afterWater. Auto handles ordinary props from their
    // bounds; an explicit phase is for large transparent meshes spanning a
    // shoreline or water plane.
    std::string transparencyPass = "auto";
    float materialAmbientScale = 1.0f;
    float materialViewFillStrength = 0.0f;
    std::string collision = "none";
    PrefabLightComponent light;
    PrefabAudioComponent audio;
    PrefabDestructibleComponent destructible;
    PrefabRigidBodyComponent rigidBody;
    PrefabSpawnerComponent spawner;
    // Original component object. Unknown component fields survive load/save and
    // provide data for generic editor overrides.
    nlohmann::json components = nlohmann::json::object();
    // Migrated source document. Unknown top-level fields survive editor saves.
    nlohmann::json document = nlohmann::json::object();
    std::filesystem::path scriptPath;
    bool generated = false;
    std::string error;
};

nlohmann::json MergePrefabComponents(const nlohmann::json& defaults,
                                     const nlohmann::json& overrides);

struct PrefabSaveResult {
    bool ok = false;
    std::string error;
};

class PrefabRegistry {
public:
    bool Refresh(const std::filesystem::path& prefabRoot = "prefabs",
                 const std::filesystem::path& modelRoot = "models");
    const std::vector<PrefabAsset>& Assets() const { return assets_; }
    const PrefabAsset* Find(const std::string& id) const;
    uint64_t Revision() const { return revision_; }
    const std::string& LastError() const { return lastError_; }

    static bool IsSupportedModel(const std::filesystem::path& path);
    static PrefabSaveResult Save(const PrefabAsset& prefab,
                                 const std::filesystem::path& path);
    static PrefabSaveResult ImportModel(const std::filesystem::path& source,
                                        std::filesystem::path& savedPrefab);

private:
    std::vector<PrefabAsset> assets_;
    uint64_t revision_ = 0;
    std::string fingerprint_;
    std::string lastError_;
};

#endif
