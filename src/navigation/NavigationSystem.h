#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <filesystem>
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

    // `solidTriangles` is world-space prop geometry (9 floats per triangle)
    // rasterized alongside the terrain: its walls block, its floors, decks and
    // ramps become walkable. Winding is ignored -- scraped assets are
    // inconsistent -- so a triangle is walkable when it is flat enough either
    // way up.
    //
    // `bakePath`, when set, is a pre-built mesh on disk: it is loaded instead of
    // building when its stored input hash matches this call's inputs, and is
    // (re)written after a build otherwise. A level start then pays for the
    // terrain sampling and the hash, not the ~5 s Recast build.
    bool BuildTerrain(const std::function<float(float, float)>& heightAt,
                      float minX, float maxX, float minZ, float maxZ,
                      const std::vector<NavigationObstacle>& obstacles,
                      const std::vector<float>& solidTriangles = {},
                      const std::filesystem::path& bakePath = {});
    bool FindPath(const DirectX::XMFLOAT3& start,
                  const DirectX::XMFLOAT3& destination,
                  std::vector<DirectX::XMFLOAT3>& points) const;
    // Uniformly random point on the walkable navmesh. Used by the enemy
    // scatter test mode to place actors somewhere they can actually stand and
    // path from, which a raw terrain-height sample cannot guarantee.
    //
    // Takes a caller-supplied [0,1) source rather than calling rand() itself so
    // a scatter can be reproduced from a seed.
    //
    // `accept` optionally rejects candidates the navmesh considers walkable but
    // the caller does not want -- the island terrain runs on out under the sea
    // and the flat seabed is well inside Recast's walkable slope, so without a
    // filter a scatter drops actors offshore. Rejected candidates are retried
    // up to an internal cap; returns false if none pass, rather than handing
    // back a point the caller already refused.
    bool FindRandomPoint(const std::function<float()>& random01,
                         DirectX::XMFLOAT3& point,
                         const std::function<bool(const DirectX::XMFLOAT3&)>&
                             accept = {}) const;
    bool Ready() const { return navMesh_ != nullptr && query_ != nullptr; }
    // Nearest walkable point within `horizontalReach` metres (XZ) of `point`.
    // False when nothing walkable is that close.
    bool NearestWalkable(const DirectX::XMFLOAT3& point, float horizontalReach,
                         DirectX::XMFLOAT3& nearest) const;
    // The walkable surface as world-space triangles, three vertices per triangle,
    // for debug visualisation. This is the detail mesh rather than the coarse
    // polygons, so it follows the terrain the way the navmesh actually does.
    //
    // Obstacles are subtracted before Recast rasterizes, so the holes a prop
    // punches in the walkable area are genuinely absent here rather than
    // something the caller has to filter out. Clears `outTriangles` first, and
    // leaves it empty when no navmesh is built.
    void DebugWalkableTriangles(
        std::vector<DirectX::XMFLOAT3>& outTriangles) const;
    void Reset();

private:
    // Bump when anything in the build changes that the input hash cannot see
    // (Recast calls, filters, Detour params), so stale bakes are rebuilt.
    static constexpr uint64_t kBakeVersion = 1;
    bool LoadBaked(const std::filesystem::path& path, uint64_t inputHash);
    static void SaveBaked(const std::filesystem::path& path, uint64_t inputHash,
                          const unsigned char* data, int size);
    dtNavMesh* navMesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
};

extern NavigationSystem g_navigation;
