#pragma once

// Bakes a level's whole sculpt stack down to a single heightmap stamp.
//
// Every sculpt stamp is evaluated per vertex by terrain_ms.hlsl, and the height
// is sampled five times over (once for the position, four more for the
// finite-difference normal). With a full stack that loop is the dominant cost of
// the visibility raster, and it grows with every stamp the level gains. Baking
// resolves the stack once, offline, into one Replace-mode heightmap stamp: the
// terrain then looks the same but the per-vertex loop runs a single iteration.
//
// The bake is lossy in one specific way. The stack is resampled onto a fixed
// grid, so detail finer than one texel of that grid is averaged away. The
// authored stamps are kept in the level file so a bake can always be undone.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "LevelDefinition.h"
#include "TerrainStampLibrary.h"

struct TerrainBakeBounds {
    float minX = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxZ = 0.0f;
    bool valid = false;
};

// World-space extent every stamp in the stack touches. Heightmap stamps are
// squares of half-width `radius` (see SampleTerrainStamp), so a rotated one
// reaches radius * sqrt(2); the radial operations are discs of `radius`.
inline TerrainBakeBounds TerrainSculptBounds(
    const std::vector<TerrainSculptStamp>& stamps) {
    TerrainBakeBounds bounds;
    for (const TerrainSculptStamp& stamp : stamps) {
        const float reach = stamp.operation == TerrainSculptOperation::Heightmap
            ? stamp.radius * 1.41421356f
            : stamp.radius;
        if (!(reach > 0.0f)) continue;
        if (!bounds.valid) {
            bounds.minX = stamp.x - reach;
            bounds.maxX = stamp.x + reach;
            bounds.minZ = stamp.z - reach;
            bounds.maxZ = stamp.z + reach;
            bounds.valid = true;
            continue;
        }
        bounds.minX = (std::min)(bounds.minX, stamp.x - reach);
        bounds.maxX = (std::max)(bounds.maxX, stamp.x + reach);
        bounds.minZ = (std::min)(bounds.minZ, stamp.z - reach);
        bounds.maxZ = (std::max)(bounds.maxZ, stamp.z + reach);
    }
    return bounds;
}

// Minimal 16-bit grayscale PNG writer.
//
// stb_image_write only emits 8 bits per channel, which would quantise the baked
// terrain into visible terraces -- the whole point of the stamp format being
// 16-bit is that it does not. Only the deflate step is non-trivial and
// stb_image_write already exposes stbi_zlib_compress for it, so this is just the
// container: signature, IHDR, IDAT, IEND, with each row prefixed by filter 0.
//
// `compress` is stbi_zlib_compress, passed in so this header does not need the
// STB implementation macro.
bool WriteGray16PNG(const std::filesystem::path& path,
                    const std::vector<uint16_t>& gray,
                    uint32_t width, uint32_t height,
                    unsigned char* (*compress)(unsigned char*, int, int*, int));

// Result of a bake attempt. `error` is empty on success.
struct TerrainBakeResult {
    bool ok = false;
    std::string error;
    std::string texture;      // filename written into TerrainStampDirectory()
    size_t bakedStamps = 0;   // how many stamps were folded in
    float metresPerTexel = 0.0f;
};

