#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

inline constexpr uint32_t kTerrainRockMaskResolution = 512;
inline constexpr size_t kTerrainRockMaskTexels =
    size_t(kTerrainRockMaskResolution) * kTerrainRockMaskResolution;

// Match texture sampling's half-texel convention, including stretched islands.
inline float SampleTerrainRockMask(const uint16_t* mask, uint32_t side,
                                  float u, float v) {
    if (!mask || side == 0 || u < 0 || u > 1 || v < 0 || v > 1) return 0;
    const float px = (std::max)(0.0f, (std::min)(float(side - 1), u * side - 0.5f));
    const float py = (std::max)(0.0f, (std::min)(float(side - 1), v * side - 0.5f));
    const uint32_t x0 = uint32_t(px), y0 = uint32_t(py);
    const uint32_t x1 = (std::min)(x0 + 1, side - 1);
    const uint32_t y1 = (std::min)(y0 + 1, side - 1);
    const float fx = px - x0, fy = py - y0;
    const float a = mask[size_t(y0) * side + x0] / 65535.0f;
    const float b = mask[size_t(y0) * side + x1] / 65535.0f;
    const float c = mask[size_t(y1) * side + x0] / 65535.0f;
    const float d = mask[size_t(y1) * side + x1] / 65535.0f;
    const float upper = a + (b - a) * fx;
    const float lower = c + (d - c) * fx;
    return upper + (lower - upper) * fy;
}

inline std::vector<uint16_t> BuildTerrainRockMask(const uint8_t* rgba,
                                               uint32_t resolution) {
    std::vector<uint16_t> result(kTerrainRockMaskTexels, 0);
    if (!rgba || resolution == 0) return result;
    std::vector<uint16_t> source(size_t(resolution) * resolution);
    for (size_t i = 0; i < source.size(); ++i) {
        const uint8_t* p = rgba + i * 4;
        const uint32_t total = uint32_t(p[0]) + p[1] + p[2] + p[3];
        const uint32_t coverage = (std::max)({p[0], p[1], p[2], p[3]});
        // Dirt and rock share protection. Normalised paint times coverage
        // keeps faint strokes and grass/sand blends only partly protected.
        const uint32_t protectedPaint = uint32_t(p[1]) + p[3];
        source[i] = total ? uint16_t((uint64_t(protectedPaint) * coverage * 65535u +
            uint64_t(total) * 255u / 2u) / (uint64_t(total) * 255u)) : 0;
    }
    if (resolution == kTerrainRockMaskResolution) return source;
    for (uint32_t y = 0; y < kTerrainRockMaskResolution; ++y)
        for (uint32_t x = 0; x < kTerrainRockMaskResolution; ++x)
            result[size_t(y) * kTerrainRockMaskResolution + x] = uint16_t(
                SampleTerrainRockMask(source.data(), resolution,
                    (x + 0.5f) / kTerrainRockMaskResolution,
                    (y + 0.5f) / kTerrainRockMaskResolution) * 65535.0f + 0.5f);
    return result;
}

inline float TerrainDestructionWeight(float rock) {
    rock = (std::max)(0.0f, (std::min)(1.0f, rock));
    return 1.0f - rock * rock * (3.0f - 2.0f * rock);
}

inline float BlendTerrainDestruction(float h, float destroyed, float weight) {
    if (weight >= 1.0f) return destroyed;
    if (weight <= 0.0f) return h;
    return h + (destroyed - h) * weight;
}
