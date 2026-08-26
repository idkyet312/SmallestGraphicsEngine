#include "CollisionMesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

using namespace DirectX;

namespace {

// Per-triangle scratch used only during the build. Centroids and bounds are
// computed once up front and permuted alongside the triangle order: recomputing
// them at every split is what makes a naive SAH build slow.
struct TriangleRef {
    float centroid[3];
    float boundsMin[3];
    float boundsMax[3];
    uint32_t source;
};

struct Bounds {
    float minimum[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float maximum[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

    void Add(const float point[3]) {
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = (std::min)(minimum[axis], point[axis]);
            maximum[axis] = (std::max)(maximum[axis], point[axis]);
        }
    }
    void Add(const Bounds& other) {
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = (std::min)(minimum[axis], other.minimum[axis]);
            maximum[axis] = (std::max)(maximum[axis], other.maximum[axis]);
        }
    }
    bool Valid() const { return minimum[0] <= maximum[0]; }
    // Half the surface area is enough: SAH compares costs, and the constant
    // factor cancels.
    float SurfaceArea() const {
        if (!Valid()) return 0.0f;
        const float dx = maximum[0] - minimum[0];
        const float dy = maximum[1] - minimum[1];
        const float dz = maximum[2] - minimum[2];
        return dx * dy + dy * dz + dz * dx;
    }
};

// Explicit build stack. Recursion would be fine at the measured depth of 37,
// but an iterative walk lets the top levels be threaded later without
// restructuring the algorithm.
struct BuildTask {
    uint32_t nodeIndex;
    uint32_t first;
    uint32_t count;
    uint32_t depth;
};

} // namespace

