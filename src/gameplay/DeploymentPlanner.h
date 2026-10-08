#ifndef DEPLOYMENT_PLANNER_H
#define DEPLOYMENT_PLANNER_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct DeploymentPlanner {
    // Gameplay ring half-span is 10 m * 2^(R-1) (G = 20, 1 m base): 8 rings
    // reach 1280 m, enough for a 12x island (1096 m); 9 reach 2560 m for the
    // 24x maximum (2152 m). Too few rings drops the seabed past the last one.
    static constexpr uint32_t MaxTerrainClipmapRings = 9;
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
    // shader quads per tile, so vertex spacing is tileSize / 8 everywhere,
    // including the outer shore and maximum insertion-radius apron.
    static uint32_t DeploymentTerrainGridSide(
        float requiredRadius, float tileSize = DeploymentTerrainTileSize) {
        const float safeRadius = (std::max)(0.0f, requiredRadius);
        uint32_t side = static_cast<uint32_t>(std::ceil(
            safeRadius * 2.0f / tileSize));
        // A centred grid needs an even side count to put the origin on a tile
        // boundary and provide identical positive/negative coverage.
        if ((side & 1u) != 0u) ++side;
        return (std::max)(2u, side);
    }

    static float DeploymentTerrainHalfSpan(
        uint32_t gridSide, float tileSize = DeploymentTerrainTileSize) {
        return static_cast<float>(gridSide) * tileSize * 0.5f;
    }

    // Auto detail keeps the overview grid at or under this many tiles a side.
    // Measured on the planning screen (VB Terrain): BigIslandv34, 96 tiles at
    // 1 m, 1.8 ms. The 12x island (12xv1): 274 tiles at 1 m 10.05 ms, 138 at
    // 2 m 4.54 ms, 70 at 4 m 1.95 ms. 4 m visibly aliases the fine relief on
    // the shallow shelf (blotchy, wider dark band); 2 m matches 1 m closely.
    // The budget admits 2 m there and leaves the smaller islands at 1 m.
    static constexpr uint32_t DeploymentTerrainAutoMaxSide = 160;

    // Overview tile size for an authored vertex spacing in metres (1, 2, 4 or
    // 8). Anything else is Auto: the finest spacing whose grid fits the budget.
    static float DeploymentTerrainTileSizeFor(float requiredRadius,
                                              int spacingMetres) {
        if (spacingMetres == 1 || spacingMetres == 2 || spacingMetres == 4 ||
            spacingMetres == 8)
            return DeploymentTerrainTileSize * static_cast<float>(spacingMetres);
        for (int spacing = 1; spacing < 8; spacing *= 2) {
            const float tileSize =
                DeploymentTerrainTileSize * static_cast<float>(spacing);
            if (DeploymentTerrainGridSide(requiredRadius, tileSize) <=
                DeploymentTerrainAutoMaxSide)
                return tileSize;
        }
        return DeploymentTerrainTileSize * 8.0f;
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
