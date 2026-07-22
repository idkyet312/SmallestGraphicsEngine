#include "AssetRegistry.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <nlohmann/json.hpp>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

using nlohmann::json;

namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool Classify(const std::filesystem::path& path, AssetKind& kind) {
    const std::string extension = Lower(path.extension().string());
    if (extension == ".fbx" || extension == ".glb" || extension == ".gltf")
        kind = AssetKind::Model;
    else if (extension == ".png" || extension == ".jpg" ||
             extension == ".jpeg" || extension == ".dds" || extension == ".tga")
        kind = AssetKind::Texture;
    else if (extension == ".mp3" || extension == ".ogg" || extension == ".wav")
        kind = AssetKind::Audio;
    else return false;
    return true;
}

uint64_t Hash(uint64_t hash, const std::string& value) {
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Signature(const AssetRecord& asset) {
    return std::to_string(static_cast<int>(asset.kind)) + ':' +
        std::to_string(asset.size) + ':' + std::to_string(asset.modified);
}

std::string MakeGuid(const AssetRecord& asset) {
    const std::string source = asset.path.generic_string() + ':' +
        std::to_string(asset.size) + ':' + std::to_string(asset.modified);
    const uint64_t first = Hash(1469598103934665603ull, source);
    const uint64_t second = Hash(1099511628211ull, source + ":sge");
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << first
           << std::setw(16) << second;
    return stream.str();
}

std::string KindString(AssetKind kind) {
    return AssetRegistry::KindName(kind);
}

bool ParseKind(const std::string& text, AssetKind& kind) {
    for (AssetKind candidate : { AssetKind::Model, AssetKind::Texture,
                                 AssetKind::Audio, AssetKind::Prefab,
                                 AssetKind::Level }) {
        if (text == AssetRegistry::KindName(candidate)) {
            kind = candidate;
            return true;
        }
    }
    return false;
}

} // namespace

const char* AssetRegistry::KindName(AssetKind kind) {
    switch (kind) {
    case AssetKind::Model: return "Models";
    case AssetKind::Texture: return "Textures";
    case AssetKind::Audio: return "Audio";
    case AssetKind::Prefab: return "Prefabs";
    case AssetKind::Level: return "Levels";
    }
    return "Unknown";
}

std::vector<const AssetRecord*> AssetRegistry::Assets(AssetKind kind) const {
    std::vector<const AssetRecord*> result;
    for (const AssetRecord& asset : assets_)
        if (asset.kind == kind) result.push_back(&asset);
    return result;
}

const AssetRecord* AssetRegistry::FindGuid(const std::string& guid) const {
    const auto found = std::find_if(assets_.begin(), assets_.end(),
        [&](const AssetRecord& asset) { return asset.guid == guid; });
    return found == assets_.end() ? nullptr : &*found;
}

const AssetRecord* AssetRegistry::FindPath(
        const std::filesystem::path& path) const {
    const std::string normalized = path.lexically_normal().generic_string();
    const auto found = std::find_if(assets_.begin(), assets_.end(),
        [&](const AssetRecord& asset) {
            return asset.path.lexically_normal().generic_string() == normalized;
        });
    return found == assets_.end() ? nullptr : &*found;
}

