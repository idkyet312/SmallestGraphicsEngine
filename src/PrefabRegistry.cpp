#include "PrefabRegistry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <nlohmann/json.hpp>
#include "EngineLogger.h"
#ifdef _WIN32
#include <Windows.h>
#endif

using nlohmann::json;

namespace {

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string Generic(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

bool ReadScale(const json& value, float out[3]) {
    if (!value.is_array() || value.size() != 3) return false;
    for (size_t i = 0; i < 3; ++i) {
        if (!value[i].is_number()) return false;
        out[i] = value[i].get<float>();
        if (!(out[i] > 0.0f) || out[i] > 10000.0f) return false;
    }
    return true;
}

bool ReadVec3(const json& value, float out[3]) {
    if (!value.is_array() || value.size() != 3) return false;
    for (size_t i = 0; i < 3; ++i) {
        if (!value[i].is_number()) return false;
        out[i] = value[i].get<float>();
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

void MigratePrefabJson(json& root) {
    const uint32_t version = root.value("schemaVersion", 1u);
    if (version == 1) {
        // v1 components already map directly to v2. Migration only declares
        // the new version, preserving all known and unknown fields.
        root["schemaVersion"] = 2;
    } else if (version != 2) {
        throw std::runtime_error("unsupported schemaVersion");
    }
}

bool IsSafeRelative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path)
        if (part == "..") return false;
    return true;
}

std::filesystem::path ResolveAssetGuid(const std::string& guid) {
    if (guid.empty()) return {};
    try {
        std::ifstream stream("assetcache/registry.json");
        json cache; stream >> cache;
        for (const json& asset : cache.value("assets", json::array()))
            if (asset.value("guid", "") == guid)
                return asset.value("path", "");
    } catch (...) {}
    return {};
}

std::string GuidForAssetPath(const std::filesystem::path& path) {
    const std::string normalized = path.lexically_normal().generic_string();
    try {
        std::ifstream stream("assetcache/registry.json");
        json cache; stream >> cache;
        for (const json& asset : cache.value("assets", json::array()))
            if (std::filesystem::path(asset.value("path", "")).lexically_normal()
                    .generic_string() == normalized)
                return asset.value("guid", "");
    } catch (...) {}
    return {};
}

std::string NewAssetGuid(const std::filesystem::path& path) {
    std::error_code error;
    const std::string source = path.lexically_normal().generic_string() + ':' +
        std::to_string(std::filesystem::file_size(path, error)) + ':' +
        std::to_string(std::filesystem::last_write_time(path, error)
            .time_since_epoch().count());
    const auto hash = [](uint64_t seed, const std::string& value) {
        for (unsigned char byte : value) { seed ^= byte; seed *= 1099511628211ull; }
        return seed;
    };
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16)
           << hash(1469598103934665603ull, source) << std::setw(16)
           << hash(1099511628211ull, source + ":sge");
    return stream.str();
}

std::vector<std::string> ValidateImportedModel(
        const std::filesystem::path& path) {
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::vector<std::string>> cache;
    std::error_code error;
    const std::string key = Generic(path) + ':' +
        std::to_string(std::filesystem::file_size(path, error)) + ':' +
        std::to_string(std::filesystem::last_write_time(path, error)
            .time_since_epoch().count());
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        const auto found = cache.find(key);
        if (found != cache.end()) return found->second;
    }
    std::vector<std::string> warnings;
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path.string(), aiProcess_Triangulate);
    if (!scene || !scene->HasMeshes()) {
        warnings.push_back("Model validation unavailable: importer could not read file");
    } else {
        uint64_t triangles = 0;
        bool hasUv = false;
        for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* mesh = scene->mMeshes[i];
            triangles += mesh->mNumFaces;
            hasUv = hasUv || mesh->HasTextureCoords(0);
        }
        if (triangles > 250000)
            warnings.push_back("High triangle count: " + std::to_string(triangles));
        if (!hasUv) warnings.push_back("Model has no UV channel 0");
        for (unsigned materialIndex = 0; materialIndex < scene->mNumMaterials;
             ++materialIndex) {
            const aiMaterial* material = scene->mMaterials[materialIndex];
            for (aiTextureType type : { aiTextureType_DIFFUSE, aiTextureType_BASE_COLOR,
                                        aiTextureType_NORMALS }) {
                for (unsigned textureIndex = 0;
                     textureIndex < material->GetTextureCount(type); ++textureIndex) {
                    aiString referenced;
                    if (material->GetTexture(type, textureIndex, &referenced) != AI_SUCCESS)
                        continue;
                    const std::string raw = referenced.C_Str();
                    if (raw.empty() || raw[0] == '*') continue;
                    std::filesystem::path texture = raw;
                    if (!texture.is_absolute()) texture = path.parent_path() / texture;
                    if (!std::filesystem::exists(texture))
                        warnings.push_back("Missing texture: " + raw);
                }
            }
        }
    }
    std::sort(warnings.begin(), warnings.end());
    warnings.erase(std::unique(warnings.begin(), warnings.end()), warnings.end());
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[key] = warnings;
    }
    return warnings;
}

