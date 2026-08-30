// Covers NavigationSystem::FindRandomPoint, which backs the enemy scatter test
// mode. The property that matters is not "returns a point" but "returns a point
// the actor can actually stand on and path from" -- a random terrain sample is
// trivial to produce and routinely lands inside geometry, which is exactly what
// this exists to avoid.

#include "NavigationSystem.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main() {
    // Flat ground: keeps the test about the sampling, not about whether Recast
    // decided a particular slope was walkable.
    const auto flatGround = [](float, float) { return 0.0f; };
    constexpr float kExtent = 40.0f;

    NavigationSystem navigation;
    CHECK(!navigation.Ready());

    // An unbuilt navmesh must fail rather than hand back the origin -- the
    // scatter would silently stack every actor at (0,0,0).
    {
        std::mt19937 generator(1234);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const std::function<float()> random01 = [&]() { return unit(generator); };
        DirectX::XMFLOAT3 point{ 999.0f, 999.0f, 999.0f };
        CHECK(!navigation.FindRandomPoint(random01, point));
        CHECK(point.x == 999.0f);   // left untouched on failure
    }

    const bool built = navigation.BuildTerrain(
        flatGround, -kExtent, kExtent, -kExtent, kExtent, {});
    if (!built) {
        // Recast is a third-party build step; if it cannot produce a mesh here
        // the rest of the file is testing nothing. Say so loudly rather than
        // reporting a pass.
        std::cerr << "Recast failed to build a test navmesh; cannot verify "
                     "FindRandomPoint\n";
        return 1;
    }
    CHECK(navigation.Ready());

    // A null generator is rejected rather than crashing in the Detour callback.
    {
        DirectX::XMFLOAT3 point{};
        CHECK(!navigation.FindRandomPoint(std::function<float()>{}, point));
    }

    // Every sample must land inside the built extent. A point outside it is not
    // on the navmesh, which is the whole guarantee the scatter mode relies on.
    std::mt19937 generator(20260813);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const std::function<float()> random01 = [&]() { return unit(generator); };

    constexpr int kSamples = 64;
    std::vector<DirectX::XMFLOAT3> points;
    for (int i = 0; i < kSamples; ++i) {
        DirectX::XMFLOAT3 point{};
        CHECK(navigation.FindRandomPoint(random01, point));
        CHECK(std::abs(point.x) <= kExtent + 1.0f);
        CHECK(std::abs(point.z) <= kExtent + 1.0f);
        // Flat ground, so anything far off y=0 means the point is not on the
        // surface the actor will be drawn standing on.
        CHECK(std::abs(point.y) < 2.0f);
        points.push_back(point);
    }

    // The sampling has to actually vary. A generator wired up wrong (always
    // returning 0, say) still returns valid on-mesh points -- it just returns
    // the same one every time, which would make the scatter mode useless while
    // every check above still passed.
    int distinct = 0;
    for (size_t i = 1; i < points.size(); ++i) {
        const float dx = points[i].x - points[0].x;
        const float dz = points[i].z - points[0].z;
        if (dx * dx + dz * dz > 1.0f) ++distinct;
    }
    CHECK(distinct > kSamples / 4);

    // Reproducibility: the same seed must reproduce the same layout, which is
    // what lets an interesting scatter be replayed from the debug UI.
    const auto sampleWithSeed = [&navigation](unsigned int seed) {
        std::mt19937 seeded(seed);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        const std::function<float()> source = [&]() { return dist(seeded); };
        std::vector<DirectX::XMFLOAT3> result;
        for (int i = 0; i < 8; ++i) {
            DirectX::XMFLOAT3 point{};
            if (navigation.FindRandomPoint(source, point)) result.push_back(point);
        }
        return result;
    };

    const std::vector<DirectX::XMFLOAT3> first = sampleWithSeed(4242);
    const std::vector<DirectX::XMFLOAT3> repeat = sampleWithSeed(4242);
    const std::vector<DirectX::XMFLOAT3> different = sampleWithSeed(9999);
    CHECK(first.size() == 8);
    CHECK(first.size() == repeat.size());
    for (size_t i = 0; i < first.size() && i < repeat.size(); ++i) {
        CHECK(std::abs(first[i].x - repeat[i].x) < 0.0001f);
        CHECK(std::abs(first[i].z - repeat[i].z) < 0.0001f);
    }
    // And a different seed must not reproduce it, or "seeded" means nothing.
    bool differs = false;
    for (size_t i = 0; i < first.size() && i < different.size(); ++i)
        if (std::abs(first[i].x - different[i].x) > 0.0001f ||
            std::abs(first[i].z - different[i].z) > 0.0001f) differs = true;
    CHECK(differs);

    // ---- Accept predicate ---------------------------------------------------
    // The scatter mode uses this to keep actors out of the sea: the island
    // terrain runs on under the water and the flat seabed is inside Recast's
    // walkable slope, so the raw mesh is mostly offshore (measured: 376 of 400
    // samples at or below the waterline). Without the filter a scatter drops
    // most of the squad in the water.
    {
        // Accept only one quadrant. Any rejected candidate must not come back.
        const auto quadrantOnly = [](const DirectX::XMFLOAT3& candidate) {
            return candidate.x > 0.0f && candidate.z > 0.0f;
        };
        std::mt19937 filtered(77);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        const std::function<float()> source = [&]() { return dist(filtered); };

        int accepted = 0;
        for (int i = 0; i < 32; ++i) {
            DirectX::XMFLOAT3 point{};
            if (!navigation.FindRandomPoint(source, point, quadrantOnly)) continue;
            ++accepted;
            // The returned point must satisfy the predicate, not merely be on
            // the mesh -- returning a rejected candidate is the failure this
            // whole mechanism exists to prevent.
            CHECK(point.x > 0.0f);
            CHECK(point.z > 0.0f);
        }
        // A quarter of the mesh is a generous target; 64 attempts should hit it
        // essentially every time.
        CHECK(accepted > 24);
    }

    // A predicate nothing satisfies must fail rather than fall back to an
    // unfiltered point. The scatter leaves the actor put on false, so a silent
    // fallback would place it somewhere the caller explicitly refused.
    {
        std::mt19937 impossible(5);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        const std::function<float()> source = [&]() { return dist(impossible); };
        DirectX::XMFLOAT3 point{ 42.0f, 42.0f, 42.0f };
        const bool found = navigation.FindRandomPoint(
            source, point, [](const DirectX::XMFLOAT3&) { return false; });
        CHECK(!found);
        CHECK(point.x == 42.0f);   // untouched
    }

    // ---- Debug triangle extraction ------------------------------------------
    // Backs the editor navmesh overlay. The property that matters is that an
    // obstacle actually removes surface: the overlay exists so a level author
    // can see that a prop blocks the route, and a extractor that ignored
    // obstacles would draw an unbroken sheet over a blocked area.
    {
        std::vector<DirectX::XMFLOAT3> open;
        navigation.DebugWalkableTriangles(open);
        // Three vertices per triangle, and a 80x80 m field is not empty.
        CHECK(!open.empty());
        CHECK(open.size() % 3 == 0);

        // Every vertex must sit inside the built extent; a vertex far outside
        // it means the tile/detail indexing is wrong and the overlay would
        // smear triangles across the level.
        bool inBounds = true;
        for (const DirectX::XMFLOAT3& vertex : open) {
            if (std::abs(vertex.x) > kExtent + 2.0f ||
                std::abs(vertex.z) > kExtent + 2.0f ||
                std::abs(vertex.y) > 2.0f) inBounds = false;
        }
        CHECK(inBounds);

        // Same ground, but with a big obstacle over the middle. Fewer walkable
        // triangles must come back, and none may fall inside the blocked rect.
        NavigationSystem blocked;
        const NavigationObstacle middle{ -10.0f, -10.0f, 10.0f, 10.0f };
        CHECK(blocked.BuildTerrain(flatGround, -kExtent, kExtent,
                                   -kExtent, kExtent, { middle }));
        std::vector<DirectX::XMFLOAT3> holed;
        blocked.DebugWalkableTriangles(holed);
        CHECK(!holed.empty());
        // Not a triangle-count comparison: punching a hole in a flat sheet
        // splits it into MORE polygons around the rim (measured: 17 -> 28).
        // Surface area is the honest measure of "less walkable ground".
        const auto walkableArea = [](const std::vector<DirectX::XMFLOAT3>& tris) {
            double area = 0.0;
            for (size_t i = 0; i + 2 < tris.size(); i += 3) {
                // Projected onto XZ, which is the area an actor can stand on.
                const double ax = tris[i+1].x - tris[i].x;
                const double az = tris[i+1].z - tris[i].z;
                const double bx = tris[i+2].x - tris[i].x;
                const double bz = tris[i+2].z - tris[i].z;
                area += std::abs(ax * bz - az * bx) * 0.5;
            }
            return area;
        };
        const double openArea = walkableArea(open);
        const double holedArea = walkableArea(holed);
        CHECK(openArea > 0.0);
        // The 20x20 m obstacle must remove roughly its own footprint.
        CHECK(holedArea < openArea - 300.0);

        // Sample triangle centres rather than vertices: Recast keeps vertices
        // on the hole boundary, so a centre is the honest test of whether any
        // surface survived inside the obstacle.
        int centresInsideObstacle = 0;
        for (size_t i = 0; i + 2 < holed.size(); i += 3) {
            const float cx = (holed[i].x + holed[i+1].x + holed[i+2].x) / 3.0f;
            const float cz = (holed[i].z + holed[i+1].z + holed[i+2].z) / 3.0f;
            if (cx > middle.minX && cx < middle.maxX &&
                cz > middle.minZ && cz < middle.maxZ) ++centresInsideObstacle;
        }
        CHECK(centresInsideObstacle == 0);

        // An unbuilt system yields nothing, and clears what the caller passed
        // in -- the overlay reuses one vector across frames, so a stale mesh
        // would otherwise keep drawing after a level teardown.
        NavigationSystem unbuilt;
        std::vector<DirectX::XMFLOAT3> stale = open;
        unbuilt.DebugWalkableTriangles(stale);
        CHECK(stale.empty());
    }

    // Reset returns the system to the unbuilt state, so a level teardown cannot
    // leave a stale mesh behind for the next level's scatter.
    navigation.Reset();
    CHECK(!navigation.Ready());
    DirectX::XMFLOAT3 afterReset{};
    CHECK(!navigation.FindRandomPoint(random01, afterReset));

    if (failures == 0) std::cout << "NavigationRandomPointTests passed\n";
    return failures ? 1 : 0;
}
