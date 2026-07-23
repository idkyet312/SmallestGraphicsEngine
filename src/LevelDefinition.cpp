#include "LevelDefinition.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <Windows.h>
#endif

using nlohmann::json;

namespace {

LevelEntity MakeEntity(uint64_t id, LevelEntityType type, const char* name,
                       float x, float y, float z, float yaw = 0.0f) {
    LevelEntity entity;
    entity.id = id;
    entity.type = type;
    entity.name = name;
    entity.transform.position[0] = x;
    entity.transform.position[1] = y;
    entity.transform.position[2] = z;
    entity.transform.rotation[1] = yaw;
    return entity;
}

bool Finite3(const float value[3]) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

json Vec3(const float value[3]) {
    return json::array({ value[0], value[1], value[2] });
}

bool ReadVec3(const json& value, float out[3]) {
    if (!value.is_array() || value.size() != 3) return false;
    for (size_t i = 0; i < 3; ++i) {
        if (!value[i].is_number()) return false;
        out[i] = value[i].get<float>();
    }
    return Finite3(out);
}

std::string JoinErrors(const std::vector<std::string>& errors) {
    std::ostringstream stream;
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) stream << "; ";
        stream << errors[i];
    }
    return stream.str();
}

} // namespace

const char* LevelEntityTypeName(LevelEntityType type) {
    switch (type) {
    case LevelEntityType::PlayerSpawn: return "player_spawn";
    case LevelEntityType::WoodHouse: return "wood_house";
    case LevelEntityType::MetalHouse: return "metal_house";
    case LevelEntityType::Palm: return "palm";
    case LevelEntityType::ExplosiveBarrel: return "explosive_barrel";
    case LevelEntityType::EnemySpawn: return "enemy_spawn";
    case LevelEntityType::Humvee: return "humvee";
    case LevelEntityType::Helicopter: return "helicopter";
    case LevelEntityType::GrassPatch: return "grass_patch";
    case LevelEntityType::Dandelion: return "dandelion";
    case LevelEntityType::Rock: return "rock";
    case LevelEntityType::Prefab: return "prefab";
    }
    return "unknown";
}

bool ParseLevelEntityType(const std::string& text, LevelEntityType& type) {
    if (text == "fern") {
        type = LevelEntityType::Dandelion;
        return true;
    }
    for (int i = static_cast<int>(LevelEntityType::PlayerSpawn);
         i <= static_cast<int>(LevelEntityType::Prefab); ++i) {
        const auto candidate = static_cast<LevelEntityType>(i);
        if (text == LevelEntityTypeName(candidate)) {
            type = candidate;
            return true;
        }
    }
    return false;
}

LevelDefinition MakeLevelOneTemplate() {
    LevelDefinition level;
    level.name = "Level 1 Copy";
    uint64_t id = 1;
    level.entities.push_back(MakeEntity(id++, LevelEntityType::PlayerSpawn,
        "Player Spawn", 0.0f, 5.0f, 10.0f, 180.0f));
    level.entities.push_back(MakeEntity(id++, LevelEntityType::WoodHouse,
        "North Wood House", 0.0f, 0.0f, 9.0f, 0.0f));
    level.entities.push_back(MakeEntity(id++, LevelEntityType::MetalHouse,
        "East Metal House", 9.0f, 0.0f, 0.0f, 90.0f));
    level.entities.push_back(MakeEntity(id++, LevelEntityType::WoodHouse,
        "South Wood House", 0.0f, 0.0f, -9.0f, 180.0f));
    level.entities.push_back(MakeEntity(id++, LevelEntityType::MetalHouse,
        "West Metal House", -9.0f, 0.0f, 0.0f, -90.0f));

    const float palms[][4] = {
        {-15.5f,-13.8f,15.0f, 0.5f}, {-3.5f,-18.0f,12.8f,-0.4f},
        { 14.8f,-14.2f,16.4f, 0.7f}, {18.0f,  2.8f,13.6f,-0.6f},
        { 14.2f, 14.8f,14.2f, 0.3f}, { 3.2f, 18.2f,12.0f,-0.5f},
        {-14.5f, 14.0f,15.6f, 0.6f}, {-18.0f, 2.5f,13.2f,-0.3f}
    };
    for (size_t i = 0; i < 8; ++i) {
        auto palm = MakeEntity(id++, LevelEntityType::Palm,
            ("Palm " + std::to_string(i + 1)).c_str(), palms[i][0], 0.0f,
            palms[i][1]);
        palm.transform.scale[1] = palms[i][2];
        palm.transform.rotation[2] = palms[i][3];
        level.entities.push_back(std::move(palm));
    }

    const float barrels[][3] = {
        { 4.6f,3.25f, 4.6f}, {-4.6f,3.25f, 4.6f},
        { 4.6f,3.25f,-4.6f}, {-4.6f,3.25f,-4.6f}
    };
    for (size_t i = 0; i < 4; ++i)
        level.entities.push_back(MakeEntity(id++, LevelEntityType::ExplosiveBarrel,
            ("Barrel " + std::to_string(i + 1)).c_str(), barrels[i][0],
            barrels[i][1], barrels[i][2]));

    const float spawns[][3] = {
        {-0.85f,0.0f,17.5f}, {0.85f,0.0f,17.5f},
        {17.5f,0.0f,0.85f}, {17.5f,0.0f,-0.85f},
        {0.85f,0.0f,-17.5f}, {-0.85f,0.0f,-17.5f},
        {-17.5f,0.0f,-0.85f}, {-17.5f,0.0f,0.85f}
    };
    for (size_t i = 0; i < 8; ++i)
        level.entities.push_back(MakeEntity(id++, LevelEntityType::EnemySpawn,
            ("Enemy Spawn " + std::to_string(i + 1)).c_str(), spawns[i][0],
            spawns[i][1], spawns[i][2]));
    level.entities.push_back(MakeEntity(id++, LevelEntityType::Humvee,
        "Humvee", 0.0f, 3.45f, 0.0f));
    level.entities.push_back(MakeEntity(id++, LevelEntityType::Helicopter,
        "Helicopter", 0.0f, 14.0f, 0.0f));

    return level;
}

