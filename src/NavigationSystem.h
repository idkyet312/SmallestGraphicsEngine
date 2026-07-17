#pragma once

#include <DirectXMath.h>
#include <functional>
#include <memory>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

struct NavigationObstacle {
    float minX = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxZ = 0.0f;
};

// Recast builds one static terrain navmesh. Detour performs runtime nearest-poly
// projection and straight-path queries for enemy steering.
class NavigationSystem {
public:
    NavigationSystem();
    ~NavigationSystem();
    NavigationSystem(const NavigationSystem&) = delete;
    NavigationSystem& operator=(const NavigationSystem&) = delete;

    bool BuildTerrain(const std::function<float(float, float)>& heightAt,
                      float minX, float maxX, float minZ, float maxZ,
                      const std::vector<NavigationObstacle>& obstacles);
    bool FindPath(const DirectX::XMFLOAT3& start,
                  const DirectX::XMFLOAT3& destination,
                  std::vector<DirectX::XMFLOAT3>& points) const;
    bool Ready() const { return navMesh_ != nullptr && query_ != nullptr; }
    void Reset();

private:
    dtNavMesh* navMesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
};

extern NavigationSystem g_navigation;
