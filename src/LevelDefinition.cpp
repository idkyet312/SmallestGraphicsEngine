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
    case LevelEntityType::Fern: return "fern";
    }
    return "unknown";
}

bool ParseLevelEntityType(const std::string& text, LevelEntityType& type) {
    for (int i = static_cast<int>(LevelEntityType::PlayerSpawn);
         i <= static_cast<int>(LevelEntityType::Fern); ++i) {
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
    std::unordered_set<uint64_t> ids;
    size_t playerSpawns = 0;
    for (const LevelEntity& entity : level.entities) {
        if (static_cast<int>(entity.type) <
                static_cast<int>(LevelEntityType::PlayerSpawn) ||
            static_cast<int>(entity.type) >
                static_cast<int>(LevelEntityType::Fern))
            result.errors.push_back("entity " + std::to_string(entity.id) +
                                    " has an unknown type");
        if (!entity.id || !ids.insert(entity.id).second)
            result.errors.push_back("entity IDs must be unique and non-zero");
        if (entity.name.empty())
            result.errors.push_back("entity " + std::to_string(entity.id) +
                                    " has an empty name");
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
        const json& entities = root.at("entities");
        if (!entities.is_array()) throw std::runtime_error("entities must be an array");
        for (const json& source : entities) {
            LevelEntity entity;
            entity.id = source.at("id").get<uint64_t>();
            const std::string typeName = source.at("type").get<std::string>();
            if (!ParseLevelEntityType(typeName, entity.type))
                throw std::runtime_error("unknown entity type: " + typeName);
            entity.name = source.at("name").get<std::string>();
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
            entities.push_back({
                {"id", entity.id}, {"type", LevelEntityTypeName(entity.type)},
                {"name", entity.name}, {"enabled", entity.enabled},
                {"transform", {
                    {"position", Vec3(entity.transform.position)},
                    {"rotation", Vec3(entity.transform.rotation)},
                    {"scale", Vec3(entity.transform.scale)}
                }}
            });
        }
        const json root = {
            {"schemaVersion", level.schemaVersion}, {"name", level.name},
            {"terrain", {{"heightScale", level.terrainHeightScale}}},
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
