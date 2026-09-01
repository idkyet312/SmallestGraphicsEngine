#include "LevelDefinition.h"
#include "TerrainStampLibrary.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <nlohmann/json.hpp>
// Decoding the splat sidecar. stb_image_write's implementation lives in
// GLBImporter.cpp; only the header is pulled in here so there is never a second
// definition of it in the link.
#include "../assets/importers/GLBImporter.h"
#include <stb_image_write.h>
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

// Uniform Catmull-Rom basis on one span. p1..p2 is the span; p0 and p3 are the
// neighbouring points that set the tangents.
float CatmullRom1D(float p0, float p1, float p2, float p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// Control point by index, with the ends handled per open/closed. An open spline
// duplicates its end points so the curve starts and stops exactly on them; a
// closed one wraps.
const float* SplineControlPoint(const LevelSplinePath& spline, int index) {
    const int count = (int)spline.points.size();
    if (count == 0) return nullptr;
    if (spline.closed) {
        index = ((index % count) + count) % count;
    } else {
        index = index < 0 ? 0 : (index >= count ? count - 1 : index);
    }
    return spline.points[(size_t)index].position;
}

} // namespace

std::vector<std::array<float, 3>> SampleSplineCurve(const LevelSplinePath& spline,
                                                    int samplesPerSpan) {
    std::vector<std::array<float, 3>> curve;
    const int count = (int)spline.points.size();
    if (count < 2) return curve;
    if (samplesPerSpan < 1) samplesPerSpan = 1;

    // A closed spline has one extra span, wrapping the last point to the first.
    const int spans = spline.closed ? count : count - 1;
    curve.reserve((size_t)spans * samplesPerSpan + 1);
    for (int span = 0; span < spans; ++span) {
        const float* p0 = SplineControlPoint(spline, span - 1);
        const float* p1 = SplineControlPoint(spline, span);
        const float* p2 = SplineControlPoint(spline, span + 1);
        const float* p3 = SplineControlPoint(spline, span + 2);
        for (int step = 0; step < samplesPerSpan; ++step) {
            const float t = (float)step / (float)samplesPerSpan;
            curve.push_back({ CatmullRom1D(p0[0], p1[0], p2[0], p3[0], t),
                              CatmullRom1D(p0[1], p1[1], p2[1], p3[1], t),
                              CatmullRom1D(p0[2], p1[2], p2[2], p3[2], t) });
        }
    }
    // Close the polyline on the final control point (or back on the first).
    const float* last = SplineControlPoint(spline, spline.closed ? 0 : count - 1);
    curve.push_back({ last[0], last[1], last[2] });
    return curve;
}

