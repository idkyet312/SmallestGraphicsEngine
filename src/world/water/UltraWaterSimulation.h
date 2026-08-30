#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

enum class WaterInteractionType : uint32_t {
    Splash = 0,
    Debris = 1,
    Wake = 2
};

struct WaterBathymetryDesc {
    DirectX::XMFLOAT2 minimumXZ = {};
    DirectX::XMFLOAT2 maximumXZ = {};
    uint32_t resolution = 0;
    uint64_t terrainRevision = 0;
    std::function<float(float, float)> heightAt;

    bool Valid() const {
        return resolution >= 2 && maximumXZ.x > minimumXZ.x &&
               maximumXZ.y > minimumXZ.y && static_cast<bool>(heightAt);
    }
};

namespace UltraWaterDetail {

inline void SquaredDistanceTransform1D(
    const float* source, uint32_t count, float spacing, float* output,
    std::vector<uint32_t>& sites, std::vector<float>& boundaries) {
    const float infinity = (std::numeric_limits<float>::infinity)();
    uint32_t siteCount = 0;
    spacing = (std::max)(std::abs(spacing), 1e-4f);

    for (uint32_t q = 0; q < count; ++q) {
        if (!std::isfinite(source[q])) continue;
        const float qPosition = static_cast<float>(q) * spacing;
        if (siteCount == 0) {
            sites[0] = q;
            boundaries[0] = -infinity;
            boundaries[1] = infinity;
            siteCount = 1;
            continue;
        }

        uint32_t k = siteCount - 1;
        float intersection = 0.0f;
        for (;;) {
            const uint32_t previous = sites[k];
            const float previousPosition =
                static_cast<float>(previous) * spacing;
            intersection =
                ((source[q] + qPosition * qPosition) -
                 (source[previous] +
                  previousPosition * previousPosition)) /
                (2.0f * (qPosition - previousPosition));
            if (intersection > boundaries[k] || k == 0) break;
            --k;
        }
        ++k;
        sites[k] = q;
        boundaries[k] = intersection;
        boundaries[k + 1] = infinity;
        siteCount = k + 1;
    }

    if (siteCount == 0) {
        std::fill(output, output + count, infinity);
        return;
    }
    uint32_t k = 0;
    for (uint32_t q = 0; q < count; ++q) {
        const float position = static_cast<float>(q) * spacing;
        while (k + 1 < siteCount && boundaries[k + 1] < position) ++k;
        const float delta = position -
            static_cast<float>(sites[k]) * spacing;
        output[q] = delta * delta + source[sites[k]];
    }
}

inline std::vector<float> SquaredDistanceToTerrainClass(
    const std::vector<float>& terrainHeights, uint32_t width,
    uint32_t height, float cellX, float cellZ, bool targetIsLand) {
    const size_t sampleCount = static_cast<size_t>(width) * height;
    if (width == 0 || height == 0 || terrainHeights.size() != sampleCount)
        return {};

    const float infinity = (std::numeric_limits<float>::infinity)();
    std::vector<float> horizontal(sampleCount, infinity);
    std::vector<float> result(sampleCount, infinity);
    const uint32_t workSize = (std::max)(width, height);
    std::vector<float> source(workSize, infinity);
    std::vector<float> transformed(workSize, infinity);
    std::vector<uint32_t> sites(workSize);
    std::vector<float> boundaries(static_cast<size_t>(workSize) + 1u);

    for (uint32_t z = 0; z < height; ++z) {
        for (uint32_t x = 0; x < width; ++x) {
            const bool isLand =
                terrainHeights[static_cast<size_t>(z) * width + x] >= 0.0f;
            source[x] = isLand == targetIsLand ? 0.0f : infinity;
        }
        SquaredDistanceTransform1D(source.data(), width, cellX,
            horizontal.data() + static_cast<size_t>(z) * width,
            sites, boundaries);
    }
    for (uint32_t x = 0; x < width; ++x) {
        for (uint32_t z = 0; z < height; ++z)
            source[z] = horizontal[static_cast<size_t>(z) * width + x];
        SquaredDistanceTransform1D(source.data(), height, cellZ,
            transformed.data(), sites, boundaries);
        for (uint32_t z = 0; z < height; ++z)
            result[static_cast<size_t>(z) * width + x] = transformed[z];
    }
    return result;
}

// Positive values are inland and negative values are offshore. Subtracting
// half a texel places zero between the opposing terrain samples instead of on
// either sample centre, so bays and headlands do not shift with resolution.
inline std::vector<float> BuildSignedShoreDistance(
    const std::vector<float>& terrainHeights, uint32_t width,
    uint32_t height, float cellX, float cellZ) {
    const size_t sampleCount = static_cast<size_t>(width) * height;
    if (terrainHeights.size() != sampleCount || sampleCount == 0) return {};
    const std::vector<float> distanceToLand =
        SquaredDistanceToTerrainClass(
            terrainHeights, width, height, cellX, cellZ, true);
    const std::vector<float> distanceToWater =
        SquaredDistanceToTerrainClass(
            terrainHeights, width, height, cellX, cellZ, false);
    std::vector<float> result(sampleCount);
    const float halfTexel = 0.5f * (std::min)(
        std::abs(cellX), std::abs(cellZ));
    const float fallbackDistance = std::hypot(
        static_cast<float>(width) * std::abs(cellX),
        static_cast<float>(height) * std::abs(cellZ));
    for (size_t i = 0; i < sampleCount; ++i) {
        const bool isLand = terrainHeights[i] >= 0.0f;
        const float squaredDistance =
            isLand ? distanceToWater[i] : distanceToLand[i];
        const float distance = std::isfinite(squaredDistance)
            ? (std::max)(std::sqrt(squaredDistance) - halfTexel, 0.0f)
            : fallbackDistance;
        result[i] = isLand ? distance : -distance;
    }
    return result;
}

// How far seaward of the real shoreline the solver bed's own waterline sits.
// The bathymetry is pushed out by this much, so the simulated wet/dry front and
// every depth contour derived from it stand off the beach instead of running
// along it.
inline constexpr float kShoreOffsetMetres = 30.0f;

inline float ShoreDistanceToBed(float signedDistance) {
    // A 40 m offshore margin reaches 14 m depth, where the deep spectrum hands
    // off fully to the coastal solver. The gentler land slope keeps the wet/dry
    // boundary stable while the actual terrain supplies the visible beach.
    //
    // The offset shifts the whole profile seaward: what was the shoreline is now
    // 30 m of shallow water, and the bed only turns to land beyond that. Slopes
    // and depth limits are unchanged, so the deep hand-off still happens at the
    // same depth, just further out.
    const float shifted = signedDistance + kShoreOffsetMetres;
    return shifted >= 0.0f
        ? (std::min)(shifted * 0.20f, 8.0f)
        : (std::max)(shifted * 0.35f, -24.0f);
}

inline std::vector<float> BuildShoreBathymetry(
    const std::vector<float>& terrainHeights, uint32_t width,
    uint32_t height, float cellX, float cellZ) {
    std::vector<float> result = BuildSignedShoreDistance(
        terrainHeights, width, height, cellX, cellZ);
    for (float& sample : result) sample = ShoreDistanceToBed(sample);
    return result;
}

}  // namespace UltraWaterDetail

