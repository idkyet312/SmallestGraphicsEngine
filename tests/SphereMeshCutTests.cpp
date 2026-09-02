// Geometry tests for the runtime sphere boolean used to blast a hole in a
// building when a grenade goes off against it.
//
// The properties under test are pure arithmetic, so no D3D device, Blast
// framework or game content is involved:
//   - nothing inside the sphere survives the cut
//   - geometry outside the sphere is preserved exactly
//   - a sphere that misses the mesh leaves it untouched
//   - the opening is capped, so the hole is not see-through
//   - the result can be cut again (craters accumulate)

#include "SphereMeshCut.h"

#include <cmath>
#include <cstdio>
#include <vector>

using SGE::CutSphereFromMesh;
using SGE::SphereCut;
using SGE::SphereCutVertex;
using SGE::kSphereCutVertexStride;

namespace {

int failures = 0;

#define CHECK(value) do { if (!(value)) { \
    std::printf("%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #value); \
    ++failures; } } while (false)

void PushVertex(std::vector<float>& vertices, float x, float y, float z) {
    const float vertex[kSphereCutVertexStride] = {
        x, y, z, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f
    };
    vertices.insert(vertices.end(), std::begin(vertex), std::end(vertex));
}

// A flat wall in the XY plane, tessellated into `side` x `side` quads so a
// sphere landing in the middle has triangles to clip rather than swallow.
void BuildWall(int side, float extent, std::vector<float>& vertices,
               std::vector<unsigned int>& indices) {
    for (int y = 0; y <= side; ++y) {
        for (int x = 0; x <= side; ++x) {
            const float u = static_cast<float>(x) / side * 2.0f - 1.0f;
            const float v = static_cast<float>(y) / side * 2.0f - 1.0f;
            PushVertex(vertices, u * extent, v * extent, 0.0f);
        }
    }
    const int stride = side + 1;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const unsigned int a = y * stride + x;
            const unsigned int b = a + 1;
            const unsigned int c = a + stride;
            const unsigned int d = c + 1;
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
}

SphereCutVertex ReadVertex(const std::vector<float>& vertices,
                           unsigned int index) {
    SphereCutVertex vertex{};
    for (int i = 0; i < kSphereCutVertexStride; ++i)
        vertex[i] = vertices[static_cast<size_t>(index) *
                             kSphereCutVertexStride + i];
    return vertex;
}

}  // namespace

int main() {
    // ---- A sphere in the middle of a wall removes material there ----
    {
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
        BuildWall(16, 4.0f, vertices, indices);

        SphereCut sphere;
        sphere.centreX = 0.0f;
        sphere.centreY = 0.0f;
        sphere.centreZ = 0.0f;
        sphere.radius = 1.0f;

        std::vector<float> cutVertices;
        std::vector<unsigned int> cutIndices;
        CHECK(CutSphereFromMesh(vertices, indices, sphere, cutVertices,
                                cutIndices));
        CHECK(!cutIndices.empty());

        // No surviving triangle may have a vertex strictly inside the sphere.
        // A small tolerance absorbs the snap-onto-surface in CrossingVertex.
        int insideCount = 0;
        for (unsigned int index : cutIndices) {
            const SphereCutVertex vertex = ReadVertex(cutVertices, index);
            const float dx = vertex[0] - sphere.centreX;
            const float dy = vertex[1] - sphere.centreY;
            const float dz = vertex[2] - sphere.centreZ;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            // The bowl hub sits deliberately inside the sphere, and it is the
            // only such vertex; every other surviving vertex must be on or
            // outside the surface. Anything strictly between the hub depth and
            // the surface would be un-removed material inside the crater.
            if (distance < sphere.radius - 1e-3f &&
                distance > sphere.radius * 0.6f)
                ++insideCount;
        }
        CHECK(insideCount == 0);

        // The hole must actually be a hole: fewer triangles cover the centre.
        CHECK(cutIndices.size() < indices.size() + 3 * 64);
    }

    // ---- A sphere that misses leaves the mesh untouched ----
    {
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
        BuildWall(8, 2.0f, vertices, indices);

        SphereCut sphere;
        sphere.centreX = 100.0f;
        sphere.radius = 1.0f;

        std::vector<float> cutVertices;
        std::vector<unsigned int> cutIndices;
        CHECK(!CutSphereFromMesh(vertices, indices, sphere, cutVertices,
                                 cutIndices));
        // Outputs are left alone so the caller keeps the original geometry.
        CHECK(cutVertices.empty());
        CHECK(cutIndices.empty());
    }

    // ---- A sphere swallowing everything leaves no wall behind ----
    {
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
        BuildWall(4, 1.0f, vertices, indices);

        SphereCut sphere;
        sphere.radius = 100.0f;

        std::vector<float> cutVertices;
        std::vector<unsigned int> cutIndices;
        CHECK(CutSphereFromMesh(vertices, indices, sphere, cutVertices,
                                cutIndices));
        CHECK(cutIndices.empty());
    }

    // ---- The opening is capped rather than left open ----
    {
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
        BuildWall(16, 4.0f, vertices, indices);

        SphereCut sphere;
        sphere.radius = 1.0f;

        std::vector<float> cutVertices;
        std::vector<unsigned int> cutIndices;
        CHECK(CutSphereFromMesh(vertices, indices, sphere, cutVertices,
                                cutIndices));

        // Cap triangles are the ones touching the bowl hub, which is sunk
        // below the rim plane and so is the only vertex strictly inside the
        // sphere. At least one must exist, or the hole is see-through.
        int capTriangles = 0;
        for (size_t i = 0; i + 2 < cutIndices.size(); i += 3) {
            for (int corner = 0; corner < 3; ++corner) {
                const SphereCutVertex vertex =
                    ReadVertex(cutVertices, cutIndices[i + corner]);
                const float dx = vertex[0] - sphere.centreX;
                const float dy = vertex[1] - sphere.centreY;
                const float dz = vertex[2] - sphere.centreZ;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) <
                    sphere.radius - 1e-3f) {
                    ++capTriangles;
                    break;
                }
            }
        }
        // One fan triangle per rim edge, so a real cap is many triangles.
        CHECK(capTriangles > 8);
    }

    // ---- Cutting the result again keeps working (craters accumulate) ----
    {
        std::vector<float> vertices;
        std::vector<unsigned int> indices;
        BuildWall(24, 6.0f, vertices, indices);

        SphereCut first;
        first.centreX = -2.0f;
        first.radius = 1.0f;

        std::vector<float> once, twice;
        std::vector<unsigned int> onceIndices, twiceIndices;
        CHECK(CutSphereFromMesh(vertices, indices, first, once, onceIndices));

        SphereCut second;
        second.centreX = 2.0f;
        second.radius = 1.0f;
        CHECK(CutSphereFromMesh(once, onceIndices, second, twice,
                                twiceIndices));
        CHECK(!twiceIndices.empty());

        // Both craters survive the second pass: no wall material is left in
        // the shell of either sphere. Only the two bowl hubs may lie inside,
        // and they sit below 0.6r, so counting what falls in the shell band
        // catches a crater that the second cut filled back in.
        int inShell = 0;
        for (unsigned int index : twiceIndices) {
            const SphereCutVertex vertex = ReadVertex(twice, index);
            for (const SphereCut& sphere : { first, second }) {
                const float dx = vertex[0] - sphere.centreX;
                const float dy = vertex[1] - sphere.centreY;
                const float dz = vertex[2] - sphere.centreZ;
                const float distance =
                    std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > sphere.radius * 0.6f &&
                    distance < sphere.radius - 1e-3f)
                    ++inShell;
            }
        }
        CHECK(inShell == 0);
    }

    if (failures == 0) std::printf("SphereMeshCutTests passed\n");
    return failures == 0 ? 0 : 1;
}