std::vector<SplineSegmentPlacement> EvaluateSplineSegments(
        const LevelSplinePath& spline,
        const std::function<float(float, float)>& terrainHeight) {
    std::vector<SplineSegmentPlacement> placements;
    if (spline.points.size() < 2) return placements;
    if (!(spline.spacing > 0.0f) || !std::isfinite(spline.spacing))
        return placements;

    // Dense polyline first, then step it by arc length. Sampling the curve at
    // uniform t instead would bunch segments on tight turns and stretch them on
    // straights.
    const std::vector<std::array<float, 3>> curve = SampleSplineCurve(spline, 24);
    if (curve.size() < 2) return placements;

    // Guard against a pathological control-point layout producing a huge run.
    constexpr size_t kMaxSegments = 4096;

    // Measure once, then sample shared boundaries. Point-tangent placement can
    // rotate neighbouring rigid pieces onto different lines, leaving visible
    // gaps even when their origins are exactly spacing metres apart.
    std::vector<float> distance(curve.size(), 0.0f);
    for (size_t i = 1; i < curve.size(); ++i) {
        const float dx = curve[i][0] - curve[i - 1][0];
        const float dz = curve[i][2] - curve[i - 1][2];
        distance[i] = distance[i - 1] + std::sqrt(dx * dx + dz * dz);
    }
    const float totalLength = distance.back();
    if (!(totalLength > 1e-6f) || !std::isfinite(totalLength)) return placements;

    const float requestedCount = totalLength / spline.spacing;
    size_t segmentCount = spline.closed
        ? (size_t)(std::max)(1.0f, std::round(requestedCount))
        : (size_t)(std::max)(1.0f, std::ceil(requestedCount));
    segmentCount = (std::min)(segmentCount, kMaxSegments);
    const float interval = totalLength / (float)segmentCount;

    const auto sampleAtDistance = [&](float wanted) {
        if (wanted <= 0.0f) return curve.front();
        if (wanted >= totalLength) return curve.back();
        const auto upper = std::upper_bound(distance.begin(), distance.end(), wanted);
        const size_t end = (size_t)(upper - distance.begin());
        const size_t begin = end - 1;
        const float span = distance[end] - distance[begin];
        const float t = span > 1e-6f ? (wanted - distance[begin]) / span : 0.0f;
        return std::array<float, 3>{
            curve[begin][0] + (curve[end][0] - curve[begin][0]) * t,
            curve[begin][1] + (curve[end][1] - curve[begin][1]) * t,
            curve[begin][2] + (curve[end][2] - curve[begin][2]) * t
        };
    };

    placements.reserve(segmentCount);
    for (size_t i = 0; i < segmentCount; ++i) {
        const std::array<float, 3> start = sampleAtDistance(interval * (float)i);
        const std::array<float, 3> end = sampleAtDistance(
            i + 1 == segmentCount ? totalLength : interval * (float)(i + 1));
        const float dx = end[0] - start[0];
        const float dz = end[2] - start[2];
        const float chordLength = std::sqrt(dx * dx + dz * dz);
        if (chordLength <= 1e-6f) continue;

        SplineSegmentPlacement placement;
        placement.position[0] = start[0];
        placement.position[2] = start[2];
        placement.position[1] = spline.conformToTerrain && terrainHeight
            ? terrainHeight(start[0], start[2])
            : start[1];

        if (spline.alignToPath) {
            // The repeated asset starts at its origin and runs along local +X.
            placement.yawDegrees =
                std::atan2(-dz, dx) * 57.29577951308232f;
        }
        placement.yawDegrees += spline.yawOffset;

        float fittedLength = chordLength;
        if (spline.pitchToSlope && spline.conformToTerrain && terrainHeight) {
            const float endHeight = terrainHeight(end[0], end[2]);
            const float rise = endHeight - placement.position[1];
            // Include the rise in the fitted length so pitching does not shorten
            // the segment's horizontal reach and reopen the joint.
            fittedLength = std::sqrt(chordLength * chordLength + rise * rise);
            placement.pitchDegrees =
                std::atan2(-rise, chordLength) * 57.29577951308232f;
        }
        placement.lengthScale = fittedLength / spline.spacing;
        placements.push_back(placement);
    }
    return placements;
}

void BakeSplineEntities(LevelDefinition& level, uint64_t& nextId,
                        const std::function<float(float, float)>& terrainHeight) {
    // Drop previously baked segments. Hand-placed entities never carry the
    // owner key, so they survive untouched.
    level.entities.erase(
        std::remove_if(level.entities.begin(), level.entities.end(),
                       [](const LevelEntity& entity) {
                           return entity.overrides.contains(kSplineOwnerKey);
                       }),
        level.entities.end());

    for (const LevelSplinePath& spline : level.splines) {
        if (spline.prefabId.empty()) continue;
        const std::vector<SplineSegmentPlacement> placements =
            EvaluateSplineSegments(spline, terrainHeight);
        for (size_t i = 0; i < placements.size(); ++i) {
            const SplineSegmentPlacement& placement = placements[i];
            LevelEntity entity;
            entity.id = nextId++;
            entity.type = LevelEntityType::Prefab;
            entity.prefabId = spline.prefabId;
            entity.name = (spline.name.empty() ? std::string("Spline")
                                               : spline.name) +
                          " " + std::to_string(i + 1);
            entity.overrides[kSplineOwnerKey] = spline.id;
            entity.transform.position[0] = placement.position[0];
            entity.transform.position[1] = placement.position[1];
            entity.transform.position[2] = placement.position[2];
            entity.transform.rotation[0] = placement.pitchDegrees;
            entity.transform.rotation[1] = placement.yawDegrees;
            entity.transform.scale[0] = placement.lengthScale;
            level.entities.push_back(std::move(entity));
        }
    }
}