bool BuildCollisionMesh(std::vector<float> triangles, CollisionMesh& out,
                        const CollisionMeshBuildParams& params,
                        CollisionMeshBuildStats* stats) {
    const auto started = std::chrono::steady_clock::now();
    out.triangles.clear();
    out.nodes.clear();
    out.sourceTriangle.clear();

    CollisionMeshBuildStats local;
    const uint32_t inputCount = static_cast<uint32_t>(triangles.size() / 9);

    // Pass 1: centroids and bounds, skipping degenerates. A million-triangle
    // export always contains some zero-area faces; they poison SAH bins and
    // would produce NaN normals at query time.
    std::vector<TriangleRef> refs;
    refs.reserve(inputCount);
    for (uint32_t index = 0; index < inputCount; ++index) {
        const float* v = triangles.data() + static_cast<size_t>(index) * 9;
        const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] };
        const float e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
        const float cross[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                                 e1[2] * e2[0] - e1[0] * e2[2],
                                 e1[0] * e2[1] - e1[1] * e2[0] };
        const float areaSquared = cross[0] * cross[0] + cross[1] * cross[1] +
                                  cross[2] * cross[2];
        if (!(areaSquared > 1e-12f)) { ++local.degenerateSkipped; continue; }

        TriangleRef ref;
        ref.source = index;
        for (int axis = 0; axis < 3; ++axis) {
            const float a = v[axis], b = v[axis + 3], c = v[axis + 6];
            ref.centroid[axis] = (a + b + c) * (1.0f / 3.0f);
            ref.boundsMin[axis] = (std::min)({ a, b, c });
            ref.boundsMax[axis] = (std::max)({ a, b, c });
        }
        refs.push_back(ref);
    }

    const uint32_t count = static_cast<uint32_t>(refs.size());
    if (count == 0) {
        if (stats) { local.triangleCount = 0; *stats = local; }
        return false;
    }

    // Upper bound for a binary tree with the given leaf size, plus slack for
    // splits that land unevenly. Reserving avoids reallocating a multi-megabyte
    // vector mid-build.
    out.nodes.reserve(static_cast<size_t>(count) * 2 / (std::max)(1u, params.maxTrianglesPerLeaf) + 64);
    out.nodes.push_back({});

    const uint32_t binCount = (std::max)(2u, params.binCount);
    const uint32_t maxLeaf = (std::max)(1u, params.maxTrianglesPerLeaf);

    std::vector<BuildTask> stack;
    stack.push_back({ 0u, 0u, count, 0u });

    std::vector<Bounds> binBounds(binCount);
    std::vector<uint32_t> binCounts(binCount);

    while (!stack.empty()) {
        const BuildTask task = stack.back();
        stack.pop_back();
        local.maxDepth = (std::max)(local.maxDepth, task.depth);

        // Node bounds always cover the triangles, not just their centroids:
        // traversal tests geometry, so a centroid-only box would miss hits on
        // triangles that straddle the boundary.
        Bounds nodeBounds;
        Bounds centroidBounds;
        for (uint32_t i = 0; i < task.count; ++i) {
            const TriangleRef& ref = refs[task.first + i];
            nodeBounds.Add(ref.boundsMin);
            nodeBounds.Add(ref.boundsMax);
            centroidBounds.Add(ref.centroid);
        }

        CollisionMesh::Node& node = out.nodes[task.nodeIndex];
        std::memcpy(node.boundsMin, nodeBounds.minimum, sizeof(node.boundsMin));
        std::memcpy(node.boundsMax, nodeBounds.maximum, sizeof(node.boundsMax));

        const auto makeLeaf = [&]() {
            CollisionMesh::Node& leaf = out.nodes[task.nodeIndex];
            leaf.leftFirst = task.first;
            leaf.count = task.count;
            ++local.leafCount;
            local.maxLeafTriangles = (std::max)(local.maxLeafTriangles, task.count);
        };

        if (task.count <= maxLeaf || task.depth >= params.maxDepth) {
            makeLeaf();
            continue;
        }

        int axis = 0;
        float extent = -1.0f;
        for (int candidate = 0; candidate < 3; ++candidate) {
            const float span = centroidBounds.maximum[candidate] -
                               centroidBounds.minimum[candidate];
            if (span > extent) { extent = span; axis = candidate; }
        }

        uint32_t mid = 0;
        if (extent < 1e-9f) {
            // Every centroid coincides (stacked coplanar geometry). SAH has
            // nothing to work with; split by count so the recursion terminates.
            mid = task.first + task.count / 2;
        } else {
            std::fill(binBounds.begin(), binBounds.end(), Bounds{});
            std::fill(binCounts.begin(), binCounts.end(), 0u);
            const float scale = static_cast<float>(binCount) / extent;

            for (uint32_t i = 0; i < task.count; ++i) {
                const TriangleRef& ref = refs[task.first + i];
                uint32_t bin = static_cast<uint32_t>(
                    (ref.centroid[axis] - centroidBounds.minimum[axis]) * scale);
                bin = (std::min)(bin, binCount - 1);
                ++binCounts[bin];
                binBounds[bin].Add(ref.boundsMin);
                binBounds[bin].Add(ref.boundsMax);
            }

            // Forward/backward prefix sweep over the binCount-1 candidate
            // planes, so each split's cost is O(1) rather than O(bins).
            std::vector<float> leftArea(binCount, 0.0f);
            std::vector<uint32_t> leftCount(binCount, 0u);
            Bounds running;
            uint32_t runningCount = 0;
            for (uint32_t bin = 0; bin < binCount; ++bin) {
                running.Add(binBounds[bin]);
                runningCount += binCounts[bin];
                leftArea[bin] = running.SurfaceArea();
                leftCount[bin] = runningCount;
            }

            float bestCost = FLT_MAX;
            uint32_t bestSplit = binCount;
            Bounds rightRunning;
            uint32_t rightCount = 0;
            for (uint32_t bin = binCount - 1; bin > 0; --bin) {
                rightRunning.Add(binBounds[bin]);
                rightCount += binCounts[bin];
                const uint32_t leftTotal = leftCount[bin - 1];
                if (leftTotal == 0 || rightCount == 0) continue;
                const float cost = leftArea[bin - 1] * static_cast<float>(leftTotal) +
                                   rightRunning.SurfaceArea() * static_cast<float>(rightCount);
                if (cost < bestCost) { bestCost = cost; bestSplit = bin; }
            }

            // A leaf costs count * parentArea. If no split beats that, stop
            // subdividing -- this is what keeps the tree from degenerating into
            // one triangle per node on uniform geometry.
            const float leafCost = static_cast<float>(task.count) * nodeBounds.SurfaceArea();
            if (bestSplit == binCount || bestCost >= leafCost) {
                makeLeaf();
                continue;
            }

            const float threshold = centroidBounds.minimum[axis] +
                                    static_cast<float>(bestSplit) / scale;
            const auto middle = std::partition(
                refs.begin() + task.first, refs.begin() + task.first + task.count,
                [&](const TriangleRef& ref) { return ref.centroid[axis] < threshold; });
            mid = static_cast<uint32_t>(middle - refs.begin());

            // Guard against a partition that put everything on one side, which
            // floating-point edge cases at the bin boundary can still produce.
            if (mid == task.first || mid == task.first + task.count)
                mid = task.first + task.count / 2;
        }

        const uint32_t leftIndex = static_cast<uint32_t>(out.nodes.size());
        out.nodes.push_back({});
        out.nodes.push_back({});
        // Re-fetch: the push_backs above may have reallocated the vector, which
        // would leave an earlier reference dangling.
        out.nodes[task.nodeIndex].leftFirst = leftIndex;
        out.nodes[task.nodeIndex].count = 0;

        stack.push_back({ leftIndex + 1, mid, task.first + task.count - mid, task.depth + 1 });
        stack.push_back({ leftIndex, task.first, mid - task.first, task.depth + 1 });
    }

    // Emit triangles in leaf order so each leaf addresses a contiguous run.
    out.triangles.resize(static_cast<size_t>(count) * 9);
    out.sourceTriangle.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const TriangleRef& ref = refs[i];
        std::memcpy(out.triangles.data() + static_cast<size_t>(i) * 9,
                    triangles.data() + static_cast<size_t>(ref.source) * 9,
                    9 * sizeof(float));
        out.sourceTriangle[i] = ref.source;
    }

    local.triangleCount = count;
    local.nodeCount = static_cast<uint32_t>(out.nodes.size());
    local.buildMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (stats) *stats = local;
    return true;
}

