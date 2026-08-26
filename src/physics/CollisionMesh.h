#ifndef COLLISION_MESH_H
#define COLLISION_MESH_H

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

// Immutable per-triangle collision geometry for static prefab instances.
//
// Built once in prefab-model space (that is, after LoadPrefabModel's targetSize
// normalization has mutated the root transform) and shared by every instance of
// that prefab. Instances transform queries into this space rather than each
// owning a copy of a million triangles.
//
// Deliberately free of D3D12, the scene graph and nlohmann: this header is
// included from src/assets/prefabs/, which compiles into several lightweight
// test targets that must not acquire a renderer dependency. The scene-graph
// traversal that produces the triangle soup lives in main.cpp beside
// ComputePrefabModelBounds; everything here takes raw float arrays.
struct CollisionMesh {
    // Triangle soup, 9 floats per triangle: v0.xyz, v1.xyz, v2.xyz.
    // Deliberately not indexed -- the leaf loop is the hot path, and chasing an
    // index buffer costs more than the memory the shared vertices would save.
    std::vector<float> triangles;

    struct Node {
        float boundsMin[3];
        // Interior: index of the first child (the second is first + 1, since
        // children are always stored adjacent). Leaf: first triangle index.
        uint32_t leftFirst = 0;
        float boundsMax[3];
        // 0 marks an interior node. Leaves hold at most
        // CollisionMeshBuildParams::maxTrianglesPerLeaf triangles.
        uint32_t count = 0;
    };
    static_assert(sizeof(Node) == 32, "Node must stay 32 bytes: two per cache line");

    std::vector<Node> nodes;

    // Build permutation. triangles[] is already reordered so leaves address a
    // contiguous range; this maps a leaf triangle back to its index in the
    // caller's original array, so a hit can later be resolved to a source
    // primitive (surface-specific impact audio, per-material decals).
    std::vector<uint32_t> sourceTriangle;

    uint32_t TriangleCount() const {
        return static_cast<uint32_t>(triangles.size() / 9);
    }
    bool Empty() const { return nodes.empty(); }
    size_t MemoryBytes() const {
        return triangles.size() * sizeof(float) +
               nodes.size() * sizeof(Node) +
               sourceTriangle.size() * sizeof(uint32_t);
    }
};

// Build parameters. Measured against a 1,004,411-triangle airport: 12 bins and
// 8-triangle leaves produced 366,919 nodes at max depth 37. Changing any of
// these invalidates every cached tree on disk -- see CollisionMeshCache's
// buildParamsHash.
struct CollisionMeshBuildParams {
    uint32_t binCount = 12;
    uint32_t maxTrianglesPerLeaf = 8;
    // Measured max depth was 37; this is a guard against pathological input,
    // not a tuning knob. The traversal stack is sized to match.
    uint32_t maxDepth = 64;
};

struct CollisionMeshBuildStats {
    uint32_t triangleCount = 0;
    uint32_t degenerateSkipped = 0;
    uint32_t nodeCount = 0;
    uint32_t leafCount = 0;
    uint32_t maxDepth = 0;
    uint32_t maxLeafTriangles = 0;
    double buildMilliseconds = 0.0;
};

// Consumes `triangles` (9 floats per triangle, already in the target space) and
// produces a BVH over them. Returns false when the input holds no usable
// triangle, leaving `out` empty rather than half-built.
bool BuildCollisionMesh(std::vector<float> triangles, CollisionMesh& out,
                        const CollisionMeshBuildParams& params = {},
                        CollisionMeshBuildStats* stats = nullptr);

struct CollisionMeshRayHit {
    // Parametric position along [start, end], so callers can compare hits from
    // several meshes without recomputing distances.
    float t = 1.0f;
    DirectX::XMFLOAT3 point{};
    // Always unit length, and always flipped to oppose the ray direction. The
    // player stands inside closed volumes (hangars) and scraped assets have
    // inconsistent winding, so a geometric normal that respects winding would
    // point the wrong way half the time.
    DirectX::XMFLOAT3 normal{};
    uint32_t triangle = 0;
};