// Evaluates `height` over the stack's bounds and writes the result as one
// heightmap stamp.
//
// `height` must return the fully sculpted world height at an XZ, i.e. the same
// function the terrain renders -- TerrainRendererDX12::HeightAt with the level's
// stamps bound. The baked stamp is Replace mode, so on reload it overwrites the
// procedural ground with exactly what was sampled here rather than adding to it.
//
// The caller supplies `stbi_zlib_compress` as `compress`.
template <typename HeightFn>
TerrainBakeResult BakeTerrainSculptToStamp(
    const std::vector<TerrainSculptStamp>& stamps,
    const HeightFn& height,
    const std::string& outputName,
    unsigned char* (*compress)(unsigned char*, int, int*, int),
    TerrainSculptStamp& outStamp) {
    TerrainBakeResult result;
    if (stamps.empty()) {
        result.error = "No sculpt stamps to bake.";
        return result;
    }
    if (!IsTerrainStampFilename(outputName)) {
        result.error = "Bake target must be a simple .png filename.";
        return result;
    }

    const TerrainBakeBounds bounds = TerrainSculptBounds(stamps);
    if (!bounds.valid) {
        result.error = "Sculpt stamps have no usable extent.";
        return result;
    }

    // The stamp is square in world space: SampleTerrainStamp maps a square of
    // half-width `radius` through the texture, so a non-square bake region would
    // be stretched on one axis. Use the larger side and centre the content.
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f;
    const float centerZ = (bounds.minZ + bounds.maxZ) * 0.5f;
    const float spanX = bounds.maxX - bounds.minX;
    const float spanZ = bounds.maxZ - bounds.minZ;
    // Half a texel of margin keeps the outermost authored detail off the very
    // edge, where the stamp's own border feather would eat it.
    const float halfSpan = (std::max)((std::max)(spanX, spanZ) * 0.5f, 0.5f) *
        1.02f;

    const uint32_t resolution = kTerrainStampResolution;
    std::vector<uint16_t> gray(static_cast<size_t>(resolution) * resolution, 0);

    // Sample the stack, tracking the range so the 16-bit codomain is used fully
    // rather than clipping tall terrain or wasting bits on a flat level.
    std::vector<float> heights(static_cast<size_t>(resolution) * resolution);
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    bool first = true;
    for (uint32_t y = 0; y < resolution; ++y) {
        // Texel centres, matching SampleTerrainStamp's uv -> texel mapping.
        const float v = (static_cast<float>(y) + 0.5f) / resolution;
        const float worldZ = centerZ + (v - 0.5f) * (halfSpan * 2.0f);
        for (uint32_t x = 0; x < resolution; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / resolution;
            const float worldX = centerX + (u - 0.5f) * (halfSpan * 2.0f);
            const float h = height(worldX, worldZ);
            heights[static_cast<size_t>(y) * resolution + x] = h;
            if (first) {
                minHeight = maxHeight = h;
                first = false;
            } else {
                minHeight = (std::min)(minHeight, h);
                maxHeight = (std::max)(maxHeight, h);
            }
        }
    }

    // The shader reconstructs relief as (sample * 2 - 1) * value, so a stamp
    // encodes a symmetric range about its midpoint. Centre the baked terrain in
    // that range and store the half-range as the stamp's value.
    const float midHeight = (minHeight + maxHeight) * 0.5f;
    const float halfRange = (std::max)((maxHeight - minHeight) * 0.5f, 1e-4f);
    for (size_t i = 0; i < heights.size(); ++i) {
        const float normalized = (heights[i] - midHeight) / halfRange;
        const float encoded = (normalized * 0.5f + 0.5f) * 65535.0f;
        gray[i] = static_cast<uint16_t>(
            (std::min)(65535.0f, (std::max)(0.0f, encoded)) + 0.5f);
    }

    std::error_code error;
    std::filesystem::create_directories(TerrainStampDirectory(), error);
    const std::filesystem::path path = TerrainStampDirectory() / outputName;
    if (!WriteGray16PNG(path, gray, resolution, resolution, compress)) {
        result.error = "Failed to write " + path.string();
        return result;
    }

    outStamp = TerrainSculptStamp{};
    outStamp.x = centerX;
    outStamp.z = centerZ;
    outStamp.radius = halfSpan;
    outStamp.operation = TerrainSculptOperation::Heightmap;
    outStamp.value = halfRange;
    outStamp.strength = 1.0f;
    outStamp.texture = outputName;
    outStamp.rotation = 0.0f;
    // Full replace: the bake already contains the procedural ground it was
    // sampled from, so adding it on top would apply that ground twice.
    outStamp.replace = 1.0f;
    outStamp.baseHeight = midHeight;

    result.ok = true;
    result.texture = outputName;
    result.bakedStamps = stamps.size();
    result.metresPerTexel = (halfSpan * 2.0f) / static_cast<float>(resolution);
    return result;
}
