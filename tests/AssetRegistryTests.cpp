#include "AssetRegistry.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

static void Write(const std::filesystem::path& path, const std::string& text) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream << text;
}

static std::string TexturedGltf(const std::string& texture) {
    return std::string(R"({"asset":{"version":"2.0"},"scene":0,
      "scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],
      "buffers":[{"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","byteLength":36}],
      "bufferViews":[{"buffer":0,"byteLength":36}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0]}],
      "images":[{"uri":")") + texture + R"("}],"textures":[{"source":0}],
      "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}]})";
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("sge-asset-registry-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(root);
    Write("models/props/crate.glb", "model bytes");
    Write("models/props/crate_copy.glb", "model bytes");
    std::filesystem::last_write_time("models/props/crate_copy.glb",
        std::filesystem::last_write_time("models/props/crate.glb"));
    Write("models/props/albedo map.png", "not a real png");
    Write("models/props/unreferenced.png", "not a real png");
    Write("models/props/textured.gltf", TexturedGltf("albedo%20map.png"));
    Write("models/props/missing_texture.gltf", TexturedGltf("missing.png"));

    AssetRegistry registry;
    CHECK(registry.Refresh());
    const AssetRecord* model = registry.FindPath("models/props/crate.glb");
    CHECK(model != nullptr);
    const std::string guid = model ? model->guid : "";
    CHECK(!guid.empty());
    const AssetRecord* copy = registry.FindPath("models/props/crate_copy.glb");
    CHECK(copy != nullptr);
    if (copy) CHECK(copy->guid != guid);
    CHECK(std::filesystem::exists("assetcache/registry.json"));
    const AssetRecord* albedo = registry.FindPath("models/props/albedo map.png");
    const AssetRecord* unreferenced = registry.FindPath(
        "models/props/unreferenced.png");
    const AssetRecord* textured = registry.FindPath("models/props/textured.gltf");
    CHECK(albedo != nullptr);
    CHECK(unreferenced != nullptr);
    CHECK(textured != nullptr);
    if (albedo && unreferenced && textured) {
        CHECK(std::find(textured->dependencies.begin(), textured->dependencies.end(),
                        albedo->guid) != textured->dependencies.end());
        CHECK(std::find(textured->dependencies.begin(), textured->dependencies.end(),
                        unreferenced->guid) == textured->dependencies.end());
    }
    const AssetRecord* missingTexture = registry.FindPath(
        "models/props/missing_texture.gltf");
    CHECK(missingTexture != nullptr);
    if (missingTexture) CHECK(std::find(missingTexture->missingDependencies.begin(),
        missingTexture->missingDependencies.end(),
        "models/props/missing.png") != missingTexture->missingDependencies.end());

    Write("prefabs/crate.json", std::string(R"({"schemaVersion":2,
      "id":"crate","components":{"staticMesh":{"path":"models/props/crate.glb",
      "assetGuid":")") + guid + R"("}}})");
    CHECK(registry.Refresh());
    const AssetRecord* prefab = registry.FindPath("prefabs/crate.json");
    CHECK(prefab != nullptr);
    if (prefab) CHECK(std::find(prefab->dependencies.begin(),
        prefab->dependencies.end(), guid) != prefab->dependencies.end());
    Write("levels/dependency_chain.json", R"({"schemaVersion":1,
      "name":"Dependency Chain","entities":[{"id":1,"type":"prefab",
      "name":"Crate","prefabId":"crate","transform":{
      "position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}}]})");
    CHECK(registry.Refresh());
    prefab = registry.FindPath("prefabs/crate.json");
    const AssetRecord* level = registry.FindPath("levels/dependency_chain.json");
    CHECK(prefab != nullptr);
    CHECK(level != nullptr);
    if (prefab && level) CHECK(std::find(level->dependencies.begin(),
        level->dependencies.end(), prefab->guid) != level->dependencies.end());

    Write("prefabs/missing.json", R"({"schemaVersion":2,
      "id":"missing","components":{"staticMesh":{
      "path":"models/props/not_here.glb"}}})");
    CHECK(registry.Refresh());
    const AssetRecord* missing = registry.FindPath("prefabs/missing.json");
    CHECK(missing != nullptr);
    if (missing) CHECK(std::find(missing->missingDependencies.begin(),
        missing->missingDependencies.end(), "models/props/not_here.glb") !=
        missing->missingDependencies.end());

    std::filesystem::remove("models/props/crate_copy.glb");
    CHECK(registry.Refresh());
    std::filesystem::rename("models/props/crate.glb", "models/props/crate_renamed.glb");
    CHECK(registry.Refresh());
    const AssetRecord* renamed = registry.FindPath("models/props/crate_renamed.glb");
    CHECK(renamed != nullptr);
    if (renamed) CHECK(renamed->guid == guid);

    std::filesystem::remove("models/props/crate_renamed.glb");
    CHECK(registry.Refresh());
    CHECK(registry.FindGuid(guid) == nullptr);

    std::filesystem::current_path(previous);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