namespace {

// Slab test against a node, with the bounds expanded by `radius`. Returns the
// near hit distance so the caller can order children front to back.
inline bool IntersectNode(const CollisionMesh::Node& node,
                          const float origin[3], const float inverseDir[3],
                          float radius, float limit, float& outNear) {
    float tNear = 0.0f;
    float tFar = limit;
    for (int axis = 0; axis < 3; ++axis) {
        const float low = (node.boundsMin[axis] - radius - origin[axis]) * inverseDir[axis];
        const float high = (node.boundsMax[axis] + radius - origin[axis]) * inverseDir[axis];
        tNear = (std::max)(tNear, (std::min)(low, high));
        tFar = (std::min)(tFar, (std::max)(low, high));
        if (tNear > tFar) return false;
    }
    outNear = tNear;
    return true;
}

// Moller-Trumbore, deliberately without backface culling: the player stands
// inside closed volumes and interior faces must block from both sides.
inline bool IntersectTriangle(const float* v, const float origin[3],
                              const float direction[3], float limit, float& outT) {
    const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] };
    const float e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
    const float p[3] = { direction[1] * e2[2] - direction[2] * e2[1],
                         direction[2] * e2[0] - direction[0] * e2[2],
                         direction[0] * e2[1] - direction[1] * e2[0] };
    const float determinant = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::abs(determinant) < 1e-12f) return false;
    const float inverse = 1.0f / determinant;
    const float t0[3] = { origin[0] - v[0], origin[1] - v[1], origin[2] - v[2] };
    const float u = (t0[0] * p[0] + t0[1] * p[1] + t0[2] * p[2]) * inverse;
    if (u < 0.0f || u > 1.0f) return false;
    const float q[3] = { t0[1] * e1[2] - t0[2] * e1[1],
                         t0[2] * e1[0] - t0[0] * e1[2],
                         t0[0] * e1[1] - t0[1] * e1[0] };
    const float vCoord = (direction[0] * q[0] + direction[1] * q[1] +
                          direction[2] * q[2]) * inverse;
    if (vCoord < 0.0f || u + vCoord > 1.0f) return false;
    const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inverse;
    if (t < 0.0f || t > limit) return false;
    outT = t;
    return true;
}