struct WaterInteraction {
    DirectX::XMFLOAT2 worldXZ = {};
    float radius = 0.35f;
    float heightImpulse = 0.08f;
    DirectX::XMFLOAT2 velocityImpulse = {};
    WaterInteractionType type = WaterInteractionType::Splash;
};

struct WaterHeightQuery {
    uint64_t objectId = 0;
    DirectX::XMFLOAT2 worldXZ = {};
};

struct WaterHeightResult {
    uint64_t objectId = 0;
    float height = 0.0f;
    DirectX::XMFLOAT2 slope = {};
};

struct UltraWaterTuning {
    float waveHeight = 1.0f;
    float waveScale = 1.0f;
    float waveSpeed = 1.0f;
    float directionRadians = 0.0f;
    float choppiness = 1.0f;
    float surfStrength = 1.0f;
    float foamStrength = 1.0f;
    float coastDamping = 1.0f;
};

template <size_t Capacity, size_t FrameSlots>
class WaterHeightQueryFrameSlots {
public:
    void Submit(size_t slot, const WaterHeightQuery* queries, size_t count) {
        if (slot >= FrameSlots) return;
        counts_[slot] = (std::min)(count, Capacity);
        for (size_t i = 0; i < counts_[slot]; ++i)
            ids_[slot][i] = queries[i].objectId;
        valid_[slot] = true;
    }

    size_t Resolve(size_t slot, const DirectX::XMFLOAT4* samples,
                   WaterHeightResult* results, size_t capacity) {
        if (slot >= FrameSlots || !valid_[slot]) return 0;
        const size_t count = (std::min)(counts_[slot], capacity);
        for (size_t i = 0; i < count; ++i) {
            results[i].objectId = ids_[slot][i];
            results[i].height = samples[i].x;
            results[i].slope = {samples[i].y, samples[i].z};
        }
        valid_[slot] = false;
        return count;
    }

private:
    std::array<std::array<uint64_t, Capacity>, FrameSlots> ids_{};
    std::array<size_t, FrameSlots> counts_{};
    std::array<bool, FrameSlots> valid_{};
};

