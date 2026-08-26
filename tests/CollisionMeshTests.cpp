// CPU-only tests for the per-triangle collision BVH. No D3D device, no scene
// graph: CollisionMesh takes raw float arrays precisely so its build and its
// traversal can be verified against synthetic geometry without a game running.

#include "CollisionMesh.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace DirectX;

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

static bool NearlyEqual(float a, float b, float tolerance = 1e-4f) {
    return std::abs(a - b) <= tolerance;
}

static void PushTriangle(std::vector<float>& out,
                         float ax, float ay, float az,
                         float bx, float by, float bz,
                         float cx, float cy, float cz) {
    const float values[9] = { ax, ay, az, bx, by, bz, cx, cy, cz };
    out.insert(out.end(), values, values + 9);
}

// Axis-aligned box as 12 triangles. `inward` reverses every winding, which is
// what the winding-independence case needs: the query results must be identical
// either way.
static std::vector<float> MakeBox(float minimum, float maximum, bool inward) {
    const float lo = minimum, hi = maximum;
    const float corners[8][3] = {
        { lo, lo, lo }, { hi, lo, lo }, { hi, hi, lo }, { lo, hi, lo },
        { lo, lo, hi }, { hi, lo, hi }, { hi, hi, hi }, { lo, hi, hi },
    };
    const int faces[6][4] = {
        { 0, 1, 2, 3 },  // -Z
        { 5, 4, 7, 6 },  // +Z
        { 4, 0, 3, 7 },  // -X
        { 1, 5, 6, 2 },  // +X
        { 4, 5, 1, 0 },  // -Y
        { 3, 2, 6, 7 },  // +Y
    };
    std::vector<float> triangles;
    triangles.reserve(12 * 9);
    for (const auto& face : faces) {
        int i0 = face[0], i1 = face[1], i2 = face[2], i3 = face[3];
        if (inward) { std::swap(i1, i3); }
        PushTriangle(triangles,
                     corners[i0][0], corners[i0][1], corners[i0][2],
                     corners[i1][0], corners[i1][1], corners[i1][2],
                     corners[i2][0], corners[i2][1], corners[i2][2]);
        PushTriangle(triangles,
                     corners[i0][0], corners[i0][1], corners[i0][2],
                     corners[i2][0], corners[i2][1], corners[i2][2],
                     corners[i3][0], corners[i3][1], corners[i3][2]);
    }
    return triangles;
}

