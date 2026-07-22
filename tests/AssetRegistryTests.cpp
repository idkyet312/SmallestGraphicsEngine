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

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("sge-asset-registry-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(root);
    Write("models/props/crate.glb", "model bytes");
    Write("models/props/albedo.png", "not a real png");

    AssetRegistry registry;
    CHECK(registry.Refresh());
    const AssetRecord* model = registry.FindPath("models/props/crate.glb");
    CHECK(model != nullptr);
    const std::string guid = model ? model->guid : "";
    CHECK(!guid.empty());
    CHECK(std::filesystem::exists("assetcache/registry.json"));

    Write("prefabs/crate.json", std::string(R"({"schemaVersion":2,
      "id":"crate","components":{"staticMesh":{"path":"models/props/crate.glb",
      "assetGuid":")") + guid + R"("}}})");
    CHECK(registry.Refresh());
    const AssetRecord* prefab = registry.FindPath("prefabs/crate.json");
    CHECK(prefab != nullptr);
    if (prefab) CHECK(std::find(prefab->dependencies.begin(),
        prefab->dependencies.end(), guid) != prefab->dependencies.end());

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