PrefabAsset LoadDefinition(const std::filesystem::path& path) {
    PrefabAsset prefab;
    prefab.definitionPath = path;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open file");
        json root;
        stream >> root;
        if (!root.is_object()) throw std::runtime_error("root must be an object");
        MigratePrefabJson(root);
        prefab.schemaVersion = root.value("schemaVersion", 2u);
        prefab.id = root.at("id").get<std::string>();
        prefab.name = root.value("name", prefab.id);
        prefab.basePrefabId = root.value("extends", "");
        if (root.contains("children")) {
            if (!root.at("children").is_array())
                throw std::runtime_error("children must be an array");
            for (const json& source : root.at("children")) {
                PrefabChildAsset child;
                child.prefabId = source.at("prefab").get<std::string>();
                if (source.contains("position") &&
                    !ReadVec3(source.at("position"), child.position))
                    throw std::runtime_error("child position must be vec3");
                if (source.contains("rotation") &&
                    !ReadVec3(source.at("rotation"), child.rotation))
                    throw std::runtime_error("child rotation must be vec3");
                if (source.contains("scale") &&
                    !ReadScale(source.at("scale"), child.scale))
                    throw std::runtime_error("child scale must be positive vec3");
                if (child.prefabId.empty())
                    throw std::runtime_error("child prefab is empty");
                prefab.children.push_back(std::move(child));
            }
        }
        const json& components = root.at("components");
        if (!components.is_object())
            throw std::runtime_error("components must be an object");
        prefab.components = components;
        if (components.contains("staticMesh")) {
            const json& mesh = components.at("staticMesh");
            prefab.modelGuid = mesh.value("assetGuid", "");
            prefab.modelPath = mesh.value("path", "");
            if (!prefab.modelGuid.empty()) {
                const std::filesystem::path resolved = ResolveAssetGuid(prefab.modelGuid);
                if (!resolved.empty()) prefab.modelPath = resolved;
            }
            if (prefab.modelGuid.empty())
                prefab.modelGuid = GuidForAssetPath(prefab.modelPath);
            prefab.castShadow = mesh.value("castShadow", true);
            prefab.targetSize = mesh.value("targetSize", 0.0f);
            if (prefab.targetSize < 0.0f || prefab.targetSize > 10000.0f)
                throw std::runtime_error("staticMesh.targetSize is out of range");
            if (mesh.contains("defaultScale") &&
                !ReadScale(mesh.at("defaultScale"), prefab.defaultScale))
                throw std::runtime_error("staticMesh.defaultScale must be three positive numbers");
            for (const json& item : mesh.value("materialOverrides", json::array())) {
                PrefabMaterialOverride overrideValue;
                overrideValue.mesh = item.at("mesh").get<std::string>();
                overrideValue.texture = item.at("texture").get<std::string>();
                if (overrideValue.mesh.empty() || !IsSafeRelative(overrideValue.texture) ||
                    !std::filesystem::exists(overrideValue.texture))
                    throw std::runtime_error("material override mesh/texture is invalid");
                prefab.materialOverrides.push_back(std::move(overrideValue));
            }
            float previousLodDistance = 0.0f;
            for (const json& item : mesh.value("lods", json::array())) {
                PrefabLodAsset lod;
                lod.path = item.value("path", "");
                lod.assetGuid = item.value("assetGuid", "");
                if (!lod.assetGuid.empty()) {
                    const auto resolved = ResolveAssetGuid(lod.assetGuid);
                    if (!resolved.empty()) lod.path = resolved;
                }
                lod.distance = item.at("distance").get<float>();
                if (!IsSafeRelative(lod.path) || !PrefabRegistry::IsSupportedModel(lod.path) ||
                    !std::filesystem::exists(lod.path) || lod.distance <= previousLodDistance)
                    throw std::runtime_error("LOD path/distance is invalid or unsorted");
                previousLodDistance = lod.distance;
                prefab.lods.push_back(std::move(lod));
            }
        } else if (prefab.basePrefabId.empty()) {
            throw std::runtime_error("components.staticMesh is required without extends");
        }
        if (components.contains("collision"))
            prefab.collision = components.at("collision").value("shape", "none");
        if (components.contains("light")) {
            const json& light = components.at("light");
            prefab.light.enabled = true;
            if (light.contains("color") &&
                !ReadVec3(light.at("color"), prefab.light.color))
                throw std::runtime_error("light.color must be vec3");
            prefab.light.intensity = light.value("intensity", 1.0f);
            prefab.light.radius = light.value("radius", 5.0f);
            if (prefab.light.intensity < 0.0f || prefab.light.radius <= 0.0f ||
                !std::isfinite(prefab.light.intensity) ||
                !std::isfinite(prefab.light.radius))
                throw std::runtime_error("light values are invalid");
        }
        if (components.contains("audio")) {
            const json& audio = components.at("audio");
            prefab.audio.enabled = true;
            prefab.audio.path = audio.value("path", "");
            prefab.audio.loop = audio.value("loop", false);
            prefab.audio.radius = audio.value("radius", 15.0f);
            if (!IsSafeRelative(prefab.audio.path) ||
                !std::filesystem::exists(prefab.audio.path) ||
                prefab.audio.radius <= 0.0f || !std::isfinite(prefab.audio.radius))
                throw std::runtime_error("audio path is missing/unsafe or radius invalid");
        }
        if (components.contains("destructible")) {
            prefab.destructible.enabled = true;
            prefab.destructible.health = components.at("destructible").value(
                "health", 100.0f);
            if (prefab.destructible.health <= 0.0f ||
                !std::isfinite(prefab.destructible.health))
                throw std::runtime_error("destructible.health must be positive");
        }
        if (components.contains("spawner")) {
            const json& spawner = components.at("spawner");
            prefab.spawner.enabled = true;
            prefab.spawner.enemyType = spawner.value("enemyType", "bandit");
            prefab.spawner.count = spawner.value("count", 1u);
            if (prefab.spawner.enemyType.empty() || prefab.spawner.count == 0 ||
                prefab.spawner.count > 1024)
                throw std::runtime_error("spawner values are invalid");
        }
        if (components.contains("script"))
            prefab.scriptPath = components.at("script").value("path", "");
        if (prefab.id.empty()) throw std::runtime_error("id is empty");
        if (prefab.basePrefabId.empty() && !IsSafeRelative(prefab.modelPath))
            throw std::runtime_error("model path must be project-relative without '..'");
        if (prefab.basePrefabId.empty() && !PrefabRegistry::IsSupportedModel(prefab.modelPath))
            throw std::runtime_error("model must be .fbx, .glb, or .gltf");
        if (prefab.basePrefabId.empty() && !std::filesystem::exists(prefab.modelPath))
            throw std::runtime_error("model does not exist: " + Generic(prefab.modelPath));
        if (prefab.collision != "none" && prefab.collision != "box" &&
            prefab.collision != "mesh")
            throw std::runtime_error("collision shape must be none, box, or mesh");
        // script.path remains reserved. Runtime/language integration is deferred.
    } catch (const std::exception& error) {
        prefab.error = path.string() + ": " + error.what();
        if (prefab.id.empty()) prefab.id = "invalid/" + path.stem().string();
        if (prefab.name.empty()) prefab.name = path.stem().string();
    }
    return prefab;
}

} // namespace