// Closest point on segment [a,b], written to `closest`. Callers that only want
// the distance ignore it; the pushout query needs the point itself to build a
// direction to move along.
inline float PointSegmentDistanceSquared(const float point[3], const float a[3],
                                         const float b[3], float closest[3]) {
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float ap[3] = { point[0] - a[0], point[1] - a[1], point[2] - a[2] };
    const float lengthSquared = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = 0.0f;
    if (lengthSquared > 1e-20f)
        t = (std::max)(0.0f, (std::min)(1.0f,
            (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / lengthSquared));
    closest[0] = a[0] + ab[0] * t;
    closest[1] = a[1] + ab[1] * t;
    closest[2] = a[2] + ab[2] * t;
    const float d[3] = { point[0] - closest[0], point[1] - closest[1],
                         point[2] - closest[2] };
    return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
}

// Squared distance from a point to a triangle: project onto the plane, accept
// if the projection is inside, otherwise fall back to the nearest edge.
inline float PointTriangleDistanceSquared(const float point[3], const float* v,
                                          float closest[3]) {
    const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] };
    const float e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
    float normal[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                        e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0] };
    const float lengthSquared = normal[0] * normal[0] + normal[1] * normal[1] +
                                normal[2] * normal[2];
    if (lengthSquared > 1e-20f) {
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        normal[0] *= inverseLength; normal[1] *= inverseLength; normal[2] *= inverseLength;
        const float offset[3] = { point[0] - v[0], point[1] - v[1], point[2] - v[2] };
        const float distance = offset[0] * normal[0] + offset[1] * normal[1] +
                               offset[2] * normal[2];
        const float projected[3] = { point[0] - normal[0] * distance,
                                     point[1] - normal[1] * distance,
                                     point[2] - normal[2] * distance };
        // Inside test by edge cross products against the face normal.
        bool inside = true;
        for (int edge = 0; edge < 3 && inside; ++edge) {
            const float* a = v + edge * 3;
            const float* b = v + ((edge + 1) % 3) * 3;
            const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const float ap[3] = { projected[0] - a[0], projected[1] - a[1],
                                  projected[2] - a[2] };
            const float cross[3] = { ab[1] * ap[2] - ab[2] * ap[1],
                                     ab[2] * ap[0] - ab[0] * ap[2],
                                     ab[0] * ap[1] - ab[1] * ap[0] };
            if (cross[0] * normal[0] + cross[1] * normal[1] +
                cross[2] * normal[2] < 0.0f) inside = false;
        }
        if (inside) {
            closest[0] = projected[0];
            closest[1] = projected[1];
            closest[2] = projected[2];
            return distance * distance;
        }
    }
    // Outside the face, or the triangle is degenerate: the nearest point lies on
    // one of the edges.
    float best[3];
    float bestDistance = PointSegmentDistanceSquared(point, v, v + 3, best);
    float candidate[3];
    float distance = PointSegmentDistanceSquared(point, v + 3, v + 6, candidate);
    if (distance < bestDistance) {
        bestDistance = distance;
        best[0] = candidate[0]; best[1] = candidate[1]; best[2] = candidate[2];
    }
    distance = PointSegmentDistanceSquared(point, v + 6, v, candidate);
    if (distance < bestDistance) {
        bestDistance = distance;
        best[0] = candidate[0]; best[1] = candidate[1]; best[2] = candidate[2];
    }
    closest[0] = best[0]; closest[1] = best[1]; closest[2] = best[2];
    return bestDistance;
}

} // namespace

