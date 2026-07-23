#ifndef DXR_PROBE_LAYOUT_H
#define DXR_PROBE_LAYOUT_H

#include "LevelDefinition.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <vector>

struct DXRProbeTriangle {
    DirectX::XMFLOAT3 a;
    DirectX::XMFLOAT3 b;
    DirectX::XMFLOAT3 c;
    uint64_t sourceId = 0;
};

enum class DXRProbeState : uint32_t {
    Pending = 0,
    Valid = 1,
    Rejected = 2
};

struct DXRProbeRecord {
    DirectX::XMFLOAT3 position = {};
    float radius = 0.0f;
    DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
    DXRProbeState state = DXRProbeState::Pending;
    uint64_t stableId = 0;
    uint32_t lastUpdatedFrame = 0;
    uint32_t padding = 0;
};

struct DXRProbeGridCell {
    int32_t x = std::numeric_limits<int32_t>::min();
    int32_t y = std::numeric_limits<int32_t>::min();
    int32_t z = std::numeric_limits<int32_t>::min();
    uint32_t offset = 0;
    uint32_t count = 0;
    uint32_t padding[3] = {};
};

class DXRProbeLayout {
public:
    std::vector<DXRProbeRecord> probes;
    std::vector<DXRProbeGridCell> cells;
    std::vector<uint32_t> cellProbeIndices;
    float cellSize = 6.0f;
    uint64_t sourceHash = 0;

    void Clear() {
        probes.clear();
        cells.clear();
        cellProbeIndices.clear();
        sourceHash = 0;
    }

