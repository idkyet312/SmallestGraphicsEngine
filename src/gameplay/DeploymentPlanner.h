#ifndef DEPLOYMENT_PLANNER_H
#define DEPLOYMENT_PLANNER_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct DeploymentPlanner {
    static constexpr uint32_t MaxTerrainClipmapRings = 8;
    static constexpr float DeploymentTerrainTileSize = 8.0f;

    struct CameraFrame {
        float orbitRadius = 95.0f;
        float height = 62.0f;
        float terrainViewRadius = 254.0f;
        float farPlane = 800.0f;
    };

    // The historical camera frames a 43 m island and 34 m insertion ring.
    // Scaling the whole rig preserves that composition when either grows.
    static CameraFrame BuildCameraFrame(float islandRadius,
                                        float deploymentRadius,
                                        float oceanHalfSpan = 4096.0f) {
        constexpr float referenceRadius = 43.0f;
        const float contentRadius = (std::max)(
            referenceRadius, (std::max)(islandRadius, deploymentRadius));
        const float scale = contentRadius / referenceRadius;
        const float orbitRadius = 95.0f * scale;
        const float height = 62.0f * scale;
        // Include the complete seabed shelf and a broad apron outside the
        // selectable insertion ring. The latter prevents a large authored
        // drop-off radius from ending exactly at the generated terrain edge.
        constexpr float shoreToLandRadius = 88.0f / 43.0f;
        constexpr float oceanMargin = 40.0f;
        constexpr float deploymentMargin = 220.0f;
        const float terrainViewRadius = (std::max)(
            islandRadius * shoreToLandRadius + oceanMargin,
            deploymentRadius + deploymentMargin);
        const float farthestTerrain = std::sqrt(
            (orbitRadius + terrainViewRadius) *
                (orbitRadius + terrainViewRadius) +
            height * height);
        // The camera may sit on any bearing around a square ocean. The most
        // distant corner occurs at a 45-degree bearing, where |x| + |z| is
        // largest. Covering only the terrain footprint cut the water plane with
        // a camera-centred far-plane circle as this orbit turned.
        const float safeOceanHalfSpan = (std::max)(0.0f, oceanHalfSpan);
        const float farthestOcean = std::sqrt(
            2.0f * safeOceanHalfSpan * safeOceanHalfSpan +
            orbitRadius * orbitRadius +
            2.0f * std::sqrt(2.0f) * safeOceanHalfSpan * orbitRadius +
            height * height);
        const float requiredFarPlane =
            (std::max)(farthestTerrain, farthestOcean) + 250.0f;
        return { orbitRadius, height, terrainViewRadius,
                 (std::max)(800.0f, requiredFarPlane) };
    }

    static float TerrainClipmapHalfSpan(uint32_t ringGrid,
                                        float baseTileSize,
                                        uint32_t ringCount) {
        if (ringGrid == 0 || baseTileSize <= 0.0f || ringCount == 0)
            return 0.0f;
        return static_cast<float>(ringGrid) * 0.5f * baseTileSize *
            static_cast<float>(uint32_t{1} << (ringCount - 1));
    }

    static uint32_t TerrainRingCount(float requiredRadius,
                                     uint32_t ringGrid,
                                     float baseTileSize,
                                     uint32_t maxRings) {
        if (ringGrid == 0 || baseTileSize <= 0.0f || maxRings == 0)
            return 0;
        uint32_t rings = 1;
        while (rings < maxRings &&
               TerrainClipmapHalfSpan(ringGrid, baseTileSize, rings) <
                   requiredRadius)
            ++rings;
        return rings;
    }

    // Deployment uses a uniform grid instead of clipmap rings. Eight mesh
    // shader quads per 8 m tile gives one-metre vertex spacing everywhere,
    // including the outer shore and maximum insertion-radius apron.
    static uint32_t DeploymentTerrainGridSide(float requiredRadius) {
        const float safeRadius = (std::max)(0.0f, requiredRadius);
        uint32_t side = static_cast<uint32_t>(std::ceil(
            safeRadius * 2.0f / DeploymentTerrainTileSize));
        // A centred grid needs an even side count to put the origin on a tile
        // boundary and provide identical positive/negative coverage.
        if ((side & 1u) != 0u) ++side;
        return (std::max)(2u, side);
    }

    static float DeploymentTerrainHalfSpan(uint32_t gridSide) {
        return static_cast<float>(gridSide) *
            DeploymentTerrainTileSize * 0.5f;
    }

    static float HeadingTowardIslandCenter(
        const DirectX::XMFLOAT3& location,
        const DirectX::XMFLOAT3& islandCenter = {}) {
        return std::atan2(islandCenter.x - location.x,
                          islandCenter.z - location.z);
    }

    template <typename HeightSampler>
    static std::vector<DirectX::XMFLOAT3> BuildPerimeterZones(
        float radiusX, float radiusZ, uint32_t count,
        HeightSampler&& heightAt) {
        std::vector<DirectX::XMFLOAT3> zones;
        if (count == 0) return zones;
        zones.reserve(count);
        constexpr float twoPi = DirectX::XM_2PI;
        for (uint32_t index = 0; index < count; ++index) {
            const float angle = twoPi * static_cast<float>(index) /
                                static_cast<float>(count);
            const float x = std::sin(angle) * radiusX;
            const float z = std::cos(angle) * radiusZ;
            zones.push_back({ x, heightAt(x, z), z });
        }
        return zones;
    }
};

#endif