json MergePrefabComponents(const json& defaults, const json& overrides) {
    json merged = defaults.is_object() ? defaults : json::object();
    if (!overrides.is_object()) return merged;
    const auto apply = [&](const auto& self, json& target,
                           const json& source) -> void {
        for (auto it = source.begin(); it != source.end(); ++it) {
            if (it.value().is_object() && target.contains(it.key()) &&
                target[it.key()].is_object())
                self(self, target[it.key()], it.value());
            else
                target[it.key()] = it.value();
        }
    };
    apply(apply, merged, overrides);
    return merged;
}

const std::vector<PrefabPropertyDescriptor>& PrefabPropertyMetadata() {
    static const std::vector<PrefabPropertyDescriptor> properties = {
        {"staticMesh", "castShadow", PrefabPropertyType::Boolean},
        {"staticMesh", "targetSize", PrefabPropertyType::Number, 0.0f, 10000.0f},
        {"collision", "shape", PrefabPropertyType::String},
        {"light", "color", PrefabPropertyType::Color3, 0.0f, 1.0f},
        {"light", "intensity", PrefabPropertyType::Number, 0.0f, 100000.0f},
        {"light", "radius", PrefabPropertyType::Number, 0.01f, 10000.0f},
        {"audio", "path", PrefabPropertyType::Path},
        {"audio", "loop", PrefabPropertyType::Boolean},
        {"audio", "radius", PrefabPropertyType::Number, 0.01f, 10000.0f},
        {"destructible", "health", PrefabPropertyType::Number, 0.01f, 1000000.0f},
        {"spawner", "enemyType", PrefabPropertyType::String},
        {"spawner", "count", PrefabPropertyType::Integer, 1.0f, 1024.0f}
    };
    return properties;
}