    bool Build(const std::vector<DXRProbeTriangle>& triangles,
               const LevelDXRDDGISettings& settings,
               uint64_t geometryHash) {
        Clear();
        sourceHash = geometryHash;
        cellSize = settings.surfaceSpacing * 2.0f;
        if (triangles.empty() || settings.maxProbes == 0) return false;

        struct Candidate {
            DXRProbeRecord probe;
            int32_t qx;
            int32_t qy;
            int32_t qz;
        };
        struct QuantizedKey {
            int32_t x, y, z;
            bool operator==(const QuantizedKey& other) const {
                return x == other.x && y == other.y && z == other.z;
            }
        };
        struct KeyHash {
            size_t operator()(const QuantizedKey& key) const {
                return static_cast<size_t>(HashCoords(key.x, key.y, key.z));
            }
        };

        const float spacing = settings.surfaceSpacing;
        const float quantization = (std::max)(spacing * 0.5f, 0.125f);
        std::unordered_map<QuantizedKey, Candidate, KeyHash> unique;
        unique.reserve((std::min)(static_cast<size_t>(settings.maxProbes) * 4u,
                                  triangles.size() * 2u));

        for (const DXRProbeTriangle& triangle : triangles) {
            using namespace DirectX;
            const XMVECTOR a = XMLoadFloat3(&triangle.a);
            const XMVECTOR b = XMLoadFloat3(&triangle.b);
            const XMVECTOR c = XMLoadFloat3(&triangle.c);
            XMVECTOR cross = XMVector3Cross(XMVectorSubtract(b, a),
                                            XMVectorSubtract(c, a));
            const float doubleArea = XMVectorGetX(XMVector3Length(cross));
            if (!std::isfinite(doubleArea) || doubleArea <= 1e-6f) continue;
            const XMVECTOR normalVector = XMVectorScale(cross, 1.0f / doubleArea);
            XMFLOAT3 normal;
            XMStoreFloat3(&normal, normalVector);
            const float area = doubleArea * 0.5f;
            const uint32_t desiredSamples = static_cast<uint32_t>(
                std::ceil(area / (spacing * spacing)));
            const uint32_t sampleCount =
                (std::min)(64u, (std::max)(1u, desiredSamples));

            for (uint32_t sample = 0; sample < sampleCount; ++sample) {
                const float u = (static_cast<float>(sample) + 0.5f) /
                                static_cast<float>(sampleCount);
                const float v = RadicalInverse(sample ^
                    static_cast<uint32_t>(triangle.sourceId));
                const float root = std::sqrt(u);
                const float wa = 1.0f - root;
                const float wb = root * (1.0f - v);
                const float wc = root * v;
                XMFLOAT3 position = {
                    triangle.a.x * wa + triangle.b.x * wb + triangle.c.x * wc +
                        normal.x * settings.surfaceOffset,
                    triangle.a.y * wa + triangle.b.y * wb + triangle.c.y * wc +
                        normal.y * settings.surfaceOffset,
                    triangle.a.z * wa + triangle.b.z * wb + triangle.c.z * wc +
                        normal.z * settings.surfaceOffset
                };
                const int32_t qx = static_cast<int32_t>(
                    std::floor(position.x / quantization));
                const int32_t qy = static_cast<int32_t>(
                    std::floor(position.y / quantization));
                const int32_t qz = static_cast<int32_t>(
                    std::floor(position.z / quantization));
                const QuantizedKey key{ qx, qy, qz };
                DXRProbeRecord probe;
                probe.position = position;
                probe.normal = normal;
                probe.radius = spacing;
                probe.stableId = Hash64(triangle.sourceId ^
                    HashCoords(qx, qy, qz));
                auto found = unique.find(key);
                if (found == unique.end() ||
                    probe.stableId < found->second.probe.stableId)
                    unique[key] = { probe, qx, qy, qz };
            }
        }

        std::vector<Candidate> candidates;
        candidates.reserve(unique.size());
        for (auto& pair : unique) candidates.push_back(std::move(pair.second));
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                return left.probe.stableId < right.probe.stableId;
            });
        if (candidates.size() > settings.maxProbes)
            candidates.resize(settings.maxProbes);
        probes.reserve(candidates.size());
        for (const Candidate& candidate : candidates)
            probes.push_back(candidate.probe);
        BuildSpatialHash();
        return !probes.empty();
    }

    void BuildSpatialHash() {
        cells.clear();
        cellProbeIndices.clear();
        if (probes.empty()) return;

        struct Coord {
            int32_t x, y, z;
            bool operator==(const Coord& other) const {
                return x == other.x && y == other.y && z == other.z;
            }
        };
        struct CoordHash {
            size_t operator()(const Coord& value) const {
                return static_cast<size_t>(
                    HashCoords(value.x, value.y, value.z));
            }
        };
        std::unordered_map<Coord, std::vector<uint32_t>, CoordHash> buckets;
        for (uint32_t index = 0; index < probes.size(); ++index) {
            const DirectX::XMFLOAT3& p = probes[index].position;
            const Coord coordinate{
                static_cast<int32_t>(std::floor(p.x / cellSize)),
                static_cast<int32_t>(std::floor(p.y / cellSize)),
                static_cast<int32_t>(std::floor(p.z / cellSize))
            };
            buckets[coordinate].push_back(index);
        }

        size_t tableSize = 1;
        while (tableSize < buckets.size() * 2u) tableSize <<= 1u;
        cells.resize((std::max)(tableSize, static_cast<size_t>(2)));
        for (const auto& bucket : buckets) {
            size_t slot = static_cast<size_t>(HashCoords(
                bucket.first.x, bucket.first.y, bucket.first.z)) &
                (cells.size() - 1u);
            while (cells[slot].count != 0)
                slot = (slot + 1u) & (cells.size() - 1u);
            DXRProbeGridCell& cell = cells[slot];
            cell.x = bucket.first.x;
            cell.y = bucket.first.y;
            cell.z = bucket.first.z;
            cell.offset = static_cast<uint32_t>(cellProbeIndices.size());
            cell.count = static_cast<uint32_t>(bucket.second.size());
            cellProbeIndices.insert(cellProbeIndices.end(),
                bucket.second.begin(), bucket.second.end());
        }
    }

    bool SaveCache(const std::filesystem::path& path,
                   uint64_t settingsHash) const {
        if (probes.empty()) return false;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        const CacheHeader header{
            kCacheMagic, kCacheVersion, sourceHash, settingsHash,
            static_cast<uint32_t>(probes.size()), cellSize
        };
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(probes.data()),
            static_cast<std::streamsize>(probes.size() * sizeof(probes[0])));
        return static_cast<bool>(stream);
    }

    bool LoadCache(const std::filesystem::path& path, uint64_t geometryHash,
                   uint64_t settingsHash, uint32_t maxProbes) {
        std::ifstream stream(path, std::ios::binary);
        CacheHeader header{};
        if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
            header.magic != kCacheMagic || header.version != kCacheVersion ||
            header.sourceHash != geometryHash ||
            header.settingsHash != settingsHash ||
            header.probeCount == 0 || header.probeCount > maxProbes)
            return false;
        std::vector<DXRProbeRecord> loaded(header.probeCount);
        if (!stream.read(reinterpret_cast<char*>(loaded.data()),
            static_cast<std::streamsize>(loaded.size() * sizeof(loaded[0]))))
            return false;
        probes = std::move(loaded);
        sourceHash = geometryHash;
        cellSize = header.cellSize;
        BuildSpatialHash();
        return true;
    }

    static uint64_t SettingsHash(const LevelDXRDDGISettings& settings) {
        uint64_t hash = 1469598103934665603ull;
        auto add = [&](uint64_t value) {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        add(static_cast<uint64_t>(settings.surfaceSpacing * 10000.0f));
        add(static_cast<uint64_t>(settings.surfaceOffset * 10000.0f));
        add(settings.maxProbes);
        return hash;
    }

    static uint64_t Hash64(uint64_t value) {
        value ^= value >> 30u;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27u;
        value *= 0x94d049bb133111ebull;
        return value ^ (value >> 31u);
    }

    static uint64_t HashCoords(int32_t x, int32_t y, int32_t z) {
        uint32_t hash = Mix32(static_cast<uint32_t>(x));
        hash ^= Mix32(static_cast<uint32_t>(y) + 0x9e3779b9u);
        hash ^= Mix32(static_cast<uint32_t>(z) + 0x85ebca6bu);
        return Mix32(hash);
    }

private:
    struct CacheHeader {
        uint32_t magic;
        uint32_t version;
        uint64_t sourceHash;
        uint64_t settingsHash;
        uint32_t probeCount;
        float cellSize;
    };
    static constexpr uint32_t kCacheMagic = 0x49474444u; // DDGI
    static constexpr uint32_t kCacheVersion = 2u;

    static uint32_t Mix32(uint32_t value) {
        value ^= value >> 16u;
        value *= 0x7feb352du;
        value ^= value >> 15u;
        value *= 0x846ca68bu;
        return value ^ (value >> 16u);
    }

    static float RadicalInverse(uint32_t bits) {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) |
               ((bits & 0xaaaaaaaau) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) |
               ((bits & 0xccccccccu) >> 2u);
        bits = ((bits & 0x0f0f0f0fu) << 4u) |
               ((bits & 0xf0f0f0f0u) >> 4u);
        bits = ((bits & 0x00ff00ffu) << 8u) |
               ((bits & 0xff00ff00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }
};

#endif