bool AssetRegistry::Refresh(bool force) {
    std::vector<AssetRecord> scanned;
    std::error_code error;
    const auto scan = [&](const std::filesystem::path& root,
                          AssetKind fixedKind, bool classify) {
        if (!std::filesystem::exists(root, error)) return;
        for (std::filesystem::recursive_directory_iterator it(root, error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) continue;
            AssetKind kind = fixedKind;
            if (classify && !Classify(it->path(), kind)) continue;
            if (!classify && Lower(it->path().extension().string()) != ".json")
                continue;
            AssetRecord record;
            record.path = it->path().lexically_normal();
            record.kind = kind;
            record.size = it->file_size(error);
            record.modified = static_cast<int64_t>(
                it->last_write_time(error).time_since_epoch().count());
            scanned.push_back(std::move(record));
        }
    };
    scan("models", AssetKind::Model, true);
    scan("textures", AssetKind::Texture, true);
    scan("audio", AssetKind::Audio, true);
    scan("prefabs", AssetKind::Prefab, false);
    scan("levels", AssetKind::Level, false);
    if (error) lastError_ = error.message(); else lastError_.clear();
    std::sort(scanned.begin(), scanned.end(), [](const AssetRecord& a,
                                                  const AssetRecord& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.path.generic_string() < b.path.generic_string();
    });
    uint64_t fingerprint = 1469598103934665603ull;
    for (const AssetRecord& asset : scanned) {
        fingerprint = Hash(fingerprint, asset.path.generic_string());
        fingerprint = Hash(fingerprint, std::to_string(asset.size));
        fingerprint = Hash(fingerprint, std::to_string(asset.modified));
    }
    if (!force && fingerprint == fingerprint_ && !assets_.empty()) return false;

    const std::filesystem::path cachePath = "assetcache/registry.json";
    std::vector<AssetRecord> cached;
    uint64_t cachedFingerprint = 0;
    try {
        std::ifstream stream(cachePath);
        json root;
        if (stream) stream >> root;
        if (root.is_object()) {
            cachedFingerprint = root.value("fingerprint", 0ull);
            for (const json& item : root.at("assets")) {
                AssetRecord record;
                if (!ParseKind(item.at("kind").get<std::string>(), record.kind))
                    throw std::runtime_error("invalid cached asset kind");
                record.guid = item.value("guid", "");
                record.path = item.at("path").get<std::string>();
                record.size = item.value("size", 0ull);
                record.modified = item.value("mtime", 0ll);
                record.width = item.value("width", 0);
                record.height = item.value("height", 0);
                record.dependencies = item.value("dependencies",
                                                  std::vector<std::string>{});
                cached.push_back(std::move(record));
            }
        }
    } catch (...) {
        cached.clear();
    }
    const bool cacheHasGuids = !cached.empty() && std::all_of(cached.begin(),
        cached.end(), [](const AssetRecord& asset) { return !asset.guid.empty(); });
    const bool cacheLoaded = !force && cachedFingerprint == fingerprint &&
                             cacheHasGuids;
    if (cacheLoaded) scanned = cached;
    if (!cacheLoaded) {
        std::unordered_map<std::string, AssetRecord> cachedByPath;
        std::unordered_map<std::string, std::string> cachedBySignature;
        std::unordered_map<std::string, std::string> declaredGuidByPath;
        for (const AssetRecord& old : cached) {
            cachedByPath[old.path.lexically_normal().generic_string()] = old;
            if (!old.guid.empty()) cachedBySignature[Signature(old)] = old.guid;
        }
        for (const AssetRecord& definition : scanned) {
            if (definition.kind != AssetKind::Prefab) continue;
            try {
                std::ifstream stream(definition.path);
                json root; stream >> root;
                const json& mesh = root.at("components").at("staticMesh");
                const std::string guid = mesh.value("assetGuid", "");
                const std::string path = std::filesystem::path(
                    mesh.value("path", "")).lexically_normal().generic_string();
                if (!guid.empty() && !path.empty()) declaredGuidByPath[path] = guid;
            } catch (...) {}
        }
        for (AssetRecord& asset : scanned) {
            const std::string normalizedPath =
                asset.path.lexically_normal().generic_string();
            const auto declared = declaredGuidByPath.find(normalizedPath);
            if (declared != declaredGuidByPath.end()) asset.guid = declared->second;
            const auto old = cachedByPath.find(
                normalizedPath);
            if (asset.guid.empty() && old != cachedByPath.end()) {
                asset.guid = old->second.guid;
                asset.width = old->second.width;
                asset.height = old->second.height;
            } else if (asset.guid.empty()) {
                const auto renamed = cachedBySignature.find(Signature(asset));
                asset.guid = renamed == cachedBySignature.end()
                    ? MakeGuid(asset) : renamed->second;
            }
            if (asset.kind == AssetKind::Texture && asset.width == 0) {
                int channels = 0;
                stbi_info(asset.path.string().c_str(), &asset.width,
                          &asset.height, &channels);
            }
        }

        std::unordered_map<std::string, std::string> guidByPath;
        std::unordered_map<std::string, std::string> guidByPrefabId;
        for (const AssetRecord& asset : scanned)
            guidByPath[asset.path.lexically_normal().generic_string()] = asset.guid;
        for (const AssetRecord& asset : scanned) {
            if (asset.kind != AssetKind::Prefab) continue;
            try {
                std::ifstream stream(asset.path);
                json root; stream >> root;
                const std::string id = root.value("id", "");
                if (!id.empty()) guidByPrefabId[id] = asset.guid;
            } catch (...) {}
        }
        const auto addDependency = [](AssetRecord& asset,
                                      const std::string& dependency) {
            if (!dependency.empty() && dependency != asset.guid &&
                std::find(asset.dependencies.begin(), asset.dependencies.end(),
                          dependency) == asset.dependencies.end())
                asset.dependencies.push_back(dependency);
        };
        for (AssetRecord& asset : scanned) {
            if (asset.kind == AssetKind::Model) {
                for (const AssetRecord& texture : scanned)
                    if (texture.kind == AssetKind::Texture &&
                        texture.path.parent_path() == asset.path.parent_path())
                        addDependency(asset, texture.guid);
            }
            if (asset.kind != AssetKind::Prefab && asset.kind != AssetKind::Level)
                continue;
            try {
                std::ifstream stream(asset.path);
                json root; stream >> root;
                const auto walk = [&](const auto& self, const json& value) -> void {
                    if (value.is_object()) {
                        for (auto it = value.begin(); it != value.end(); ++it)
                            self(self, it.value());
                    } else if (value.is_array()) {
                        for (const json& child : value) self(self, child);
                    } else if (value.is_string()) {
                        const std::string text = value.get<std::string>();
                        auto byPath = guidByPath.find(
                            std::filesystem::path(text).lexically_normal().generic_string());
                        if (byPath != guidByPath.end()) addDependency(asset, byPath->second);
                        auto byId = guidByPrefabId.find(text);
                        if (byId != guidByPrefabId.end()) addDependency(asset, byId->second);
                    }
                };
                walk(walk, root);
            } catch (...) {}
            std::sort(asset.dependencies.begin(), asset.dependencies.end());
        }
        try {
            std::filesystem::create_directories(cachePath.parent_path());
            json root;
            root["schemaVersion"] = 2;
            root["fingerprint"] = fingerprint;
            root["assets"] = json::array();
            for (const AssetRecord& asset : scanned)
                root["assets"].push_back({ {"guid", asset.guid},
                    {"path", asset.path.generic_string()},
                    {"kind", KindString(asset.kind)}, {"size", asset.size},
                    {"mtime", asset.modified}, {"width", asset.width},
                    {"height", asset.height},
                    {"dependencies", asset.dependencies} });
            std::ofstream stream(cachePath);
            stream << root.dump(2) << '\n';
        } catch (const std::exception& exception) {
            lastError_ = exception.what();
        }
    }
    const bool changed = fingerprint != fingerprint_ || assets_.empty();
    assets_ = std::move(scanned);
    fingerprint_ = fingerprint;
    if (changed) ++revision_;
    return changed;
}