bool CollisionMeshRaycast(const CollisionMesh& mesh, const XMFLOAT3& start,
                          const XMFLOAT3& end, float radius,
                          CollisionMeshRayHit& hit) {
    if (mesh.Empty()) return false;

    const float origin[3] = { start.x, start.y, start.z };
    const float direction[3] = { end.x - start.x, end.y - start.y, end.z - start.z };
    const float lengthSquared = direction[0] * direction[0] +
                                direction[1] * direction[1] +
                                direction[2] * direction[2];
    if (lengthSquared < 1e-20f) return false;

    float inverseDir[3];
    for (int axis = 0; axis < 3; ++axis) {
        // Copysign rather than a per-node branch: an axis-parallel ray gets a
        // signed infinity, and the slab test handles it correctly.
        inverseDir[axis] = std::abs(direction[axis]) < 1e-20f
            ? std::copysign(FLT_MAX, direction[axis] == 0.0f ? 1.0f : direction[axis])
            : 1.0f / direction[axis];
    }
    // Radius is expressed along the segment's own parameterisation, so it has
    // to be scaled out of world units into t units.
    const float radiusT = radius > 0.0f ? radius / std::sqrt(lengthSquared) : 0.0f;

    bool found = false;
    // `best` is the reported hit, clamped onto the segment so callers that
    // interpolate `point` from it stay within [start, end].
    //
    // `limit` is the search bound the traversal actually prunes against. It
    // starts one radius past the segment end -- catching a triangle just beyond
    // `end` is the whole point of a thickened query -- and only ever shrinks as
    // nearer hits are found, which is what keeps the front-to-back early-out
    // valid.
    float best = 1.0f;
    float limit = 1.0f + radiusT;
    uint32_t bestTriangle = 0;

    // Depth 37 measured on the airport; 64 matches maxDepth and leaves slack.
    uint32_t stack[64];
    uint32_t depth = 0;
    stack[depth++] = 0;

    while (depth > 0) {
        const uint32_t nodeIndex = stack[--depth];
        const CollisionMesh::Node& node = mesh.nodes[nodeIndex];
        float nodeNear = 0.0f;
        if (!IntersectNode(node, origin, inverseDir, radius, limit, nodeNear)) continue;
        if (nodeNear > limit) continue;

        if (node.count > 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t triangleIndex = node.leftFirst + i;
                const float* v = mesh.triangles.data() +
                                 static_cast<size_t>(triangleIndex) * 9;
                float t;
                if (!IntersectTriangle(v, origin, direction, limit, t)) continue;
                if (t < limit) {
                    // Tighten the search to this hit, so every later triangle
                    // and node is pruned against it. The reported distance is
                    // clamped onto the segment; the overshoot a radius allows
                    // is a thickness fudge, not a claim that the ray travelled
                    // past its end.
                    limit = t;
                    best = (std::min)(t, 1.0f);
                    bestTriangle = triangleIndex;
                    found = true;
                }
            }
            continue;
        }

        // Front-to-back: push the far child first so the near one is popped
        // next. Combined with the `nodeNear > limit` reject above, this is what
        // keeps a typical hit to tens of node visits rather than thousands.
        const uint32_t left = node.leftFirst;
        const uint32_t right = left + 1;
        float leftNear = 0.0f, rightNear = 0.0f;
        const bool hitLeft = IntersectNode(mesh.nodes[left], origin, inverseDir,
                                           radius, limit, leftNear);
        const bool hitRight = IntersectNode(mesh.nodes[right], origin, inverseDir,
                                            radius, limit, rightNear);
        if (hitLeft && hitRight) {
            if (leftNear <= rightNear) {
                if (depth + 2 <= 64) { stack[depth++] = right; stack[depth++] = left; }
            } else {
                if (depth + 2 <= 64) { stack[depth++] = left; stack[depth++] = right; }
            }
        } else if (hitLeft) {
            if (depth < 64) stack[depth++] = left;
        } else if (hitRight) {
            if (depth < 64) stack[depth++] = right;
        }
    }

    if (!found) return false;

    const float* v = mesh.triangles.data() + static_cast<size_t>(bestTriangle) * 9;
    const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] };
    const float e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
    float normal[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                        e1[2] * e2[0] - e1[0] * e2[2],
                        e1[0] * e2[1] - e1[1] * e2[0] };
    const float normalLengthSquared = normal[0] * normal[0] +
                                      normal[1] * normal[1] + normal[2] * normal[2];
    if (normalLengthSquared > 1e-20f) {
        const float inverseLength = 1.0f / std::sqrt(normalLengthSquared);
        normal[0] *= inverseLength; normal[1] *= inverseLength; normal[2] *= inverseLength;
    } else {
        normal[0] = 0.0f; normal[1] = 1.0f; normal[2] = 0.0f;
    }
    // Always oppose the ray. Scraped assets have inconsistent winding and the
    // player shoots interior walls from inside, so respecting winding would
    // point the normal into the surface half the time.
    if (normal[0] * direction[0] + normal[1] * direction[1] +
        normal[2] * direction[2] > 0.0f) {
        normal[0] = -normal[0]; normal[1] = -normal[1]; normal[2] = -normal[2];
    }

    hit.t = best;
    hit.point = XMFLOAT3(start.x + direction[0] * best,
                         start.y + direction[1] * best,
                         start.z + direction[2] * best);
    hit.normal = XMFLOAT3(normal[0], normal[1], normal[2]);
    hit.triangle = bestTriangle;
    return true;
}

bool CollisionMeshOverlapSphere(const CollisionMesh& mesh,
                                const XMFLOAT3& center, float radius) {
    if (mesh.Empty() || radius <= 0.0f) return false;
    const float point[3] = { center.x, center.y, center.z };
    const float radiusSquared = radius * radius;

    uint32_t stack[64];
    uint32_t depth = 0;
    stack[depth++] = 0;

    while (depth > 0) {
        const CollisionMesh::Node& node = mesh.nodes[stack[--depth]];
        float outside = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            const float over = (std::max)(0.0f,
                (std::max)(node.boundsMin[axis] - point[axis],
                           point[axis] - node.boundsMax[axis]));
            outside += over * over;
        }
        if (outside > radiusSquared) continue;

        if (node.count > 0) {
            for (uint32_t i = 0; i < node.count; ++i) {
                const float* v = mesh.triangles.data() +
                                 static_cast<size_t>(node.leftFirst + i) * 9;
                float closest[3];
                if (PointTriangleDistanceSquared(point, v, closest) <= radiusSquared)
                    return true;
            }
            continue;
        }
        if (depth + 2 <= 64) {
            stack[depth++] = node.leftFirst;
            stack[depth++] = node.leftFirst + 1;
        }
    }
    return false;
}