bool PrefabRegistry::IsSupportedModel(const std::filesystem::path& path) {
    const std::string extension = Lower(path.extension().string());
    return extension == ".fbx" || extension == ".glb" || extension == ".gltf";
}

bool PrefabRegistry::Refresh(const std::filesystem::path& prefabRoot,
                             const std::filesystem::path& modelRoot) {
    std::vector<PrefabAsset> found;
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> configuredModels;
    std::vector<std::string> fingerprintParts;
    std::error_code error;
    lastError_.clear();
    std::filesystem::create_directories(prefabRoot, error);
    error.clear();

    for (std::filesystem::recursive_directory_iterator it(prefabRoot, error), end;
         !error && it != end; it.increment(error)) {
        if (!it->is_regular_file() || Lower(it->path().extension().string()) != ".json")
            continue;
        PrefabAsset prefab = LoadDefinition(it->path());
        if (prefab.error.empty()) {
            prefab.warnings = ValidateImportedModel(prefab.modelPath);
            for (const std::string& warning : prefab.warnings)
                SGE_LOG("LogAssetValidation", EngineLog::Level::Warning,
                    prefab.id + ": " + warning);
        }
        fingerprintParts.push_back(Generic(it->path()) + ':' +
            std::to_string(it->file_size(error)) + ':' +
            std::to_string(it->last_write_time(error).time_since_epoch().count()));
        if (!prefab.scriptPath.empty() && std::filesystem::exists(prefab.scriptPath, error))
            fingerprintParts.push_back(Generic(prefab.scriptPath) + ':' +
                std::to_string(std::filesystem::file_size(prefab.scriptPath, error)) + ':' +
                std::to_string(std::filesystem::last_write_time(
                    prefab.scriptPath, error).time_since_epoch().count()));
        if (!prefab.error.empty()) lastError_ = prefab.error;
        if (!ids.insert(prefab.id).second) {
            prefab.error = "duplicate prefab id: " + prefab.id;
            lastError_ = prefab.error;
        }
        if (prefab.error.empty()) configuredModels.insert(Generic(prefab.modelPath));
        found.push_back(std::move(prefab));
    }
    if (error) lastError_ = "prefab scan failed: " + error.message();

    error.clear();
    if (std::filesystem::exists(modelRoot, error)) {
        for (std::filesystem::recursive_directory_iterator it(modelRoot, error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_regular_file() || !IsSupportedModel(it->path())) continue;
            const std::filesystem::path relative = std::filesystem::relative(it->path(), error);
            if (error) { error.clear(); continue; }
            const std::string model = Generic(relative);
            fingerprintParts.push_back(model + ':' +
                std::to_string(it->file_size(error)) + ':' +
                std::to_string(it->last_write_time(error).time_since_epoch().count()));
            if (configuredModels.count(model)) continue;
            PrefabAsset generated;
            generated.id = "model/" + model;
            generated.name = it->path().stem().string();
            generated.modelPath = relative;
            generated.modelGuid = GuidForAssetPath(relative);
            generated.generated = true;
            generated.warnings = ValidateImportedModel(relative);
            for (const std::string& warning : generated.warnings)
                SGE_LOG("LogAssetValidation", EngineLog::Level::Warning,
                    generated.id + ": " + warning);
            if (ids.insert(generated.id).second) found.push_back(std::move(generated));
        }
    }
    if (error) lastError_ = "model scan failed: " + error.message();

    std::unordered_map<std::string, size_t> prefabIndex;
    for (size_t i = 0; i < found.size(); ++i)
        if (!found[i].id.empty() && !prefabIndex.count(found[i].id))
            prefabIndex[found[i].id] = i;
    std::vector<uint8_t> resolveState(found.size(), 0);
    const auto resolveVariant = [&](const auto& self, size_t index) -> bool {
        if (resolveState[index] == 2) return found[index].error.empty();
        if (resolveState[index] == 1) {
            found[index].error = "prefab inheritance cycle at: " + found[index].id;
            lastError_ = found[index].error;
            return false;
        }
        resolveState[index] = 1;
        PrefabAsset local = found[index];
        if (!local.basePrefabId.empty()) {
            const auto parentIt = prefabIndex.find(local.basePrefabId);
            if (parentIt == prefabIndex.end() || !self(self, parentIt->second)) {
                found[index].error = "missing/invalid base prefab: " +
                                     local.basePrefabId;
                lastError_ = found[index].error;
                resolveState[index] = 2;
                return false;
            }
            PrefabAsset merged = found[parentIt->second];
            merged.schemaVersion = 2;
            merged.id = local.id;
            merged.name = local.name;
            merged.basePrefabId = local.basePrefabId;
            merged.definitionPath = local.definitionPath;
            merged.generated = local.generated;
            merged.error = local.error;
            merged.components = MergePrefabComponents(merged.components,
                                                       local.components);
            merged.children.insert(merged.children.end(), local.children.begin(),
                                   local.children.end());
            if (local.components.contains("staticMesh")) {
                merged.modelPath = local.modelPath;
                merged.modelGuid = local.modelGuid;
                merged.castShadow = local.castShadow;
                merged.targetSize = local.targetSize;
                merged.materialOverrides = local.materialOverrides;
                merged.lods = local.lods;
                std::copy(std::begin(local.defaultScale), std::end(local.defaultScale),
                          std::begin(merged.defaultScale));
            }
            if (local.components.contains("collision"))
                merged.collision = local.collision;
            if (local.components.contains("light")) merged.light = local.light;
            if (local.components.contains("audio")) merged.audio = local.audio;
            if (local.components.contains("destructible"))
                merged.destructible = local.destructible;
            if (local.components.contains("spawner")) merged.spawner = local.spawner;
            if (local.components.contains("script"))
                merged.scriptPath = local.scriptPath;
            found[index] = std::move(merged);
        }
        for (const PrefabChildAsset& child : found[index].children) {
            const auto childIt = prefabIndex.find(child.prefabId);
            if (childIt == prefabIndex.end() || childIt->second == index) {
                found[index].error = "missing/cyclic child prefab: " + child.prefabId;
                lastError_ = found[index].error;
                resolveState[index] = 2;
                return false;
            }
        }
        resolveState[index] = 2;
        return found[index].error.empty();
    };
    for (size_t i = 0; i < found.size(); ++i) resolveVariant(resolveVariant, i);

    std::sort(found.begin(), found.end(), [](const PrefabAsset& a,
                                             const PrefabAsset& b) {
        if (a.error.empty() != b.error.empty()) return a.error.empty();
        return Lower(a.name) < Lower(b.name);
    });
    std::sort(fingerprintParts.begin(), fingerprintParts.end());
    std::ostringstream fingerprint;
    for (const std::string& part : fingerprintParts) fingerprint << part << ';';
    const std::string nextFingerprint = fingerprint.str();
    const bool changed = nextFingerprint != fingerprint_;
    assets_ = std::move(found);
    if (changed) {
        fingerprint_ = nextFingerprint;
        ++revision_;
    }
    return changed;
}