// Fixed-capacity and allocation-free in the render loop. New impacts are more
// useful than old ones, so overflow evicts the oldest event rather than
// silently dropping the bullet or hull wake the player just created.
template <size_t Capacity>
class WaterInteractionRing {
public:
    void Push(const WaterInteraction& interaction) {
        if (count_ < Capacity) {
            entries_[(head_ + count_) % Capacity] = interaction;
            ++count_;
            return;
        }
        entries_[head_] = interaction;
        head_ = (head_ + 1) % Capacity;
        ++overflowCount_;
    }

    size_t Drain(WaterInteraction* output, size_t outputCapacity) {
        const size_t written = (std::min)(count_, outputCapacity);
        for (size_t i = 0; i < written; ++i)
            output[i] = entries_[(head_ + i) % Capacity];
        head_ = (head_ + written) % Capacity;
        count_ -= written;
        return written;
    }

    size_t Size() const { return count_; }
    uint64_t OverflowCount() const { return overflowCount_; }
    void Clear() { head_ = 0; count_ = 0; }

private:
    std::array<WaterInteraction, Capacity> entries_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t overflowCount_ = 0;
};

struct UltraSpectralWave {
    DirectX::XMFLOAT2 direction = {};
    float amplitude = 0.0f;
    float wavelength = 1.0f;
    float phase = 0.0f;
    float steepness = 0.0f;
};

// Deterministic low-frequency representatives of the GPU spectrum. Physics
// and surface queries use these modes while the GPU adds the dense short-wave
// spectrum, keeping boats on the visible swell without a blocking readback.
inline std::array<UltraSpectralWave, 16> BuildUltraSpectralWaves(
    uint32_t seed = 0x53474557u) {
    std::array<UltraSpectralWave, 16> result{};
    auto random01 = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<float>(seed & 0x00ffffffu) / 16777216.0f;
    };

    constexpr float windBearing = 0.34906585f;
    for (size_t i = 0; i < result.size(); ++i) {
        const float t = static_cast<float>(i) /
            static_cast<float>(result.size() - 1);
        const float wavelength = 2.0f * std::pow(18.0f, 1.0f - t);
        const float spread = (random01() - 0.5f) * 0.78f * (0.45f + t);
        const float angle = windBearing + spread;
        const float spectralEnvelope = std::exp(-1.75f * t) *
            (0.72f + random01() * 0.28f);
        UltraSpectralWave& wave = result[i];
        wave.direction = { std::cos(angle), std::sin(angle) };
        wave.wavelength = wavelength;
        wave.amplitude = 0.055f * spectralEnvelope *
            std::sqrt(wavelength / 2.0f);
        wave.phase = random01() * DirectX::XM_2PI;
        wave.steepness = (std::min)(1.0f, 0.38f + t * 0.55f);
    }
    return result;
}

inline float EvaluateUltraSpectralHeight(
    const std::array<UltraSpectralWave, 16>& waves,
    float x, float z, float time) {
    constexpr float gravity = 9.81f;
    float height = 0.0f;
    for (const UltraSpectralWave& wave : waves) {
        const float k = DirectX::XM_2PI / wave.wavelength;
        const float omega = std::sqrt(gravity * k);
        height += wave.amplitude * std::sin(
            k * (wave.direction.x * x + wave.direction.y * z) -
            omega * time + wave.phase);
    }
    return height;
}

inline uint32_t SelectBathymetryResolution(float spanX, float spanZ) {
    const float largestSpan = (std::max)(spanX, spanZ);
    return largestSpan <= 256.0f ? 1024u : 2048u;
}

inline uint32_t SelectCoastalResolution(float spanX, float spanZ) {
    const float largestSpan = (std::max)(spanX, spanZ);
    return largestSpan <= 256.0f ? 512u : 1024u;
}

inline uint32_t UltraClipmapInnerSegments(uint32_t gridCells,
                                          uint32_t level) {
    (void)level;
    return gridCells;
}

inline uint32_t UltraClipmapOuterSegments(uint32_t gridCells,
                                          uint32_t level) {
    (void)level;
    return gridCells;
}

inline DirectX::XMFLOAT2 UltraClipmapEdgePoint(
    float extent, uint32_t side, uint32_t segment, uint32_t segments) {
    const float t = static_cast<float>(segment) /
                    static_cast<float>((std::max)(segments, 1u));
    switch (side & 3u) {
    case 0: return {-extent + 2.0f * extent * t, extent};
    case 1: return {extent, extent - 2.0f * extent * t};
    case 2: return {extent - 2.0f * extent * t, -extent};
    default: return {-extent, -extent + 2.0f * extent * t};
    }
}