int main() {
    // --- Empty and degenerate input ---------------------------------------
    {
        CollisionMesh mesh;
        CollisionMeshBuildStats stats;
        CHECK(!BuildCollisionMesh({}, mesh, {}, &stats));
        CHECK(mesh.Empty());
        CHECK(mesh.TriangleCount() == 0);
        CHECK(stats.triangleCount == 0);

        // A query against an empty tree must reject rather than index node 0.
        CollisionMeshRayHit hit;
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(0, 0, 0), XMFLOAT3(1, 0, 0), 0.0f, hit));
        CHECK(!CollisionMeshOverlapSphere(mesh, XMFLOAT3(0, 0, 0), 1.0f));
    }
    {
        // Zero-area and collapsed triangles are skipped, not built over: they
        // poison SAH bins and would yield NaN normals at query time.
        std::vector<float> triangles;
        PushTriangle(triangles, 0, 0, 0, 0, 0, 0, 0, 0, 0);          // point
        PushTriangle(triangles, 0, 0, 0, 1, 0, 0, 2, 0, 0);          // collinear
        CollisionMesh mesh;
        CollisionMeshBuildStats stats;
        CHECK(!BuildCollisionMesh(triangles, mesh, {}, &stats));
        CHECK(stats.degenerateSkipped == 2);
        CHECK(mesh.Empty());
    }
    {
        // One real triangle among degenerates still builds, and the surviving
        // triangle maps back to its index in the caller's original array.
        std::vector<float> triangles;
        PushTriangle(triangles, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        PushTriangle(triangles, 0, 0, 0, 1, 0, 0, 0, 1, 0);
        CollisionMesh mesh;
        CollisionMeshBuildStats stats;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, &stats));
        CHECK(stats.triangleCount == 1);
        CHECK(stats.degenerateSkipped == 1);
        CHECK(mesh.TriangleCount() == 1);
        CHECK(mesh.sourceTriangle.size() == 1);
        CHECK(mesh.sourceTriangle[0] == 1);
    }

    // --- Single triangle: hit, miss, and normals from both faces -----------
    {
        // Unit triangle in the z = 0 plane, winding giving a +Z geometric normal.
        std::vector<float> triangles;
        PushTriangle(triangles, 0, 0, 0, 1, 0, 0, 0, 1, 0);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));
        CHECK(!mesh.Empty());

        // Straight down the +Z face, through the centroid.
        CollisionMeshRayHit hit;
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0.25f, 0.25f, 1.0f),
                                   XMFLOAT3(0.25f, 0.25f, -1.0f), 0.0f, hit));
        CHECK(NearlyEqual(hit.t, 0.5f));
        CHECK(NearlyEqual(hit.point.z, 0.0f));
        // Normal opposes the ray, so a ray travelling -Z gets a +Z normal.
        CHECK(NearlyEqual(hit.normal.z, 1.0f));
        CHECK(hit.triangle == 0);

        // Same triangle from behind. The geometric normal is unchanged, but the
        // returned normal flips: this is what makes decals lie correctly on the
        // inside face of a hangar wall.
        CollisionMeshRayHit back;
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0.25f, 0.25f, -1.0f),
                                   XMFLOAT3(0.25f, 0.25f, 1.0f), 0.0f, back));
        CHECK(NearlyEqual(back.normal.z, -1.0f));

        // Outside the triangle in the same plane: a miss, not a plane hit.
        CollisionMeshRayHit miss;
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(0.9f, 0.9f, 1.0f),
                                    XMFLOAT3(0.9f, 0.9f, -1.0f), 0.0f, miss));
        // Segment that stops short of the plane must not report a hit: the cast
        // is bounded by [start, end], not an infinite ray.
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(0.25f, 0.25f, 1.0f),
                                    XMFLOAT3(0.25f, 0.25f, 0.5f), 0.0f, miss));
        // Pointing away from the triangle entirely.
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(0.25f, 0.25f, 1.0f),
                                    XMFLOAT3(0.25f, 0.25f, 3.0f), 0.0f, miss));
    }

    // --- Nearest hit wins across many triangles ---------------------------
    {
        // Five parallel walls at z = 0..4. A ray from z = 10 must stop at the
        // furthest one it meets first, which exercises the front-to-back
        // ordering and the `nodeNear > best` early-out.
        std::vector<float> triangles;
        for (int i = 0; i < 5; ++i) {
            const float z = static_cast<float>(i);
            PushTriangle(triangles, -1, -1, z, 1, -1, z, 1, 1, z);
            PushTriangle(triangles, -1, -1, z, 1, 1, z, -1, 1, z);
        }
        CollisionMesh mesh;
        CollisionMeshBuildStats stats;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, &stats));
        CHECK(stats.triangleCount == 10);

        CollisionMeshRayHit hit;
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0, 0, 10), XMFLOAT3(0, 0, -10), 0.0f, hit));
        CHECK(NearlyEqual(hit.point.z, 4.0f));

        // From the other side the nearest is z = 0 instead.
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0, 0, -10), XMFLOAT3(0, 0, 10), 0.0f, hit));
        CHECK(NearlyEqual(hit.point.z, 0.0f));
    }

    // --- Hollow box: the interior is empty --------------------------------
    {
        std::vector<float> triangles = MakeBox(-1.0f, 1.0f, false);
        CollisionMesh mesh;
        CollisionMeshBuildStats stats;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, &stats));
        CHECK(stats.triangleCount == 12);

        // A ray wholly inside the box hits nothing: the walls are surfaces, and
        // the volume between them is empty. This is the property that lets the
        // player walk through a hangar instead of into a solid block.
        CollisionMeshRayHit hit;
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(-0.5f, 0, 0),
                                    XMFLOAT3(0.5f, 0, 0), 0.0f, hit));

        // From inside, aimed at a wall: hits it, with the normal pointing back
        // inward at the shooter.
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0, 0, 0), XMFLOAT3(3, 0, 0), 0.0f, hit));
        CHECK(NearlyEqual(hit.point.x, 1.0f));
        CHECK(NearlyEqual(hit.normal.x, -1.0f));

        // From outside: the near wall, not the far one.
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(-5, 0, 0), XMFLOAT3(5, 0, 0), 0.0f, hit));
        CHECK(NearlyEqual(hit.point.x, -1.0f));
        CHECK(NearlyEqual(hit.normal.x, -1.0f));

        // Passing beside the box entirely.
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(-5, 5, 0), XMFLOAT3(5, 5, 0), 0.0f, hit));
    }

    // --- Winding independence (R7) ----------------------------------------
    {
        // The same box with every face reversed must answer identically. A
        // scraped million-triangle asset has inconsistent winding throughout, so
        // no query result may depend on it.
        CollisionMesh outward, inward;
        CHECK(BuildCollisionMesh(MakeBox(-1.0f, 1.0f, false), outward, {}, nullptr));
        CHECK(BuildCollisionMesh(MakeBox(-1.0f, 1.0f, true), inward, {}, nullptr));

        CollisionMeshRayHit a, b;
        CHECK(CollisionMeshRaycast(outward, XMFLOAT3(-5, 0, 0), XMFLOAT3(5, 0, 0), 0.0f, a));
        CHECK(CollisionMeshRaycast(inward, XMFLOAT3(-5, 0, 0), XMFLOAT3(5, 0, 0), 0.0f, b));
        CHECK(NearlyEqual(a.t, b.t));
        CHECK(NearlyEqual(a.normal.x, b.normal.x));
        CHECK(NearlyEqual(a.normal.x, -1.0f));

        // And from inside, where a backface-culling implementation would miss.
        CHECK(CollisionMeshRaycast(outward, XMFLOAT3(0, 0, 0), XMFLOAT3(0, 5, 0), 0.0f, a));
        CHECK(CollisionMeshRaycast(inward, XMFLOAT3(0, 0, 0), XMFLOAT3(0, 5, 0), 0.0f, b));
        CHECK(NearlyEqual(a.t, b.t));
        CHECK(NearlyEqual(a.normal.y, -1.0f));
        CHECK(NearlyEqual(b.normal.y, -1.0f));
    }

    // --- Radius thickening -------------------------------------------------
    {
        std::vector<float> triangles;
        PushTriangle(triangles, 0, 0, 0, 1, 0, 0, 0, 1, 0);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        // A ray that ends just short of the plane misses at radius 0 but is
        // caught once the segment is thickened. Callers use radius exactly this
        // way -- as a "the bullet is slightly fat" fudge, not real physics.
        CollisionMeshRayHit hit;
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(0.25f, 0.25f, 1.0f),
                                    XMFLOAT3(0.25f, 0.25f, 0.02f), 0.0f, hit));
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0.25f, 0.25f, 1.0f),
                                   XMFLOAT3(0.25f, 0.25f, 0.02f), 0.2f, hit));
    }

    // --- Axis-parallel rays ------------------------------------------------
    {
        // Rays exactly parallel to two slab axes produce infinities in the
        // reciprocal direction. The slab test must survive them rather than
        // returning NaN comparisons.
        std::vector<float> triangles = MakeBox(-1.0f, 1.0f, false);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));
        CollisionMeshRayHit hit;
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0, -5, 0), XMFLOAT3(0, 5, 0), 0.0f, hit));
        CHECK(NearlyEqual(hit.point.y, -1.0f));
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0, 0, -5), XMFLOAT3(0, 0, 5), 0.0f, hit));
        CHECK(NearlyEqual(hit.point.z, -1.0f));

        // Degenerate zero-length segment: rejected, never divided through.
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(0, 0, 0), XMFLOAT3(0, 0, 0), 0.0f, hit));
    }

    // --- Sphere overlap ----------------------------------------------------
    {
        std::vector<float> triangles = MakeBox(-1.0f, 1.0f, false);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        // Straddling a wall.
        CHECK(CollisionMeshOverlapSphere(mesh, XMFLOAT3(1.0f, 0, 0), 0.25f));
        // Just outside it.
        CHECK(!CollisionMeshOverlapSphere(mesh, XMFLOAT3(1.5f, 0, 0), 0.25f));
        // Reaching it from just outside.
        CHECK(CollisionMeshOverlapSphere(mesh, XMFLOAT3(1.5f, 0, 0), 0.75f));
        // Well inside the hollow interior touches no surface.
        CHECK(!CollisionMeshOverlapSphere(mesh, XMFLOAT3(0, 0, 0), 0.5f));
        // Zero and negative radii are rejected outright.
        CHECK(!CollisionMeshOverlapSphere(mesh, XMFLOAT3(1.0f, 0, 0), 0.0f));
        CHECK(!CollisionMeshOverlapSphere(mesh, XMFLOAT3(1.0f, 0, 0), -1.0f));
    }

    // --- Build shape on a larger soup --------------------------------------
    {
        // A 40x40 grid of quads: enough triangles for the SAH to actually split,
        // so leaf size, depth bound and node/leaf accounting are exercised
        // rather than short-circuited by the single-leaf path.
        std::vector<float> triangles;
        for (int x = 0; x < 40; ++x) {
            for (int z = 0; z < 40; ++z) {
                const float fx = static_cast<float>(x), fz = static_cast<float>(z);
                PushTriangle(triangles, fx, 0, fz, fx + 1, 0, fz, fx + 1, 0, fz + 1);
                PushTriangle(triangles, fx, 0, fz, fx + 1, 0, fz + 1, fx, 0, fz + 1);
            }
        }
        CollisionMesh mesh;
        CollisionMeshBuildStats stats;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, &stats));
        CHECK(stats.triangleCount == 3200);
        CHECK(stats.degenerateSkipped == 0);
        CHECK(stats.nodeCount == mesh.nodes.size());
        CHECK(stats.leafCount > 1);
        CHECK(stats.maxDepth < 64);
        // Leaves respect the configured cap. The build only exceeds it at the
        // depth bound, which this input is nowhere near.
        CHECK(stats.maxLeafTriangles <= 8);
        // Every triangle is reachable from exactly one leaf, and no leaf
        // addresses past the end of the soup.
        std::vector<int> seen(stats.triangleCount, 0);
        for (const CollisionMesh::Node& node : mesh.nodes) {
            if (node.count == 0) continue;
            CHECK(node.leftFirst + node.count <= mesh.TriangleCount());
            for (uint32_t i = 0; i < node.count; ++i) ++seen[node.leftFirst + i];
        }
        for (int value : seen) CHECK(value == 1);

        // The permutation is a bijection onto the input indices.
        std::vector<int> sources(stats.triangleCount, 0);
        for (uint32_t source : mesh.sourceTriangle) {
            CHECK(source < stats.triangleCount);
            if (source < stats.triangleCount) ++sources[source];
        }
        for (int value : sources) CHECK(value == 1);

        // The plane is hit wherever it is sampled, at the right height.
        for (int sample = 0; sample < 8; ++sample) {
            const float p = 2.5f + static_cast<float>(sample) * 4.0f;
            CollisionMeshRayHit hit;
            CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(p, 5, p), XMFLOAT3(p, -5, p), 0.0f, hit));
            CHECK(NearlyEqual(hit.point.y, 0.0f));
            CHECK(NearlyEqual(hit.normal.y, 1.0f));
        }
        // Off the edge of the grid.
        CollisionMeshRayHit miss;
        CHECK(!CollisionMeshRaycast(mesh, XMFLOAT3(-5, 5, -5), XMFLOAT3(-5, -5, -5), 0.0f, miss));
    }

    // --- Coincident centroids do not recurse forever -----------------------
    {
        // Many identical triangles share one centroid, so the SAH has nothing to
        // separate. The median fallback must still terminate the build.
        std::vector<float> triangles;
        for (int i = 0; i < 256; ++i)
            PushTriangle(triangles, 0, 0, 0, 1, 0, 0, 0, 1, 0);
        CollisionMesh mesh;
        CollisionMeshBuildStats stats;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, &stats));
        CHECK(stats.triangleCount == 256);
        CHECK(stats.maxDepth <= 64);

        CollisionMeshRayHit hit;
        CHECK(CollisionMeshRaycast(mesh, XMFLOAT3(0.25f, 0.25f, 1.0f),
                                   XMFLOAT3(0.25f, 0.25f, -1.0f), 0.0f, hit));
        CHECK(NearlyEqual(hit.t, 0.5f));
    }

    // --- Instance transform ------------------------------------------------
    {
        std::vector<float> triangles = MakeBox(-1.0f, 1.0f, false);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        // Identity: world queries match local ones exactly.
        {
            CollisionMeshInstance instance;
            InitializeCollisionMeshInstance(instance, mesh, XMMatrixIdentity());
            CHECK(NearlyEqual(instance.uniformScale, 1.0f));
            CHECK(NearlyEqual(instance.worldBoundsMin.x, -1.0f));
            CHECK(NearlyEqual(instance.worldBoundsMax.x, 1.0f));

            CollisionMeshRayHit hit;
            CHECK(CollisionMeshInstanceRaycast(instance, XMFLOAT3(-5, 0, 0),
                                               XMFLOAT3(5, 0, 0), 0.0f, hit));
            CHECK(NearlyEqual(hit.point.x, -1.0f));
            CHECK(NearlyEqual(hit.normal.x, -1.0f));
        }

        // Translated and scaled: the world bounds move with it, and the hit
        // comes back in world space rather than mesh space.
        {
            CollisionMeshInstance instance;
            InitializeCollisionMeshInstance(instance, mesh,
                XMMatrixScaling(2, 2, 2) * XMMatrixTranslation(100, 0, 0));
            CHECK(NearlyEqual(instance.uniformScale, 2.0f));
            CHECK(NearlyEqual(instance.worldBoundsMin.x, 98.0f));
            CHECK(NearlyEqual(instance.worldBoundsMax.x, 102.0f));

            CollisionMeshRayHit hit;
            CHECK(CollisionMeshInstanceRaycast(instance, XMFLOAT3(90, 0, 0),
                                               XMFLOAT3(110, 0, 0), 0.0f, hit));
            CHECK(NearlyEqual(hit.point.x, 98.0f, 1e-3f));
            CHECK(NearlyEqual(hit.normal.x, -1.0f));

            // Broadphase rejects a segment nowhere near the instance without
            // touching the tree.
            CHECK(!CollisionMeshInstanceRaycast(instance, XMFLOAT3(-90, 0, 0),
                                                XMFLOAT3(-70, 0, 0), 0.0f, hit));
        }

        // Rotated 45 degrees about Y: the world AABB grows, which is why all
        // eight corners are transformed rather than the two extreme points.
        {
            CollisionMeshInstance instance;
            InitializeCollisionMeshInstance(instance, mesh,
                XMMatrixRotationY(XM_PIDIV4));
            CHECK(instance.worldBoundsMax.x > 1.3f);
            CHECK(NearlyEqual(instance.worldBoundsMax.x, std::sqrt(2.0f), 1e-3f));

            CollisionMeshRayHit hit;
            CHECK(CollisionMeshInstanceRaycast(instance, XMFLOAT3(-5, 0, 0),
                                               XMFLOAT3(5, 0, 0), 0.0f, hit));
            // Ray along +X meets a face rotated 45 degrees, so the normal is
            // diagonal but still opposes the ray.
            CHECK(hit.normal.x < 0.0f);
            CHECK(NearlyEqual(std::sqrt(hit.normal.x * hit.normal.x +
                                        hit.normal.y * hit.normal.y +
                                        hit.normal.z * hit.normal.z), 1.0f, 1e-3f));
        }

        // An instance with no mesh must reject rather than dereference null.
        {
            CollisionMeshInstance instance;
            CollisionMeshRayHit hit;
            CHECK(!CollisionMeshInstanceRaycast(instance, XMFLOAT3(-5, 0, 0),
                                                XMFLOAT3(5, 0, 0), 0.0f, hit));
        }
    }

    // --- Capsule resolve ---------------------------------------------------
    {
        // A single large floor quad at y = 0.
        std::vector<float> triangles;
        PushTriangle(triangles, -10, 0, -10, 10, 0, -10, 10, 0, 10);
        PushTriangle(triangles, -10, 0, -10, 10, 0, 10, -10, 0, 10);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        // Standing on the floor: reports it as floor, and does not shove the
        // player sideways.
        CollisionMeshPushout onFloor = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(0, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(onFloor.touched);
        CHECK(onFloor.hasFloor);
        CHECK(NearlyEqual(onFloor.floorY, 0.0f));
        CHECK(NearlyEqual(onFloor.displacement.x, 0.0f));
        CHECK(NearlyEqual(onFloor.displacement.z, 0.0f));

        // Well above the floor: no contact at all.
        CollisionMeshPushout above = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(0, 5, 0), 0.35f, 1.8f, 0.45f);
        CHECK(!above.touched);
        CHECK(!above.hasFloor);
    }
    {
        // A vertical wall in the x = 0 plane. A capsule overlapping it must be
        // pushed out horizontally, never downward.
        std::vector<float> triangles;
        PushTriangle(triangles, 0, 0, -5, 0, 4, -5, 0, 4, 5);
        PushTriangle(triangles, 0, 0, -5, 0, 4, 5, 0, 0, 5);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        // Standing just inside the wall from -x.
        CollisionMeshPushout hit = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(-0.2f, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(hit.touched);
        CHECK(hit.displacement.x < 0.0f);          // pushed back out to -x
        CHECK(hit.displacement.y >= 0.0f);         // never downward
        // And far enough that the capsule clears the wall.
        CHECK(-0.2f + hit.displacement.x <= -0.35f + 1e-3f);
        // A wall is not floor.
        CHECK(!hit.hasFloor);

        // From the other side it pushes the other way, so a wall blocks both
        // faces regardless of winding.
        CollisionMeshPushout other = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(0.2f, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(other.touched);
        CHECK(other.displacement.x > 0.0f);

        // Standing clear of it: untouched.
        CollisionMeshPushout clear = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(-2.0f, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(!clear.touched);
    }
    {
        // The property that makes walking into a hangar work: a doorway gap in
        // a wall must let the capsule through, while the wall beside it blocks.
        std::vector<float> triangles;
        // Wall spanning z, with a gap between z = -0.6 and z = 0.6.
        PushTriangle(triangles, 0, 0, -5, 0, 4, -5, 0, 4, -0.6f);
        PushTriangle(triangles, 0, 0, -5, 0, 4, -0.6f, 0, 0, -0.6f);
        PushTriangle(triangles, 0, 0, 0.6f, 0, 4, 0.6f, 0, 4, 5);
        PushTriangle(triangles, 0, 0, 0.6f, 0, 4, 5, 0, 0, 5);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        // Dead centre of the doorway: a 0.35 radius capsule fits in the 1.2 gap.
        CollisionMeshPushout through = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(0, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(!through.touched);

        // Against the wall beside the doorway: blocked.
        CollisionMeshPushout blocked = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(0, 0, 3.0f), 0.35f, 1.8f, 0.45f);
        CHECK(blocked.touched);
        CHECK(std::abs(blocked.displacement.x) > 0.0f);
    }
    {
        // A low sill inside the step height is floor to walk over, not a wall.
        std::vector<float> triangles;
        PushTriangle(triangles, -5, 0.2f, -5, 5, 0.2f, -5, 5, 0.2f, 5);
        PushTriangle(triangles, -5, 0.2f, -5, 5, 0.2f, 5, -5, 0.2f, 5);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        CollisionMeshPushout step = CollisionMeshResolveCapsule(
            mesh, XMFLOAT3(0, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(step.touched);
        CHECK(step.hasFloor);
        CHECK(NearlyEqual(step.floorY, 0.2f));
        // Stepped onto, not shoved sideways.
        CHECK(NearlyEqual(step.displacement.x, 0.0f));
        CHECK(NearlyEqual(step.displacement.z, 0.0f));
    }
    {
        // Instance wrapper: the same wall, translated, resolves in world space.
        std::vector<float> triangles;
        PushTriangle(triangles, 0, 0, -5, 0, 4, -5, 0, 4, 5);
        PushTriangle(triangles, 0, 0, -5, 0, 4, 5, 0, 0, 5);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));

        CollisionMeshInstance instance;
        InitializeCollisionMeshInstance(instance, mesh,
            XMMatrixTranslation(50, 0, 0));

        CollisionMeshPushout hit = CollisionMeshInstanceResolveCapsule(
            instance, XMFLOAT3(49.8f, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(hit.touched);
        CHECK(hit.displacement.x < 0.0f);

        // Far away: rejected by the broadphase without touching the tree.
        CollisionMeshPushout far = CollisionMeshInstanceResolveCapsule(
            instance, XMFLOAT3(0, 0, 0), 0.35f, 1.8f, 0.45f);
        CHECK(!far.touched);

        // An instance with no mesh must not dereference null.
        CollisionMeshInstance empty;
        CHECK(!CollisionMeshInstanceResolveCapsule(
            empty, XMFLOAT3(0, 0, 0), 0.35f, 1.8f, 0.45f).touched);
    }

    // --- Memory accounting -------------------------------------------------
    {
        std::vector<float> triangles = MakeBox(-1.0f, 1.0f, false);
        CollisionMesh mesh;
        CHECK(BuildCollisionMesh(triangles, mesh, {}, nullptr));
        const size_t expected = mesh.triangles.size() * sizeof(float) +
                                mesh.nodes.size() * sizeof(CollisionMesh::Node) +
                                mesh.sourceTriangle.size() * sizeof(uint32_t);
        CHECK(mesh.MemoryBytes() == expected);
        CHECK(mesh.TriangleCount() == 12);
    }

    if (failures == 0) {
        std::cout << "CollisionMeshTests passed\n";
        return 0;
    }
    std::cerr << "CollisionMeshTests: " << failures << " failure(s)\n";
    return 1;
}