PrefabSaveResult PrefabRegistry::ImportModel(
        const std::filesystem::path& source,
        std::filesystem::path& savedPrefab) {
    PrefabSaveResult result;
    if (!std::filesystem::exists(source) || !IsSupportedModel(source)) {
        result.error = "select an existing FBX, GLB, or GLTF model";
        return result;
    }
    try {
        const std::string safeStem = [&]() {
            std::string value = source.stem().string();
            for (char& c : value) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (!(std::isalnum(u) || c == '-' || c == '_')) c = '_';
            }
            return value.empty() ? std::string("imported_model") : value;
        }();
        const std::filesystem::path modelDirectory =
            std::filesystem::path("models/Imported") / safeStem;
        std::filesystem::create_directories(modelDirectory);
        const std::filesystem::path destination = modelDirectory / source.filename();
        std::filesystem::copy_file(source, destination,
            std::filesystem::copy_options::overwrite_existing);
        const std::unordered_set<std::string> sidecarExtensions = {
            ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".bin", ".mtl"
        };
        std::error_code scanError;
        for (std::filesystem::directory_iterator it(source.parent_path(), scanError), end;
             !scanError && it != end; it.increment(scanError)) {
            if (!it->is_regular_file()) continue;
            if (!sidecarExtensions.count(Lower(it->path().extension().string()))) continue;
            std::filesystem::copy_file(it->path(), modelDirectory / it->path().filename(),
                std::filesystem::copy_options::overwrite_existing, scanError);
            scanError.clear();
        }
        PrefabAsset prefab;
        prefab.id = "imported/" + safeStem;
        prefab.name = source.stem().string();
        prefab.modelPath = destination;
        prefab.modelGuid = GuidForAssetPath(destination);
        if (prefab.modelGuid.empty()) prefab.modelGuid = NewAssetGuid(destination);
        prefab.targetSize = 2.0f;
        prefab.collision = "box";
        savedPrefab = std::filesystem::path("prefabs/Imported") /
                      (safeStem + ".json");
        return Save(prefab, savedPrefab);
    } catch (const std::exception& error) {
        result.error = source.string() + ": " + error.what();
        return result;
    }
}

