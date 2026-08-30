#include "NavigationSystem.h"

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

using namespace DirectX;

NavigationSystem::NavigationSystem() = default;
NavigationSystem::~NavigationSystem() { Reset(); }

void NavigationSystem::Reset() {
    if (query_) { dtFreeNavMeshQuery(query_); query_ = nullptr; }
    if (navMesh_) { dtFreeNavMesh(navMesh_); navMesh_ = nullptr; }
}

void NavigationSystem::DebugWalkableTriangles(
        std::vector<DirectX::XMFLOAT3>& outTriangles) const {
    outTriangles.clear();
    if (!navMesh_) return;

    // Through a const pointer: dtNavMesh also declares a private non-const
    // getTile overload, and a non-const navMesh_ selects that one.
    const dtNavMesh* navMesh = navMesh_;

    // BuildTerrain creates a single tile, but iterate anyway so this keeps
    // working if that ever becomes a tiled build.
    for (int tileIndex = 0; tileIndex < navMesh->getMaxTiles(); ++tileIndex) {
        const dtMeshTile* tile = navMesh->getTile(tileIndex);
        // An unused slot in the tile array has no header, which is how Detour
        // marks it free -- not an error.
        if (!tile || !tile->header) continue;

        for (int polyIndex = 0; polyIndex < tile->header->polyCount; ++polyIndex) {
            const dtPoly& poly = tile->polys[polyIndex];
            // Off-mesh connections are a two-vertex link, not a surface.
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            const dtPolyDetail& detail = tile->detailMeshes[polyIndex];

            for (int triangle = 0; triangle < detail.triCount; ++triangle) {
                // Stride 4: three vertex indices plus an edge-flags byte.
                const unsigned char* indices =
                    &tile->detailTris[(detail.triBase + triangle) * 4];
                for (int corner = 0; corner < 3; ++corner) {
                    // Indices below the polygon's own vertex count address the
                    // tile's shared vertices; the rest are vertices unique to
                    // the detail mesh, stored separately.
                    const float* position =
                        indices[corner] < poly.vertCount
                            ? &tile->verts[poly.verts[indices[corner]] * 3]
                            : &tile->detailVerts[
                                  (detail.vertBase +
                                   (indices[corner] - poly.vertCount)) * 3];
                    outTriangles.push_back(
                        DirectX::XMFLOAT3(position[0], position[1], position[2]));
                }
            }
        }
    }
}