namespace {

// Gathers triangles whose bounds overlap the query box. Shared by the capsule
// resolve, which needs every nearby triangle rather than a first hit.
void GatherOverlappingTriangles(const CollisionMesh& mesh,
                                const float boxMin[3], const float boxMax[3],
                                std::vector<uint32_t>& out) {
    out.clear();
    if (mesh.Empty()) return;
    uint32_t stack[64];
    uint32_t depth = 0;
    stack[depth++] = 0;
    while (depth > 0) {
        const CollisionMesh::Node& node = mesh.nodes[stack[--depth]];
        bool separated = false;
        for (int axis = 0; axis < 3 && !separated; ++axis)
            if (node.boundsMin[axis] > boxMax[axis] ||
                node.boundsMax[axis] < boxMin[axis]) separated = true;
        if (separated) continue;
        if (node.count > 0) {
            for (uint32_t i = 0; i < node.count; ++i)
                out.push_back(node.leftFirst + i);
            continue;
        }
        if (depth + 2 <= 64) {
            stack[depth++] = node.leftFirst;
            stack[depth++] = node.leftFirst + 1;
        }
    }
}

// Unit geometric normal of a triangle. Winding is not trusted -- the caller
// orients it against the direction it needs.
inline bool TriangleNormal(const float* v, float normal[3]) {
    const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] };
    const float e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
    normal[0] = e1[1] * e2[2] - e1[2] * e2[1];
    normal[1] = e1[2] * e2[0] - e1[0] * e2[2];
    normal[2] = e1[0] * e2[1] - e1[1] * e2[0];
    const float lengthSquared = normal[0] * normal[0] + normal[1] * normal[1] +
                                normal[2] * normal[2];
    if (!(lengthSquared > 1e-20f)) return false;
    const float inverse = 1.0f / std::sqrt(lengthSquared);
    normal[0] *= inverse; normal[1] *= inverse; normal[2] *= inverse;
    return true;
}

} // namespace

CollisionMeshPushout CollisionMeshResolveCapsule(
    const CollisionMesh& mesh, const XMFLOAT3& base, float radius,
    float height, float stepHeight) {
    CollisionMeshPushout result;
    if (mesh.Empty() || radius <= 0.0f) return result;

    // The capsule is sampled as a stack of spheres. Three is enough for a human
    // figure against building geometry and keeps this ~3x a single sphere test:
    // feet, middle, head. The feet sphere sits a radius up so it rests on the
    // floor rather than half-buried in it.
    const float usableHeight = (std::max)(height, radius * 2.0f);
    const float lowest = radius;
    const float highest = usableHeight - radius;
    constexpr int kSamples = 3;
    float centerY[kSamples];
    for (int i = 0; i < kSamples; ++i) {
        centerY[i] = lowest + (highest - lowest) *
            (static_cast<float>(i) / static_cast<float>(kSamples - 1));
    }

    XMFLOAT3 offset(0.0f, 0.0f, 0.0f);
    std::vector<uint32_t> candidates;

    // Each pass re-gathers against the corrected position: pushing out of one
    // wall can move the capsule into another, and resolving against stale
    // candidates is how a body ends up wedged in a corner.
    constexpr int kPasses = 4;
    for (int pass = 0; pass < kPasses; ++pass) {
        const float originX = base.x + offset.x;
        const float originY = base.y + offset.y;
        const float originZ = base.z + offset.z;
        const float boxMin[3] = { originX - radius, originY - radius,
                                  originZ - radius };
        const float boxMax[3] = { originX + radius, originY + usableHeight + radius,
                                  originZ + radius };
        GatherOverlappingTriangles(mesh, boxMin, boxMax, candidates);
        if (candidates.empty()) break;

        float deepest = 0.0f;
        float pushNormal[3] = { 0.0f, 0.0f, 0.0f };
        for (uint32_t index : candidates) {
            const float* v = mesh.triangles.data() +
                             static_cast<size_t>(index) * 9;
            float normal[3];
            if (!TriangleNormal(v, normal)) continue;

            for (int sample = 0; sample < kSamples; ++sample) {
                const float point[3] = { originX, originY + centerY[sample],
                                         originZ };
                float closest[3];
                const float distanceSquared =
                    PointTriangleDistanceSquared(point, v, closest);
                // Small tolerance rather than strict penetration: a player
                // resting exactly on a floor sits at exactly `radius` from it,
                // and that is the common case, not an edge case. Without the
                // slack the ground under a standing player reports no contact
                // and floorY never rises.
                const float contactRadius = radius + 1e-3f;
                if (distanceSquared >= contactRadius * contactRadius) continue;

                result.touched = true;
                // Walkable surfaces under the capsule raise the floor rather
                // than pushing horizontally, which is what lets the player
                // stand on a hangar floor and step over low sills.
                float upward[3] = { normal[0], normal[1], normal[2] };
                if (upward[1] < 0.0f) {
                    upward[0] = -upward[0]; upward[1] = -upward[1];
                    upward[2] = -upward[2];
                }
                const bool walkable = upward[1] > 0.7f;
                if (walkable && closest[1] <= originY + stepHeight) {
                    if (!result.hasFloor || closest[1] > result.floorY) {
                        result.floorY = closest[1];
                        result.hasFloor = true;
                    }
                    continue;
                }

                const float distance = std::sqrt((std::max)(distanceSquared, 0.0f));
                const float penetration = radius - distance;
                if (penetration <= deepest) continue;

                // Direction from the surface toward the sphere centre. When the
                // centre sits exactly on the triangle the difference is
                // degenerate, so fall back to the face normal.
                float direction[3] = { point[0] - closest[0], point[1] - closest[1],
                                       point[2] - closest[2] };
                const float lengthSquared = direction[0] * direction[0] +
                                            direction[1] * direction[1] +
                                            direction[2] * direction[2];
                if (lengthSquared > 1e-12f) {
                    const float inverse = 1.0f / std::sqrt(lengthSquared);
                    direction[0] *= inverse; direction[1] *= inverse;
                    direction[2] *= inverse;
                } else {
                    direction[0] = normal[0]; direction[1] = normal[1];
                    direction[2] = normal[2];
                }
                // Never push the player downward out of a wall: that drives them
                // into the floor and, with gravity, through it.
                if (direction[1] < 0.0f) direction[1] = 0.0f;
                const float horizontalSquared = direction[0] * direction[0] +
                                                direction[1] * direction[1] +
                                                direction[2] * direction[2];
                if (!(horizontalSquared > 1e-12f)) continue;
                const float renormalize = 1.0f / std::sqrt(horizontalSquared);

                deepest = penetration;
                pushNormal[0] = direction[0] * renormalize;
                pushNormal[1] = direction[1] * renormalize;
                pushNormal[2] = direction[2] * renormalize;
            }
        }

        if (deepest <= 0.0f) break;
        // Slight overshoot so the next pass starts clear of the surface rather
        // than exactly touching it, which would re-trigger on floating-point noise.
        const float correction = deepest + 1e-4f;
        offset.x += pushNormal[0] * correction;
        offset.y += pushNormal[1] * correction;
        offset.z += pushNormal[2] * correction;
    }

    result.displacement = offset;
    return result;
}