LevelValidationResult ValidateLevel(const LevelDefinition& level) {
    LevelValidationResult result;
    if (level.schemaVersion != 1)
        result.errors.push_back("unsupported schemaVersion " +
                                std::to_string(level.schemaVersion));
    if (level.name.empty()) result.errors.push_back("level name is empty");
    if (!std::isfinite(level.terrainHeightScale) ||
        level.terrainHeightScale < 0.0f || level.terrainHeightScale > 50.0f)
        result.errors.push_back("terrain heightScale must be between 0 and 50");
    if (level.terrainSculpt.size() > 256)
        result.errors.push_back("terrain sculpt supports at most 256 stamps");
    const LevelDXRDDGISettings& gi = level.dxrDDGI;
    if (!std::isfinite(gi.surfaceSpacing) || gi.surfaceSpacing < 0.25f ||
        gi.surfaceSpacing > 50.0f || !std::isfinite(gi.surfaceOffset) ||
        gi.surfaceOffset < 0.0f || gi.surfaceOffset > 5.0f ||
        gi.maxProbes == 0 || gi.maxProbes > 2048 ||
        gi.raysPerProbe < 8 || gi.raysPerProbe > 64 ||
        gi.probesPerFrame == 0 || gi.probesPerFrame > gi.maxProbes ||
        !std::isfinite(gi.maxRayDistance) || gi.maxRayDistance < 1.0f ||
        gi.maxRayDistance > 200.0f ||
        !std::isfinite(gi.intensity) || gi.intensity < 0.0f ||
        gi.intensity > 5.0f || !std::isfinite(gi.normalBias) ||
        gi.normalBias < 0.0f || gi.normalBias > 2.0f ||
        !std::isfinite(gi.viewBias) || gi.viewBias < 0.0f ||
        gi.viewBias > 2.0f || !std::isfinite(gi.hysteresis) ||
        gi.hysteresis < 0.0f || gi.hysteresis > 0.999f ||
        !std::isfinite(gi.multiBounceStrength) ||
        gi.multiBounceStrength < 0.0f || gi.multiBounceStrength > 1.0f)
        result.errors.push_back("lighting.dxrDDGI contains invalid settings");
    for (const TerrainSculptStamp& stamp : level.terrainSculpt) {
        if (!std::isfinite(stamp.x) || !std::isfinite(stamp.z) ||
            !std::isfinite(stamp.radius) || !std::isfinite(stamp.value) ||
            !std::isfinite(stamp.strength) || stamp.radius < 0.1f ||
            stamp.radius > 64.0f || stamp.strength < 0.0f ||
            stamp.strength > 10.0f ||
            (stamp.operation != TerrainSculptOperation::Add &&
             stamp.operation != TerrainSculptOperation::Flatten))
            result.errors.push_back("terrain contains an invalid sculpt stamp");
    }
    std::unordered_set<uint64_t> ids;
    size_t playerSpawns = 0;
    for (const LevelEntity& entity : level.entities) {
        if (static_cast<int>(entity.type) <
            static_cast<int>(LevelEntityType::PlayerSpawn) ||
            static_cast<int>(entity.type) >
                static_cast<int>(LevelEntityType::Prefab))
            result.errors.push_back("entity " + std::to_string(entity.id) +
                                    " has an unknown type");
        if (!entity.id || !ids.insert(entity.id).second)
            result.errors.push_back("entity IDs must be unique and non-zero");
        if (entity.name.empty())
            result.errors.push_back("entity " + std::to_string(entity.id) +
                                    " has an empty name");
        if (entity.type == LevelEntityType::Prefab && entity.prefabId.empty())
            result.errors.push_back("prefab entity " + std::to_string(entity.id) +
                                    " has no prefab ID");
        if (!entity.overrides.is_object())
            result.errors.push_back("entity " + std::to_string(entity.id) +
                                    " overrides must be an object");
        if (!Finite3(entity.transform.position) ||
            !Finite3(entity.transform.rotation) || !Finite3(entity.transform.scale))
            result.errors.push_back("entity " + std::to_string(entity.id) +
                                    " has a non-finite transform");
        if (entity.transform.scale[0] <= 0.0f ||
            entity.transform.scale[1] <= 0.0f || entity.transform.scale[2] <= 0.0f)
            result.errors.push_back("entity " + std::to_string(entity.id) +
                                    " has a non-positive scale");
        if (entity.enabled && entity.type == LevelEntityType::PlayerSpawn)
            ++playerSpawns;
    }
    if (playerSpawns != 1)
        result.errors.push_back("level must contain exactly one enabled player spawn");
    result.ok = result.errors.empty();
    return result;
}