bool NavigationSystem::BuildTerrain(
    const std::function<float(float, float)>& heightAt,
    float minX, float maxX, float minZ, float maxZ,
    const std::vector<NavigationObstacle>& obstacles) {
    Reset();
    if (!heightAt || minX >= maxX || minZ >= maxZ) return false;

    constexpr float sampleSpacing = 0.65f;
    const int columns = static_cast<int>(std::ceil((maxX - minX) / sampleSpacing)) + 1;
    const int rows = static_cast<int>(std::ceil((maxZ - minZ) / sampleSpacing)) + 1;
    std::vector<float> vertices(static_cast<size_t>(columns) * rows * 3);
    for (int z = 0; z < rows; ++z) {
        for (int x = 0; x < columns; ++x) {
            const float wx = (std::min)(maxX, minX + x * sampleSpacing);
            const float wz = (std::min)(maxZ, minZ + z * sampleSpacing);
            const size_t offset = (static_cast<size_t>(z) * columns + x) * 3;
            vertices[offset + 0] = wx;
            vertices[offset + 1] = heightAt(wx, wz);
            vertices[offset + 2] = wz;
        }
    }

    std::vector<int> triangles;
    triangles.reserve(static_cast<size_t>(columns - 1) * (rows - 1) * 6);
    for (int z = 0; z + 1 < rows; ++z) {
        for (int x = 0; x + 1 < columns; ++x) {
            const int a = z * columns + x;
            const int b = a + 1;
            const int c = a + columns;
            const int d = c + 1;
            // Recast is Y-up. Winding below produces upward-facing normals.
            triangles.insert(triangles.end(), { a, c, b, b, c, d });
        }
    }

    rcConfig cfg{};
    cfg.cs = 0.30f;
    cfg.ch = 0.15f;
    cfg.walkableSlopeAngle = 43.0f;
    cfg.walkableHeight = static_cast<int>(std::ceil(1.75f / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(0.55f / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(0.42f / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = rcSqr(8);
    cfg.mergeRegionArea = rcSqr(20);
    cfg.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch;
    rcCalcBounds(vertices.data(), static_cast<int>(vertices.size() / 3), cfg.bmin, cfg.bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    rcContext context(false);
    std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)>
        solid(rcAllocHeightfield(), rcFreeHeightField);
    if (!solid || !rcCreateHeightfield(&context, *solid, cfg.width, cfg.height,
                                        cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return false;

    const int triangleCount = static_cast<int>(triangles.size() / 3);
    std::vector<unsigned char> areas(static_cast<size_t>(triangleCount), RC_NULL_AREA);
    rcMarkWalkableTriangles(&context, cfg.walkableSlopeAngle, vertices.data(),
                            static_cast<int>(vertices.size() / 3), triangles.data(),
                            triangleCount, areas.data());
    for (int i = 0; i < triangleCount; ++i) {
        const int* tri = triangles.data() + i * 3;
        const float centerX = (vertices[tri[0]*3] + vertices[tri[1]*3] +
                               vertices[tri[2]*3]) / 3.0f;
        const float centerZ = (vertices[tri[0]*3+2] + vertices[tri[1]*3+2] +
                               vertices[tri[2]*3+2]) / 3.0f;
        for (const NavigationObstacle& obstacle : obstacles) {
            if (centerX >= obstacle.minX && centerX <= obstacle.maxX &&
                centerZ >= obstacle.minZ && centerZ <= obstacle.maxZ) {
                areas[i] = RC_NULL_AREA;
                break;
            }
        }
    }
    if (!rcRasterizeTriangles(&context, vertices.data(),
                              static_cast<int>(vertices.size() / 3), triangles.data(),
                              areas.data(), triangleCount, *solid, cfg.walkableClimb)) return false;
    rcFilterLowHangingWalkableObstacles(&context, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&context, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&context, cfg.walkableHeight, *solid);

    std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)>
        compact(rcAllocCompactHeightfield(), rcFreeCompactHeightfield);
    if (!compact || !rcBuildCompactHeightfield(&context, cfg.walkableHeight,
            cfg.walkableClimb, *solid, *compact)) return false;
    solid.reset();
    if (!rcErodeWalkableArea(&context, cfg.walkableRadius, *compact) ||
        !rcBuildDistanceField(&context, *compact) ||
        !rcBuildRegions(&context, *compact, 0, cfg.minRegionArea, cfg.mergeRegionArea)) return false;

    std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)>
        contours(rcAllocContourSet(), rcFreeContourSet);
    if (!contours || !rcBuildContours(&context, *compact, cfg.maxSimplificationError,
                                       cfg.maxEdgeLen, *contours)) return false;
    std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)>
        mesh(rcAllocPolyMesh(), rcFreePolyMesh);
    if (!mesh || !rcBuildPolyMesh(&context, *contours, cfg.maxVertsPerPoly, *mesh)) return false;
    std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)>
        detail(rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);
    if (!detail || !rcBuildPolyMeshDetail(&context, *mesh, *compact,
            cfg.detailSampleDist, cfg.detailSampleMaxError, *detail)) return false;
    for (int i = 0; i < mesh->npolys; ++i)
        if (mesh->areas[i] == RC_WALKABLE_AREA) mesh->flags[i] = 1;

    dtNavMeshCreateParams params{};
    params.verts = mesh->verts;
    params.vertCount = mesh->nverts;
    params.polys = mesh->polys;
    params.polyAreas = mesh->areas;
    params.polyFlags = mesh->flags;
    params.polyCount = mesh->npolys;
    params.nvp = mesh->nvp;
    params.detailMeshes = detail->meshes;
    params.detailVerts = detail->verts;
    params.detailVertsCount = detail->nverts;
    params.detailTris = detail->tris;
    params.detailTriCount = detail->ntris;
    params.walkableHeight = 1.75f;
    params.walkableRadius = 0.42f;
    params.walkableClimb = 0.55f;
    rcVcopy(params.bmin, mesh->bmin);
    rcVcopy(params.bmax, mesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) return false;
    navMesh_ = dtAllocNavMesh();
    if (!navMesh_ || dtStatusFailed(navMesh_->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
        dtFree(navData);
        Reset();
        return false;
    }
    query_ = dtAllocNavMeshQuery();
    if (!query_ || dtStatusFailed(query_->init(navMesh_, 2048))) {
        Reset();
        return false;
    }
    std::cout << "Navigation ready: " << mesh->npolys << " polygons, "
              << obstacles.size() << " blocked regions\n";
    return true;
}

namespace {
// Detour's findRandomPoint takes a bare `float(*)()`, which cannot carry the
// caller's generator. This thread-local holds it for the duration of the call
// so a seeded generator can still drive the sampling -- without it the scatter
// could only ever use a global rand() and would not be reproducible.
thread_local const std::function<float()>* t_randomSource = nullptr;

float DetourRandom() {
    return t_randomSource && *t_randomSource ? (*t_randomSource)() : 0.0f;
}
}  // namespace

bool NavigationSystem::FindRandomPoint(
        const std::function<float()>& random01, XMFLOAT3& point,
        const std::function<bool(const XMFLOAT3&)>& accept) const {
    if (!Ready() || !random01) return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(1);
    filter.setExcludeFlags(0);

    // Rejection sampling. Detour has no way to express "walkable, but also on
    // land", so the filter is applied here, and the cap keeps a caller whose
    // predicate matches almost nothing from spinning.
    //
    // 64 is sized against a measurement, not a guess: sampling the island's
    // navmesh 400 times put 376 points at or below the waterline, and only ~13
    // above 0.55 m. The mesh follows the terrain out under the sea and the flat
    // seabed sits well inside Recast's 43-degree walkable slope, so the great
    // majority of it is offshore. At that ~6% acceptance rate 64 attempts fail
    // by chance roughly once in 10^5 calls -- rare enough that a failed scatter
    // leaves the actor where it was, which is a visible no-op rather than a
    // wrong placement.
    constexpr int kMaxAttempts = 64;
    const int attempts = accept ? kMaxAttempts : 1;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        dtPolyRef ref = 0;
        float found[3]{};
        t_randomSource = &random01;
        const dtStatus status =
            query_->findRandomPoint(&filter, DetourRandom, &ref, found);
        t_randomSource = nullptr;
        if (dtStatusFailed(status) || !ref) return false;

        const XMFLOAT3 candidate{ found[0], found[1], found[2] };
        if (accept && !accept(candidate)) continue;
        point = candidate;
        return true;
    }
    return false;
}

bool NavigationSystem::FindPath(const XMFLOAT3& start,
                                const XMFLOAT3& destination,
                                std::vector<XMFLOAT3>& points) const {
    points.clear();
    if (!Ready()) return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(1);
    filter.setExcludeFlags(0);
    const float extents[3] = { 2.0f, 4.0f, 2.0f };
    const float from[3] = { start.x, start.y, start.z };
    const float to[3] = { destination.x, destination.y, destination.z };
    float nearestFrom[3]{}, nearestTo[3]{};
    dtPolyRef fromRef = 0, toRef = 0;
    if (dtStatusFailed(query_->findNearestPoly(from, extents, &filter,
            &fromRef, nearestFrom)) || !fromRef ||
        dtStatusFailed(query_->findNearestPoly(to, extents, &filter,
            &toRef, nearestTo)) || !toRef) return false;

    dtPolyRef corridor[256]{};
    int corridorCount = 0;
    if (dtStatusFailed(query_->findPath(fromRef, toRef, nearestFrom, nearestTo,
            &filter, corridor, &corridorCount, 256)) || corridorCount == 0) return false;
    if (corridor[corridorCount - 1] != toRef) {
        float partialEnd[3]{};
        if (dtStatusSucceed(query_->closestPointOnPoly(
                corridor[corridorCount - 1], nearestTo, partialEnd, nullptr))) {
            nearestTo[0] = partialEnd[0];
            nearestTo[1] = partialEnd[1];
            nearestTo[2] = partialEnd[2];
        }
    }

    float straight[256 * 3]{};
    unsigned char straightFlags[256]{};
    dtPolyRef straightRefs[256]{};
    int straightCount = 0;
    if (dtStatusFailed(query_->findStraightPath(nearestFrom, nearestTo, corridor,
            corridorCount, straight, straightFlags, straightRefs, &straightCount, 256)) ||
        straightCount == 0) return false;
    points.reserve(straightCount);
    for (int i = 0; i < straightCount; ++i)
        points.emplace_back(straight[i*3], straight[i*3+1], straight[i*3+2]);
    return true;
}
