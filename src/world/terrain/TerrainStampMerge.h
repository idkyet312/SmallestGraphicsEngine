#pragma once

#include "TerrainStampBake.h"
#include <limits>
#include <set>

inline constexpr size_t kTerrainAutoMergeMinStamps = 32;
// 512 samples over at most 64 m keep merges finer than the closest mesh ring.
inline constexpr float kTerrainAutoMergeMaxSpan = 64.0f;

struct TerrainStampMergePlan {
    size_t first = 0;
    TerrainBakeBounds bounds;
    bool valid = false;
};

inline TerrainBakeBounds TerrainStampMergeBounds(const TerrainSculptStamp& stamp) {
    float reach = stamp.radius;
    if (stamp.operation == TerrainSculptOperation::Heightmap) {
        const float angle = stamp.rotation * 0.01745329252f;
        reach *= std::abs(std::cos(angle)) + std::abs(std::sin(angle));
    } else if (stamp.operation == TerrainSculptOperation::Crater) {
        // The cut's ejecta lip continues beyond its nominal radius.
        reach *= 1.18f;
    }
    if (!(reach > 0.0f) || !std::isfinite(reach) ||
        !std::isfinite(stamp.x) || !std::isfinite(stamp.z)) return {};
    return { stamp.x - reach, stamp.z - reach,
             stamp.x + reach, stamp.z + reach, true };
}

inline TerrainStampMergePlan PlanTerrainStampMerge(
        const std::vector<TerrainSculptStamp>& stamps) {
    TerrainStampMergePlan plan;
    plan.first = stamps.size();
    // Only a contiguous suffix can be replaced without moving a flatten or
    // replace operation across another edit. Distant edits stop this walk.
    for (size_t i = stamps.size(); i > 0; --i) {
        TerrainBakeBounds next = TerrainStampMergeBounds(stamps[i - 1]);
        if (!next.valid || IsTerrainStampBakeFilename(stamps[i - 1].texture)) break;
        if (plan.bounds.valid) {
            next.minX = (std::min)(next.minX, plan.bounds.minX);
            next.minZ = (std::min)(next.minZ, plan.bounds.minZ);
            next.maxX = (std::max)(next.maxX, plan.bounds.maxX);
            next.maxZ = (std::max)(next.maxZ, plan.bounds.maxZ);
        }
        const float span = (std::max)(next.maxX - next.minX, next.maxZ - next.minZ);
        if ((std::max)(span, 1.0f) * 1.02f > kTerrainAutoMergeMaxSpan) break;
        plan.bounds = next;
        plan.first = i - 1;
    }
    if (stamps.size() - plan.first < kTerrainAutoMergeMinStamps) return plan;

    // Reserve a layer for the new image before writing anything. Never discard
    // authored textures when the live level itself fills the atlas.
    std::set<std::string> textures;
    for (size_t i = 0; i < plan.first; ++i) {
        if (stamps[i].operation == TerrainSculptOperation::Heightmap &&
            !IsTerrainStampBakeFilename(stamps[i].texture))
            textures.insert(stamps[i].texture);
    }
    plan.valid = textures.size() < kMaxTerrainStampTextures;
    return plan;
}

// GPU buffers already have per-frame copies and dirty-region uploads. Reusing
// an inactive atlas layer changes texels, never descriptors or GPU lifetimes.
inline size_t FindUnusedTerrainStampLayer(
        const std::vector<std::string>& residentNames,
        const std::vector<TerrainSculptStamp>& activeStamps,
        const std::string& previewTexture = {}) {
    for (size_t layer = 0; layer < residentNames.size(); ++layer) {
        if (residentNames[layer] == previewTexture) continue;
        const bool active = std::any_of(activeStamps.begin(), activeStamps.end(),
            [&](const TerrainSculptStamp& stamp) {
                return stamp.operation == TerrainSculptOperation::Heightmap &&
                    stamp.texture == residentNames[layer];
            });
        if (!active) return layer;
    }
    return (std::numeric_limits<size_t>::max)();
}