CollisionMeshPushout CollisionMeshInstanceResolveCapsule(
    const CollisionMeshInstance& instance, const XMFLOAT3& base,
    float radius, float height, float stepHeight) {
    CollisionMeshPushout result;
    if (!instance.mesh || instance.mesh->Empty()) return result;

    // Broadphase: a capsule nowhere near the instance never pays for the
    // transform or the tree walk.
    const float* boundsMin = &instance.worldBoundsMin.x;
    const float* boundsMax = &instance.worldBoundsMax.x;
    if (base.x + radius < boundsMin[0] || base.x - radius > boundsMax[0] ||
        base.z + radius < boundsMin[2] || base.z - radius > boundsMax[2] ||
        base.y + height < boundsMin[1] || base.y > boundsMax[1])
        return result;

    const XMMATRIX inverse = XMLoadFloat4x4(&instance.inverseWorld);
    XMFLOAT3 localBase;
    XMStoreFloat3(&localBase, XMVector3TransformCoord(XMLoadFloat3(&base), inverse));

    const float scale = (std::max)(instance.uniformScale, 1e-6f);
    const CollisionMeshPushout local = CollisionMeshResolveCapsule(
        *instance.mesh, localBase, radius / scale, height / scale,
        stepHeight / scale);
    if (!local.touched) return result;

    const XMMATRIX world = XMLoadFloat4x4(&instance.worldTransform);
    // Displacement is a direction and length, so it transforms as a vector --
    // applying the full matrix would add the instance's translation to it.
    XMFLOAT3 worldDisplacement;
    XMStoreFloat3(&worldDisplacement, XMVector3TransformNormal(
        XMLoadFloat3(&local.displacement), world));
    result.displacement = worldDisplacement;
    result.touched = true;

    if (local.hasFloor) {
        // The floor is a position, so it takes the full transform. Carry x/z
        // through from the local contact so a rotated instance maps its height
        // correctly rather than sampling the wrong point.
        const XMFLOAT3 localFloor(localBase.x, local.floorY, localBase.z);
        XMFLOAT3 worldFloor;
        XMStoreFloat3(&worldFloor, XMVector3TransformCoord(
            XMLoadFloat3(&localFloor), world));
        result.floorY = worldFloor.y;
        result.hasFloor = true;
    }
    return result;
}