const PrefabAsset* PrefabRegistry::Find(const std::string& id) const {
    const auto found = std::find_if(assets_.begin(), assets_.end(),
        [&](const PrefabAsset& prefab) { return prefab.id == id; });
    return found == assets_.end() || !found->error.empty() ? nullptr : &*found;
}

PrefabSaveResult PrefabRegistry::Save(const PrefabAsset& prefab,
                                      const std::filesystem::path& path) {
    PrefabSaveResult result;
    if (prefab.id.empty()) { result.error = "prefab id is empty"; return result; }
    if (prefab.basePrefabId.empty() &&
        (!IsSafeRelative(prefab.modelPath) || !IsSupportedModel(prefab.modelPath))) {
        result.error = "model path must be a project-relative FBX/GLB/GLTF";
        return result;
    }
    if (prefab.basePrefabId.empty() && !std::filesystem::exists(prefab.modelPath)) {
        result.error = "model does not exist: " + prefab.modelPath.string();
        return result;
    }
    if (prefab.collision != "none" && prefab.collision != "box" &&
        prefab.collision != "mesh") {
        result.error = "collision shape must be none, box, or mesh";
        return result;
    }
    if (prefab.targetSize < 0.0f || !std::isfinite(prefab.targetSize)) {
        result.error = "target size is invalid";
        return result;
    }
    for (float scale : prefab.defaultScale)
        if (!(scale > 0.0f) || !std::isfinite(scale)) {
            result.error = "default scale must be positive";
            return result;
        }
    if (!prefab.scriptPath.empty() &&
        (!IsSafeRelative(prefab.scriptPath) ||
         !std::filesystem::exists(prefab.scriptPath))) {
        result.error = "script path is missing or unsafe";
        return result;
    }
    if (prefab.audio.enabled && (!IsSafeRelative(prefab.audio.path) ||
        !std::filesystem::exists(prefab.audio.path) || prefab.audio.radius <= 0.0f)) {
        result.error = "audio path is missing/unsafe or radius invalid";
        return result;
    }
    try {
        json components = prefab.components.is_object()
            ? prefab.components : json::object();
        if (!prefab.modelPath.empty()) components["staticMesh"] = {
            {"path", Generic(prefab.modelPath)},
            {"assetGuid", prefab.modelGuid.empty()
                ? ([&]() { const std::string cached = GuidForAssetPath(prefab.modelPath);
                    return cached.empty() ? NewAssetGuid(prefab.modelPath) : cached; })()
                : prefab.modelGuid},
            {"defaultScale", json::array({ prefab.defaultScale[0],
                prefab.defaultScale[1], prefab.defaultScale[2] })},
            {"targetSize", prefab.targetSize}, {"castShadow", prefab.castShadow}
        };
        else components.erase("staticMesh");
        if (components.contains("staticMesh")) {
            json materialOverrides = json::array();
            for (const PrefabMaterialOverride& overrideValue : prefab.materialOverrides)
                materialOverrides.push_back({ {"mesh", overrideValue.mesh},
                    {"texture", Generic(overrideValue.texture)} });
            if (!materialOverrides.empty())
                components["staticMesh"]["materialOverrides"] = materialOverrides;
            else components["staticMesh"].erase("materialOverrides");
            json lods = json::array();
            for (const PrefabLodAsset& lod : prefab.lods)
                lods.push_back({ {"path", Generic(lod.path)},
                    {"assetGuid", lod.assetGuid}, {"distance", lod.distance} });
            if (!lods.empty()) components["staticMesh"]["lods"] = lods;
            else components["staticMesh"].erase("lods");
        }
        components["collision"] = {{"shape", prefab.collision}};
        if (!prefab.scriptPath.empty())
            components["script"] = {{"path", Generic(prefab.scriptPath)}};
        else components.erase("script");
        if (prefab.light.enabled)
            components["light"] = {
                {"color", json::array({ prefab.light.color[0],
                    prefab.light.color[1], prefab.light.color[2] })},
                {"intensity", prefab.light.intensity}, {"radius", prefab.light.radius}
            };
        else components.erase("light");
        if (prefab.audio.enabled)
            components["audio"] = { {"path", Generic(prefab.audio.path)},
                {"loop", prefab.audio.loop}, {"radius", prefab.audio.radius} };
        else components.erase("audio");
        if (prefab.destructible.enabled)
            components["destructible"] = {{"health", prefab.destructible.health}};
        else components.erase("destructible");
        if (prefab.spawner.enabled)
            components["spawner"] = { {"enemyType", prefab.spawner.enemyType},
                                       {"count", prefab.spawner.count} };
        else components.erase("spawner");
        const json rootV1 = {
            {"schemaVersion", 1}, {"id", prefab.id}, {"name", prefab.name},
            {"components", {
                {"staticMesh", {
                    {"path", Generic(prefab.modelPath)},
                    {"defaultScale", json::array({ prefab.defaultScale[0],
                        prefab.defaultScale[1], prefab.defaultScale[2] })},
                    {"targetSize", prefab.targetSize},
                    {"castShadow", prefab.castShadow}
                }},
                {"collision", {{"shape", prefab.collision}}},
                {"script", {{"path", Generic(prefab.scriptPath)}}}
            }}
        };
        (void)rootV1;
        json root = { {"schemaVersion", 2}, {"id", prefab.id},
                      {"name", prefab.name}, {"components", components} };
        if (!prefab.basePrefabId.empty()) root["extends"] = prefab.basePrefabId;
        if (!prefab.children.empty()) {
            root["children"] = json::array();
            for (const PrefabChildAsset& child : prefab.children)
                root["children"].push_back({ {"prefab", child.prefabId},
                    {"position", json::array({ child.position[0], child.position[1],
                                               child.position[2] })},
                    {"rotation", json::array({ child.rotation[0], child.rotation[1],
                                               child.rotation[2] })},
                    {"scale", json::array({ child.scale[0], child.scale[1],
                                            child.scale[2] })} });
        }
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        { std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
          if (!stream) throw std::runtime_error("cannot open temporary file");
          stream << root.dump(2) << '\n'; }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace destination file");
#else
        std::filesystem::rename(temporary, path);
#endif
        result.ok = true;
    } catch (const std::exception& error) {
        result.error = path.string() + ": " + error.what();
    }
    return result;
}