LevelLoadResult LoadLevel(const std::filesystem::path& path) {
    LevelLoadResult result;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open file");
        json root;
        stream >> root;
        if (!root.is_object()) throw std::runtime_error("root must be an object");
        LevelDefinition level;
        level.schemaVersion = root.at("schemaVersion").get<uint32_t>();
        level.name = root.at("name").get<std::string>();
        level.terrainHeightScale = root.at("terrain").at("heightScale").get<float>();
        if (root.contains("lighting") && root.at("lighting").is_object() &&
            root.at("lighting").contains("dxrDDGI")) {
            const json& source = root.at("lighting").at("dxrDDGI");
            if (!source.is_object())
                throw std::runtime_error("lighting.dxrDDGI must be an object");
            level.dxrDDGI.enabled = source.value("enabled", false);
            level.dxrDDGI.surfaceSpacing =
                source.value("surfaceSpacing", 3.0f);
            level.dxrDDGI.surfaceOffset =
                source.value("surfaceOffset", 0.35f);
            level.dxrDDGI.maxProbes = source.value("maxProbes", 2048u);
            level.dxrDDGI.raysPerProbe =
                source.value("raysPerProbe", 64u);
            level.dxrDDGI.probesPerFrame =
                source.value("probesPerFrame", 16u);
            level.dxrDDGI.maxRayDistance =
                source.value("maxRayDistance", 24.0f);
            level.dxrDDGI.intensity = source.value("intensity", 0.45f);
            level.dxrDDGI.normalBias = source.value("normalBias", 0.18f);
            level.dxrDDGI.viewBias = source.value("viewBias", 0.05f);
            level.dxrDDGI.hysteresis = source.value("hysteresis", 0.95f);
            level.dxrDDGI.multiBounceStrength =
                source.value("multiBounceStrength", 0.35f);
            level.dxrDDGI.showProbes = source.value("showProbes", false);
        }
        const json& terrain = root.at("terrain");
        if (terrain.contains("sculpt")) {
            if (!terrain.at("sculpt").is_array())
                throw std::runtime_error("terrain sculpt must be an array");
            for (const json& source : terrain.at("sculpt")) {
                TerrainSculptStamp stamp;
                stamp.x = source.at("x").get<float>();
                stamp.z = source.at("z").get<float>();
                stamp.radius = source.at("radius").get<float>();
                const std::string operation = source.value("operation", "add");
                if (operation == "add") stamp.operation = TerrainSculptOperation::Add;
                else if (operation == "flatten")
                    stamp.operation = TerrainSculptOperation::Flatten;
                else throw std::runtime_error("unknown terrain sculpt operation: " + operation);
                stamp.value = source.at("value").get<float>();
                stamp.strength = source.value("strength", 1.0f);
                level.terrainSculpt.push_back(stamp);
            }
        }
        const json& entities = root.at("entities");
        if (!entities.is_array()) throw std::runtime_error("entities must be an array");
        for (const json& source : entities) {
            LevelEntity entity;
            entity.id = source.at("id").get<uint64_t>();
            const std::string typeName = source.at("type").get<std::string>();
            if (!ParseLevelEntityType(typeName, entity.type))
                throw std::runtime_error("unknown entity type: " + typeName);
            entity.name = source.at("name").get<std::string>();
            entity.prefabId = source.value("prefab", "");
            entity.overrides = source.value("overrides", json::object());
            entity.enabled = source.value("enabled", true);
            const json& transform = source.at("transform");
            if (!ReadVec3(transform.at("position"), entity.transform.position) ||
                !ReadVec3(transform.at("rotation"), entity.transform.rotation) ||
                !ReadVec3(transform.at("scale"), entity.transform.scale))
                throw std::runtime_error("entity transform must contain finite vec3 values");
            level.entities.push_back(std::move(entity));
        }
        LevelValidationResult validation = ValidateLevel(level);
        if (!validation.ok) throw std::runtime_error(JoinErrors(validation.errors));
        result.level = std::move(level);
        result.ok = true;
    } catch (const std::exception& error) {
        result.error = path.string() + ": " + error.what();
    }
    return result;
}