// Nearest-hit segment cast. `radius` thickens the ray conservatively: node
// bounds are expanded and the triangle test is offset. It is not an exact swept
// sphere -- every caller uses radius as a "the bullet is slightly fat" fudge
// (0.04 line-of-sight, 0.08 cover rays, bullet radius), never as real physics.
bool CollisionMeshRaycast(const CollisionMesh& mesh,
                          const DirectX::XMFLOAT3& start,
                          const DirectX::XMFLOAT3& end,
                          float radius, CollisionMeshRayHit& hit);

// Boolean sphere overlap, early-outs on the first touching triangle.
bool CollisionMeshOverlapSphere(const CollisionMesh& mesh,
                                const DirectX::XMFLOAT3& center, float radius);

// Result of resolving a sphere against the mesh.
struct CollisionMeshPushout {
    // Displacement to add to the sphere centre so it no longer intersects any
    // triangle. Zero when nothing was touched.
    DirectX::XMFLOAT3 displacement{};
    // Highest surface point under the sphere whose triangle is walkable (its
    // normal points sufficiently upward). This is what a caller raises floorY
    // to, so the player stands on a hangar floor instead of sinking through it.
    float floorY = 0.0f;
    bool hasFloor = false;
    bool touched = false;
};

// Resolves a vertical capsule, approximated by a stack of spheres, against the
// mesh.
//
// Iterative: each pass finds the deepest overlapping triangle, pushes out along
// its normal, and repeats. That resolves a corner where two walls both push, and
// converges in a handful of passes -- resolving every contact at once instead
// tends to double-count and shove the player through the opposite wall.
//
// `stepHeight` is how far above the sphere's base a walkable surface may be and
// still count as floor rather than as a wall to be blocked by.
CollisionMeshPushout CollisionMeshResolveCapsule(
    const CollisionMesh& mesh, const DirectX::XMFLOAT3& base, float radius,
    float height, float stepHeight);

// One placed instance of a shared CollisionMesh.
//
// Stores the full inverse world matrix rather than PrefabCollider's single yaw
// angle. That struct discards pitch and roll and mishandles a rotated
// non-uniform scale; baked triangles have no such limitation, so there is no
// reason to inherit it.
struct CollisionMeshInstance {
    uint64_t entityId = 0;
    // Borrowed from the prefab model cache, which holds it by shared_ptr and
    // outlives every instance through the deferred-release queue.
    const CollisionMesh* mesh = nullptr;
    DirectX::XMFLOAT4X4 worldTransform{};
    DirectX::XMFLOAT4X4 inverseWorld{};
    // World-space bounds of the root node. Broadphase: a query that misses this
    // never touches the inverse matrix or the tree.
    DirectX::XMFLOAT3 worldBoundsMin{};
    DirectX::XMFLOAT3 worldBoundsMax{};
    // Largest scale component of the linear part. Query radii are divided by it
    // so the local-space radius is never smaller than the true requirement:
    // under non-uniform scale a hit may be reported marginally early, but never
    // missed.
    float uniformScale = 1.0f;
};

// Fills worldTransform/inverseWorld/uniformScale and the world bounds from the
// mesh's root node. `world` is the instance's full SRT matrix.
void InitializeCollisionMeshInstance(CollisionMeshInstance& instance,
                                     const CollisionMesh& mesh,
                                     DirectX::FXMMATRIX world);

// World-space nearest-hit cast against one instance. Handles the broadphase
// rejection, the transform into and out of mesh space, and the radius scaling.
bool CollisionMeshInstanceRaycast(const CollisionMeshInstance& instance,
                                  const DirectX::XMFLOAT3& start,
                                  const DirectX::XMFLOAT3& end,
                                  float radius, CollisionMeshRayHit& hit);

// World-space capsule resolve against one instance. `base` is the capsule's
// bottom (the player's feet). Returns a world-space displacement and, where the
// capsule is standing on walkable geometry, a world floor height.
//
// Assumes a near-uniform instance scale, which every prefab placement uses: the
// capsule is transformed by dividing through uniformScale rather than being
// skewed into mesh space, because a genuinely non-uniform scale would turn the
// sphere into an ellipsoid this test cannot represent.
CollisionMeshPushout CollisionMeshInstanceResolveCapsule(
    const CollisionMeshInstance& instance, const DirectX::XMFLOAT3& base,
    float radius, float height, float stepHeight);

#endif