const char* TerrainSculptOperationName(TerrainSculptOperation operation) {
    switch (operation) {
    case TerrainSculptOperation::Add: return "add";
    case TerrainSculptOperation::Flatten: return "flatten";
    case TerrainSculptOperation::Heightmap: return "stamp";
    case TerrainSculptOperation::Crater: return "crater";
    }
    return "add";
}

const char* LevelEntityTypeName(LevelEntityType type) {
    switch (type) {
    case LevelEntityType::PlayerSpawn: return "player_spawn";
    case LevelEntityType::WoodHouse: return "wood_house";
    case LevelEntityType::MetalHouse: return "metal_house";
    case LevelEntityType::Palm: return "palm";
    case LevelEntityType::ExplosiveBarrel: return "explosive_barrel";
    case LevelEntityType::EnemySpawn: return "enemy_spawn";
    case LevelEntityType::AllySpawn: return "ally_spawn";
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

const char* LevelInsertionModeName(LevelInsertionMode mode) {
    switch (mode) {
    case LevelInsertionMode::Helicopter: return "helicopter";
    case LevelInsertionMode::Boat: return "boat";
    case LevelInsertionMode::FastRappel: return "fast_rappel";
    case LevelInsertionMode::PlayerChoice: return "player_choice";
    }
    return "helicopter";
}

bool ParseLevelInsertionMode(const std::string& text, LevelInsertionMode& mode) {
    for (uint32_t i = static_cast<uint32_t>(LevelInsertionMode::Helicopter);
         i <= static_cast<uint32_t>(LevelInsertionMode::PlayerChoice); ++i) {
        const auto candidate = static_cast<LevelInsertionMode>(i);
        if (text == LevelInsertionModeName(candidate)) {
            mode = candidate;
            return true;
        }
    }
    return false;
}

LevelDefinition MakeFlatLevelTemplate() {
    LevelDefinition level;
    level.name = "Flat Level";
    level.terrainFlat = true;
    // The plane is level regardless, but zeroing the scale keeps the Inspector
    // honest -- a non-zero Terrain Height on flat ground reads as a bug.
    level.terrainHeightScale = 0.0f;
    // One spawn and nothing else. A level with no PlayerSpawn fails validation,
    // and there is nowhere to stand otherwise.
    level.entities.push_back(MakeEntity(1, LevelEntityType::PlayerSpawn,
        "Player Spawn", 0.0f, 5.0f, 0.0f, 0.0f));
    return level;
}

LevelDefinition MakeLevelOneTemplate() {
    LevelDefinition level;
    level.name = "Level 1 Copy";
    // Level 1 is built here in code, not loaded from Content/Levels, so it
    // does not pick up the dxrDDGI block the island JSONs carry and keeps the
    // struct default of enabled=false: it runs on the legacy grid. Sparse DXR
    // DDGI is opt-in via the UI or a JSON level.
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
    if (!std::isfinite(level.deploymentRadius) ||
        level.deploymentRadius < kMinDeploymentRadius ||
        level.deploymentRadius > kMaxDeploymentRadius)
        result.errors.push_back("deployment radius must be between 5 and 600");
    if (!std::isfinite(level.terrainHeightScale) ||
        level.terrainHeightScale < 0.0f || level.terrainHeightScale > 50.0f)
        result.errors.push_back("terrain heightScale must be between 0 and 50");
    if (level.terrainSculpt.size() > kMaxTerrainSculptStamps)
        result.errors.push_back("terrain sculpt supports at most " +
            std::to_string(kMaxTerrainSculptStamps) + " stamps");
    if (level.terrainTilesX < 4 || level.terrainTilesX > 48 ||
        level.terrainTilesZ < 4 || level.terrainTilesZ > 48)
        result.errors.push_back("terrain tile extent must be between 4 and 48");
    if (!std::isfinite(level.terrainIslandScaleX) ||
        level.terrainIslandScaleX < 0.5f || level.terrainIslandScaleX > 12.0f ||
        !std::isfinite(level.terrainIslandScaleZ) ||
        level.terrainIslandScaleZ < 0.5f || level.terrainIslandScaleZ > 12.0f)
        result.errors.push_back("terrain island scale must be between 0.5 and 12");
    if (level.terrainOriginTileX < -256 || level.terrainOriginTileX > 256 ||
        level.terrainOriginTileZ < -256 || level.terrainOriginTileZ > 256)
        result.errors.push_back("terrain origin tile offset out of range");
    const LevelDXRDDGISettings& gi = level.dxrDDGI;
    if (!std::isfinite(gi.surfaceSpacing) || gi.surfaceSpacing < 0.25f ||
        gi.surfaceSpacing > 50.0f || !std::isfinite(gi.surfaceOffset) ||
        gi.surfaceOffset < 0.0f || gi.surfaceOffset > 5.0f ||
        gi.maxProbes == 0 || gi.maxProbes > kMaxDDGIProbes ||
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
        const float maxStampRadius =
            stamp.operation == TerrainSculptOperation::Heightmap
                ? kMaxBakedTerrainStampRadius : 64.0f;
        if (!std::isfinite(stamp.x) || !std::isfinite(stamp.z) ||
            !std::isfinite(stamp.radius) || !std::isfinite(stamp.value) ||
            !std::isfinite(stamp.strength) || !std::isfinite(stamp.rotation) ||
            !std::isfinite(stamp.replace) || !std::isfinite(stamp.baseHeight) ||
            !std::isfinite(stamp.edgeFalloff) ||
            stamp.edgeFalloff < 0.0f || stamp.edgeFalloff > 1.0f ||
            stamp.replace < 0.0f || stamp.replace > 1.0f ||
            stamp.radius < 0.1f ||
            stamp.radius > maxStampRadius || stamp.strength < 0.0f ||
            stamp.strength > 10.0f ||
            (stamp.operation != TerrainSculptOperation::Add &&
             stamp.operation != TerrainSculptOperation::Flatten &&
             stamp.operation != TerrainSculptOperation::Heightmap &&
             stamp.operation != TerrainSculptOperation::Crater) ||
            (stamp.operation == TerrainSculptOperation::Heightmap &&
             !IsTerrainStampFilename(stamp.texture)))
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
    // Spline ids share the entity id space: a baked segment records its owner's
    // id, so a collision would make a re-bake delete the wrong entities.
    for (const LevelSplinePath& spline : level.splines) {
        if (!spline.id || !ids.insert(spline.id).second)
            result.errors.push_back("spline IDs must be unique and non-zero");
        if (spline.name.empty())
            result.errors.push_back("spline " + std::to_string(spline.id) +
                                    " has an empty name");
        if (spline.prefabId.empty())
            result.errors.push_back("spline " + std::to_string(spline.id) +
                                    " has no prefab ID");
        if (!std::isfinite(spline.spacing) || spline.spacing <= 0.0f ||
            spline.spacing > 500.0f)
            result.errors.push_back("spline " + std::to_string(spline.id) +
                                    " spacing must be between 0 and 500");
        if (!std::isfinite(spline.yawOffset))
            result.errors.push_back("spline " + std::to_string(spline.id) +
                                    " has a non-finite yaw offset");
        if (spline.points.size() < 2)
            result.errors.push_back("spline " + std::to_string(spline.id) +
                                    " needs at least two points");
        for (const LevelSplinePoint& point : spline.points) {
            if (!Finite3(point.position)) {
                result.errors.push_back("spline " + std::to_string(spline.id) +
                                        " has a non-finite point");
                break;
            }
        }
    }
    result.ok = result.errors.empty();
    return result;
}

LevelLoadResult LoadLevel(const std::filesystem::path& path) {
    LevelLoadResult result;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open file");
        // A file that is empty, or sized but full of NULs, is the signature of a
        // save whose bytes never reached the disk. Say so plainly rather than
        // letting the JSON parser report a confusing error at line 1.
        {
            const int first = stream.peek();
            if (first == std::ifstream::traits_type::eof())
                throw std::runtime_error(
                    "file is empty; the save did not complete");
            if (first == 0)
                throw std::runtime_error(
                    "file contains no data (all zero bytes); the save was lost "
                    "before reaching disk");
        }
        json root;
        stream >> root;
        if (!root.is_object()) throw std::runtime_error("root must be an object");
        LevelDefinition level;
        level.schemaVersion = root.at("schemaVersion").get<uint32_t>();
        level.name = root.at("name").get<std::string>();
        // Absent on levels saved before insertion modes existed, which all
        // arrived by helicopter.
        if (root.contains("insertionMode")) {
            const json& mode = root.at("insertionMode");
            if (!mode.is_string())
                throw std::runtime_error("insertionMode must be a string");
            if (!ParseLevelInsertionMode(mode.get<std::string>(),
                                         level.insertionMode))
                throw std::runtime_error("unknown insertionMode '" +
                                         mode.get<std::string>() + "'");
        }
        // Old levels used the historical 34 m ring and did not save this key.
        level.deploymentRadius = root.value(
            "deploymentRadius", kDefaultDeploymentRadius);
        // Older maps all shipped with the patrol boat, so absence preserves
        // their existing behavior.
        level.patrolBoatEnabled = root.value("patrolBoatEnabled", true);
        level.terrainHeightScale = root.at("terrain").at("heightScale").get<float>();
        {
            const json& terrainRoot = root.at("terrain");
            // Absent on every level authored before flat mode existed, and
            // false is exactly the procedural island they were built as.
            level.terrainFlat = terrainRoot.value("flat", false);
            level.terrainTilesX = terrainRoot.value("tilesX", 16u);
            level.terrainTilesZ = terrainRoot.value("tilesZ", 16u);
            // Back-compat: old files stored a single "islandScale"; use it as
            // the default for both axes, then let per-axis keys override.
            const float legacyScale = terrainRoot.value("islandScale", 1.0f);
            level.terrainIslandScaleX =
                terrainRoot.value("islandScaleX", legacyScale);
            level.terrainIslandScaleZ =
                terrainRoot.value("islandScaleZ", legacyScale);
            level.terrainOriginTileX = terrainRoot.value("originTileX", 0);
            level.terrainOriginTileZ = terrainRoot.value("originTileZ", 0);
        }
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
                else if (operation == "stamp")
                    stamp.operation = TerrainSculptOperation::Heightmap;
                else if (operation == "crater")
                    stamp.operation = TerrainSculptOperation::Crater;
                else throw std::runtime_error("unknown terrain sculpt operation: " + operation);
                stamp.value = source.at("value").get<float>();
                stamp.strength = source.value("strength", 1.0f);
                stamp.texture = source.value("texture", std::string{});
                stamp.rotation = source.value("rotation", 0.0f);
                // Absent on levels saved before replace mode existed; those
                // stamps were all additive, which is exactly what 0 means.
                stamp.replace = source.value("replace", 0.0f);
                stamp.baseHeight = source.value("baseHeight", 0.0f);
                // Absent before the feather was tunable; 0.82 is the width it
                // was hardcoded to, so those levels render exactly as before.
                stamp.edgeFalloff = source.value("edgeFalloff", 0.82f);
                level.terrainSculpt.push_back(stamp);
            }
        }
        // Absent on every level saved before foliage clearing existed, which
        // simply keeps their full procedural scatter.
        if (terrain.contains("foliageClear")) {
            if (!terrain.at("foliageClear").is_array())
                throw std::runtime_error("terrain foliageClear must be an array");
            for (const json& source : terrain.at("foliageClear")) {
                FoliageClearStamp stamp;
                stamp.x = source.at("x").get<float>();
                stamp.z = source.at("z").get<float>();
                stamp.radius = source.at("radius").get<float>();
                level.foliageClear.push_back(stamp);
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
        // Authored prefab runs. Optional: levels saved before splines existed
        // simply have no such key and load exactly as they always did.
        if (root.contains("splines")) {
            if (!root.at("splines").is_array())
                throw std::runtime_error("splines must be an array");
            for (const json& source : root.at("splines")) {
                LevelSplinePath spline;
                spline.id = source.at("id").get<uint64_t>();
                spline.name = source.value("name", "");
                spline.prefabId = source.value("prefab", "");
                spline.spacing = source.value("spacing", 3.108f);
                spline.yawOffset = source.value("yawOffset", 0.0f);
                spline.alignToPath = source.value("alignToPath", true);
                spline.conformToTerrain = source.value("conformToTerrain", true);
                spline.pitchToSlope = source.value("pitchToSlope", true);
                spline.closed = source.value("closed", false);
                const json& points = source.at("points");
                if (!points.is_array())
                    throw std::runtime_error("spline points must be an array");
                for (const json& point : points) {
                    LevelSplinePoint control;
                    if (!ReadVec3(point, control.position))
                        throw std::runtime_error(
                            "spline point must be a finite vec3");
                    spline.points.push_back(control);
                }
                level.splines.push_back(std::move(spline));
            }
        }
        // Painted terrain weights, if this level has a sidecar. A missing file
        // is the normal case and not an error. A malformed or non-square one is
        // also non-fatal: the level still loads and simply falls back to purely
        // procedural terrain, which is strictly better than refusing to open a
        // map because its paint data is damaged.
        {
            const std::filesystem::path splatPath =
                TerrainSplatSidecarPath(path);
            std::error_code ignored;
            if (std::filesystem::exists(splatPath, ignored)) {
                std::vector<unsigned char> pixels;
                int width = 0, height = 0;
                if (GLBImporter::LoadPixelsRGBA(splatPath.string(), pixels,
                                                width, height) &&
                    width > 0 && width == height &&
                    pixels.size() == static_cast<size_t>(width) * height * 4u) {
                    level.terrainSplatResolution = static_cast<uint32_t>(width);
                    level.terrainSplatRGBA = std::move(pixels);
                }
            }
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

std::filesystem::path TerrainSplatSidecarPath(
    const std::filesystem::path& levelPath) {
    std::filesystem::path sidecar = levelPath;
    sidecar.replace_extension();
    sidecar += "_splat.png";
    return sidecar;
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
            // Spline segments are regenerated from their control points on
            // load, so writing them too would duplicate the whole run on every
            // save/load round trip.
            if (entity.overrides.contains(kSplineOwnerKey)) continue;
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
            json saved = {
                {"x", stamp.x}, {"z", stamp.z}, {"radius", stamp.radius},
                {"operation", TerrainSculptOperationName(stamp.operation)},
                {"value", stamp.value}, {"strength", stamp.strength}
            };
            if (stamp.operation == TerrainSculptOperation::Heightmap) {
                saved["texture"] = stamp.texture;
                saved["rotation"] = stamp.rotation;
                saved["replace"] = stamp.replace;
                saved["baseHeight"] = stamp.baseHeight;
                saved["edgeFalloff"] = stamp.edgeFalloff;
            }
            sculpt.push_back(std::move(saved));
        }
        json foliageClear = json::array();
        for (const FoliageClearStamp& stamp : level.foliageClear) {
            foliageClear.push_back({
                {"x", stamp.x}, {"z", stamp.z}, {"radius", stamp.radius}
            });
        }
        json splines = json::array();
        for (const LevelSplinePath& spline : level.splines) {
            json points = json::array();
            for (const LevelSplinePoint& point : spline.points)
                points.push_back(Vec3(point.position));
            splines.push_back({
                {"id", spline.id}, {"name", spline.name},
                {"prefab", spline.prefabId}, {"spacing", spline.spacing},
                {"yawOffset", spline.yawOffset},
                {"alignToPath", spline.alignToPath},
                {"conformToTerrain", spline.conformToTerrain},
                {"pitchToSlope", spline.pitchToSlope},
                {"closed", spline.closed},
                {"points", std::move(points)}
            });
        }
        json root = {
            {"schemaVersion", level.schemaVersion}, {"name", level.name},
            {"insertionMode", LevelInsertionModeName(level.insertionMode)},
            {"deploymentRadius", level.deploymentRadius},
            {"patrolBoatEnabled", level.patrolBoatEnabled},
            {"terrain", {{"heightScale", level.terrainHeightScale},
                         {"flat", level.terrainFlat},
                         {"tilesX", level.terrainTilesX},
                         {"tilesZ", level.terrainTilesZ},
                         {"islandScaleX", level.terrainIslandScaleX},
                         {"islandScaleZ", level.terrainIslandScaleZ},
                         {"originTileX", level.terrainOriginTileX},
                         {"originTileZ", level.terrainOriginTileZ},
                         {"sculpt", std::move(sculpt)},
                         {"foliageClear", std::move(foliageClear)}}},
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
        // Only levels that actually use splines gain the key, so files authored
        // before this feature round-trip unchanged.
        if (!splines.empty()) root["splines"] = std::move(splines);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("cannot open temporary file");
            stream << root.dump(2) << '\n';
            if (!stream) throw std::runtime_error("failed while writing temporary file");
            // Close explicitly: the destructor swallows a failure from the final
            // flush, which would leave us renaming a half-written file into place.
            stream.close();
            if (!stream) throw std::runtime_error("failed while closing temporary file");
        }
#ifdef _WIN32
        // Push the bytes out of the OS cache before the rename. Without this a
        // crash or power loss between the two can commit the file's new length
        // while its contents are still unwritten, leaving a correctly sized file
        // full of zeros that no longer parses as JSON.
        {
            HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                throw std::runtime_error("cannot reopen temporary file to flush");
            const BOOL flushed = FlushFileBuffers(file);
            CloseHandle(file);
            if (!flushed) throw std::runtime_error("cannot flush temporary file");
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace destination file");
#else
        std::filesystem::rename(temporary, path);
#endif

        // Painted terrain weights, written only after the JSON has landed so a
        // failed save never leaves an orphaned sidecar describing a level that
        // was not written. A level with no painting deletes any stale sidecar
        // instead of leaving one behind for the next load to pick up.
        const std::filesystem::path splatPath = TerrainSplatSidecarPath(path);
        const size_t expectedSplatBytes =
            static_cast<size_t>(level.terrainSplatResolution) *
            level.terrainSplatResolution * 4u;
        if (level.terrainSplatResolution > 0 &&
            level.terrainSplatRGBA.size() == expectedSplatBytes) {
            const int written = stbi_write_png(
                splatPath.string().c_str(),
                static_cast<int>(level.terrainSplatResolution),
                static_cast<int>(level.terrainSplatResolution), 4,
                level.terrainSplatRGBA.data(),
                static_cast<int>(level.terrainSplatResolution) * 4);
            if (!written)
                throw std::runtime_error("cannot write terrain splat sidecar");
        } else {
            std::error_code ignored;
            std::filesystem::remove(splatPath, ignored);
        }

        result.ok = true;
    } catch (const std::exception& error) {
        result.error = path.string() + ": " + error.what();
    }
    return result;
}