void InitializeCollisionMeshInstance(CollisionMeshInstance& instance,
                                     const CollisionMesh& mesh,
                                     FXMMATRIX world) {
    instance.mesh = &mesh;
    XMStoreFloat4x4(&instance.worldTransform, world);
    XMStoreFloat4x4(&instance.inverseWorld, XMMatrixInverse(nullptr, world));

    float scaleX, scaleY, scaleZ;
    XMStoreFloat(&scaleX, XMVector3Length(world.r[0]));
    XMStoreFloat(&scaleY, XMVector3Length(world.r[1]));
    XMStoreFloat(&scaleZ, XMVector3Length(world.r[2]));
    instance.uniformScale = (std::max)({ scaleX, scaleY, scaleZ, 1e-6f });

    if (mesh.Empty()) {
        instance.worldBoundsMin = XMFLOAT3(0.0f, 0.0f, 0.0f);
        instance.worldBoundsMax = XMFLOAT3(0.0f, 0.0f, 0.0f);
        return;
    }

    // Transform all eight corners: under rotation the root AABB's corners do
    // not map to the transformed AABB's corners, so taking min/max of the two
    // extreme points alone would under-cover the instance.
    const CollisionMesh::Node& root = mesh.nodes[0];
    XMVECTOR minimum = XMVectorReplicate(FLT_MAX);
    XMVECTOR maximum = XMVectorReplicate(-FLT_MAX);
    for (int corner = 0; corner < 8; ++corner) {
        const XMVECTOR point = XMVectorSet(
            (corner & 1) ? root.boundsMax[0] : root.boundsMin[0],
            (corner & 2) ? root.boundsMax[1] : root.boundsMin[1],
            (corner & 4) ? root.boundsMax[2] : root.boundsMin[2], 1.0f);
        const XMVECTOR transformed = XMVector3TransformCoord(point, world);
        minimum = XMVectorMin(minimum, transformed);
        maximum = XMVectorMax(maximum, transformed);
    }
    XMStoreFloat3(&instance.worldBoundsMin, minimum);
    XMStoreFloat3(&instance.worldBoundsMax, maximum);
}

bool CollisionMeshInstanceRaycast(const CollisionMeshInstance& instance,
                                  const XMFLOAT3& start, const XMFLOAT3& end,
                                  float radius, CollisionMeshRayHit& hit) {
    if (!instance.mesh || instance.mesh->Empty()) return false;

    // Broadphase against the world bounds, expanded by the query radius, so a
    // distant shot never pays for the inverse transform or the tree walk.
    const float segmentMin[3] = { (std::min)(start.x, end.x) - radius,
                                  (std::min)(start.y, end.y) - radius,
                                  (std::min)(start.z, end.z) - radius };
    const float segmentMax[3] = { (std::max)(start.x, end.x) + radius,
                                  (std::max)(start.y, end.y) + radius,
                                  (std::max)(start.z, end.z) + radius };
    const float* boundsMin = &instance.worldBoundsMin.x;
    const float* boundsMax = &instance.worldBoundsMax.x;
    for (int axis = 0; axis < 3; ++axis)
        if (segmentMin[axis] > boundsMax[axis] || segmentMax[axis] < boundsMin[axis])
            return false;

    const XMMATRIX inverse = XMLoadFloat4x4(&instance.inverseWorld);
    XMFLOAT3 localStart, localEnd;
    XMStoreFloat3(&localStart, XMVector3TransformCoord(XMLoadFloat3(&start), inverse));
    XMStoreFloat3(&localEnd, XMVector3TransformCoord(XMLoadFloat3(&end), inverse));

    CollisionMeshRayHit localHit;
    if (!CollisionMeshRaycast(*instance.mesh, localStart, localEnd,
                              radius / instance.uniformScale, localHit))
        return false;

    const XMMATRIX world = XMLoadFloat4x4(&instance.worldTransform);
    XMStoreFloat3(&hit.point,
        XMVector3TransformCoord(XMLoadFloat3(&localHit.point), world));
    // Normals transform by the inverse transpose, which is what keeps them
    // perpendicular to the surface under non-uniform scale.
    const XMMATRIX normalMatrix = XMMatrixTranspose(inverse);
    XMVECTOR normal = XMVector3Normalize(
        XMVector3TransformNormal(XMLoadFloat3(&localHit.normal), normalMatrix));
    // The local-space flip does not survive a mirroring transform, so re-apply
    // it in world space.
    const XMVECTOR direction = XMVectorSubtract(XMLoadFloat3(&end), XMLoadFloat3(&start));
    if (XMVectorGetX(XMVector3Dot(normal, direction)) > 0.0f)
        normal = XMVectorNegate(normal);
    XMStoreFloat3(&hit.normal, normal);
    hit.t = localHit.t;
    hit.triangle = localHit.triangle;
    return true;
}
