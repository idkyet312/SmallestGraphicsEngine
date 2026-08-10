#ifndef DEPLOYMENT_PLANNER_H
#define DEPLOYMENT_PLANNER_H

#include <DirectXMath.h>
#include <cmath>
#include <cstdint>
#include <vector>

struct DeploymentPlanner {
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