LevelSaveResult SaveLevel(const LevelDefinition& level,
                          const std::filesystem::path& path) {
    LevelSaveResult result;
    const LevelValidationResult validation = ValidateLevel(level);
    if (!validation.ok) {
        result.error = JoinErrors(validation.errors);
        return result;
    }
    try {
        json entities = json::array();
        for (const LevelEntity& entity : level.entities) {
            json saved = {
                {"id", entity.id}, {"type", LevelEntityTypeName(entity.type)},
                {"name", entity.name}, {"enabled", entity.enabled},
                {"transform", {
                    {"position", Vec3(entity.transform.position)},
                    {"rotation", Vec3(entity.transform.rotation)},
                    {"scale", Vec3(entity.transform.scale)}
                }}
            };
            if (!entity.prefabId.empty()) saved["prefab"] = entity.prefabId;
            if (!entity.overrides.empty()) saved["overrides"] = entity.overrides;
            entities.push_back(std::move(saved));
        }
        json sculpt = json::array();
        for (const TerrainSculptStamp& stamp : level.terrainSculpt) {
            sculpt.push_back({
                {"x", stamp.x}, {"z", stamp.z}, {"radius", stamp.radius},
                {"operation", stamp.operation == TerrainSculptOperation::Flatten
                    ? "flatten" : "add"},
                {"value", stamp.value}, {"strength", stamp.strength}
            });
        }
        const json root = {
            {"schemaVersion", level.schemaVersion}, {"name", level.name},
            {"terrain", {{"heightScale", level.terrainHeightScale},
                         {"sculpt", std::move(sculpt)}}},
            {"lighting", {{"dxrDDGI", {
                {"enabled", level.dxrDDGI.enabled},
                {"surfaceSpacing", level.dxrDDGI.surfaceSpacing},
                {"surfaceOffset", level.dxrDDGI.surfaceOffset},
                {"maxProbes", level.dxrDDGI.maxProbes},
                {"raysPerProbe", level.dxrDDGI.raysPerProbe},
                {"probesPerFrame", level.dxrDDGI.probesPerFrame},
                {"maxRayDistance", level.dxrDDGI.maxRayDistance},
                {"intensity", level.dxrDDGI.intensity},
                {"normalBias", level.dxrDDGI.normalBias},
                {"viewBias", level.dxrDDGI.viewBias},
                {"hysteresis", level.dxrDDGI.hysteresis},
                {"multiBounceStrength",
                    level.dxrDDGI.multiBounceStrength},
                {"showProbes", level.dxrDDGI.showProbes}
            }}}},
            {"entities", std::move(entities)}
        };
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("cannot open temporary file");
            stream << root.dump(2) << '\n';
            if (!stream) throw std::runtime_error("failed while writing temporary file");
        }
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
