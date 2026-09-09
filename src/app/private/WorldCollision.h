#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// World-space collision triangles baked from the wreck's mesh. The airframe is
// static once it is down, so the mesh can be transformed through the world
// matrix once at impact and collided against directly -- far more faithful than
// any box approximation, and free per frame beyond the triangle test itself.
struct BlackHawkCollisionMesh {
    struct Triangle {
        XMFLOAT3 a, b, c;
        XMFLOAT3 normal;   // unit, faces out of the hull
    };
    std::vector<Triangle> triangles;
    XMFLOAT3 boundsMin{}, boundsMax{};
    // Uniform XZ grid over the bounds. The airframe is ~38k triangles, so
    // testing them all per frame is far too slow to do while merely standing
    // next to the wreck; each cell lists only the triangles overlapping it.
    static constexpr float kCellSize = 1.0f;
    int cellsX = 0, cellsZ = 0;
    std::vector<std::vector<int>> cells;
    bool valid = false;

    void Clear() {
        triangles.clear();
        cells.clear();
        cellsX = cellsZ = 0;
        valid = false;
    }

    const std::vector<int>* CellAt(float x, float z) const {
        if (!valid) return nullptr;
        const int ix = int((x - boundsMin.x) / kCellSize);
        const int iz = int((z - boundsMin.z) / kCellSize);
        if (ix < 0 || iz < 0 || ix >= cellsX || iz >= cellsZ) return nullptr;
        return &cells[size_t(iz) * cellsX + ix];
    }
};

static BlackHawkCollisionMesh g_blackHawkCollisionMesh;

// Walks the model's primitives, transforms every triangle into world space and
// keeps the ones that can matter to a walking player. Called once, the frame
// the wreck settles.
static void BakeBlackHawkCollisionMesh() {
    g_blackHawkCollisionMesh.Clear();
    if (!g_blackHawkModel) return;

    const XMMATRIX world = BlackHawkWorldMatrix();
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    // The blades sweep >13 m; collide against the airframe only, matching what
    // a player can actually walk into. Two ways to spot the rotor, because the
    // two airframes rig it differently: the UH-60 gives its disc dedicated
    // materials, while the NewBlackHawk's disc shares 'корпус'/'детали' with the
    // fuselage and is told apart only by hanging under the rotor node. Skipping
    // that whole subtree is what keeps a 13 m spinning disc from baking into
    // the walkable wreck.
    const SceneNode* const rotorSubtree = g_newBlackHawkRotorNode.get();
    std::function<void(const SceneNode*, const XMMATRIX&)> collect =
        [&](const SceneNode* node, const XMMATRIX& parentToRoot) {
        if (!node) return;
        if (rotorSubtree && node == rotorSubtree) return;
        const XMMATRIX localToRoot =
            XMLoadFloat4x4(&node->localTransform) * parentToRoot;
        if (node->mesh) {
            for (const MeshPrimitive& primitive : node->mesh->primitives) {
                if (primitive.material &&
                    (primitive.material->name == "rotor" ||
                     primitive.material->name == "rotorhead")) continue;
                if (primitive.vertices.empty()) continue;
                const XMMATRIX toWorld = localToRoot * world;
                auto vertexAt = [&](unsigned int index) {
                    const size_t base = size_t(index) * 12;
                    XMFLOAT3 out{};
                    if (base + 2 >= primitive.vertices.size()) return out;
                    XMStoreFloat3(&out, XMVector3TransformCoord(
                        XMVectorSet(primitive.vertices[base],
                                    primitive.vertices[base + 1],
                                    primitive.vertices[base + 2], 1.0f),
                        toWorld));
                    return out;
                };
                const size_t indexCount = primitive.indices.size();
                for (size_t i = 0; i + 2 < indexCount; i += 3) {
                    BlackHawkCollisionMesh::Triangle triangle;
                    triangle.a = vertexAt(primitive.indices[i]);
                    triangle.b = vertexAt(primitive.indices[i + 1]);
                    triangle.c = vertexAt(primitive.indices[i + 2]);
                    const XMVECTOR va = XMLoadFloat3(&triangle.a);
                    const XMVECTOR edge1 = XMLoadFloat3(&triangle.b) - va;
                    const XMVECTOR edge2 = XMLoadFloat3(&triangle.c) - va;
                    const XMVECTOR cross = XMVector3Cross(edge1, edge2);
                    // Drop degenerate slivers: they contribute no surface and
                    // their normals are numerical noise.
                    if (XMVectorGetX(XMVector3LengthSq(cross)) < 1e-10f) continue;
                    XMStoreFloat3(&triangle.normal, XMVector3Normalize(cross));
                    g_blackHawkCollisionMesh.triangles.push_back(triangle);
                    for (const XMFLOAT3* point :
                         { &triangle.a, &triangle.b, &triangle.c }) {
                        minimum.x = (std::min)(minimum.x, point->x);
                        minimum.y = (std::min)(minimum.y, point->y);
                        minimum.z = (std::min)(minimum.z, point->z);
                        maximum.x = (std::max)(maximum.x, point->x);
                        maximum.y = (std::max)(maximum.y, point->y);
                        maximum.z = (std::max)(maximum.z, point->z);
                    }
                }
            }
        }
        for (const auto& child : node->children) collect(child.get(), localToRoot);
    };
    collect(g_blackHawkModel.get(), XMMatrixIdentity());

    if (g_blackHawkCollisionMesh.triangles.empty()) return;
    g_blackHawkCollisionMesh.boundsMin = minimum;
    g_blackHawkCollisionMesh.boundsMax = maximum;

    // Bucket triangles into the XZ grid so a lookup touches a handful instead
    // of all ~38k. Each triangle goes into every cell its XZ footprint covers.
    BlackHawkCollisionMesh& mesh = g_blackHawkCollisionMesh;
    const float spanX = maximum.x - minimum.x;
    const float spanZ = maximum.z - minimum.z;
    mesh.cellsX = (std::max)(1, int(spanX / mesh.kCellSize) + 1);
    mesh.cellsZ = (std::max)(1, int(spanZ / mesh.kCellSize) + 1);
    mesh.cells.assign(size_t(mesh.cellsX) * mesh.cellsZ, {});
    for (int index = 0; index < int(mesh.triangles.size()); ++index) {
        const BlackHawkCollisionMesh::Triangle& triangle = mesh.triangles[index];
        const float lowX = (std::min)({ triangle.a.x, triangle.b.x, triangle.c.x });
        const float highX = (std::max)({ triangle.a.x, triangle.b.x, triangle.c.x });
        const float lowZ = (std::min)({ triangle.a.z, triangle.b.z, triangle.c.z });
        const float highZ = (std::max)({ triangle.a.z, triangle.b.z, triangle.c.z });
        const int fromX = (std::max)(0,
            int((lowX - minimum.x) / mesh.kCellSize));
        const int toX = (std::min)(mesh.cellsX - 1,
            int((highX - minimum.x) / mesh.kCellSize));
        const int fromZ = (std::max)(0,
            int((lowZ - minimum.z) / mesh.kCellSize));
        const int toZ = (std::min)(mesh.cellsZ - 1,
            int((highZ - minimum.z) / mesh.kCellSize));
        for (int iz = fromZ; iz <= toZ; ++iz)
            for (int ix = fromX; ix <= toX; ++ix)
                mesh.cells[size_t(iz) * mesh.cellsX + ix].push_back(index);
    }
    mesh.valid = true;
    std::cout << "BlackHawk wreck collision: " << mesh.triangles.size()
              << " triangles in " << mesh.cellsX << "x" << mesh.cellsZ
              << " cells\n";
}

// Closest point on a triangle to a point, by barycentric region test.
static XMVECTOR ClosestPointOnTriangle(FXMVECTOR point, FXMVECTOR a,
                                       FXMVECTOR b, FXMVECTOR c) {
    const XMVECTOR ab = b - a;
    const XMVECTOR ac = c - a;
    const XMVECTOR ap = point - a;
    const float d1 = XMVectorGetX(XMVector3Dot(ab, ap));
    const float d2 = XMVectorGetX(XMVector3Dot(ac, ap));
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const XMVECTOR bp = point - b;
    const float d3 = XMVectorGetX(XMVector3Dot(ab, bp));
    const float d4 = XMVectorGetX(XMVector3Dot(ac, bp));
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        return a + ab * (d1 / (d1 - d3));

    const XMVECTOR cp = point - c;
    const float d5 = XMVectorGetX(XMVector3Dot(ab, cp));
    const float d6 = XMVectorGetX(XMVector3Dot(ac, cp));
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        return a + ac * (d2 / (d2 - d6));

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

    const float denominator = 1.0f / (va + vb + vc);
    return a + ab * (vb * denominator) + ac * (vc * denominator);
}

// One segment of the BlackHawk's collision hull, in the aircraft's own frame:
// a box spanning [forwardMin, forwardMax] along the nose axis, halfWidth to each
// side, and [heightMin, heightMax] vertically. Measured off the GLB's body
// materials (rotor and rotorhead excluded). Used while the bird is airborne,
// where the mesh moves every frame and baking it would be wasted work.
struct BlackHawkHullSegment {
    float forwardMin, forwardMax;
    float halfWidth;
    float heightMin, heightMax;
};

// The airframe is nothing like a box: the cabin bulges to ~2.0 m half-width, the
// tail boom pinches to ~0.55 m, and the stabilizer flares back out to ~2.5 m.
// A single box either swallowed the gap under the boom or fenced off open air
// beside it, which is obvious once a wreck is lying on the ground and walkable.
// These segments trace the real silhouette instead.
//
// The rotor disc is deliberately excluded: it sweeps 16 m and would fence the
// player out of the whole landing zone.
static constexpr BlackHawkHullSegment kBlackHawkHull[] = {
    // Tail fin and stabilizer: tall, narrow, flares wide at the very back.
    { -10.95f, -9.35f, 2.54f, 0.90f, 3.60f },
    // Tail boom: thin tube, sits high off the ground.
    {  -9.35f, -4.70f, 0.60f, 1.05f, 2.35f },
    // Rear fuselage stepping up into the cabin.
    {  -4.70f, -1.55f, 1.60f, 0.30f, 3.10f },
    // Cabin: the widest walkable mass.
    {  -1.55f,  1.60f, 1.75f, 0.10f, 3.30f },
    // Cockpit and nose, sponsons included.
    {   1.60f,  4.70f, 2.00f, 0.15f, 3.05f },
    // Nose cone tapering off.
    {   4.70f,  6.25f, 1.25f, 0.35f, 2.45f },
};

// Blocks the player against the BlackHawk's fuselage, segment by segment. Uses
// the full roll/pitch/yaw the airframe is drawn with, so a wreck lying on its
// side blocks along the shape it actually has rather than an upright box.
// Collides the player capsule against the baked wreck triangles. Runs only for
// a downed airframe, where the mesh is static and the bake stays valid.
static void ResolvePlayerBlackHawkMeshCollision(
        XMFLOAT3& position, float& floorY, float radius, float playerHeight) {
    const BlackHawkCollisionMesh& mesh = g_blackHawkCollisionMesh;
    if (!mesh.valid) return;

    // Broad phase against the wreck's bounds, expanded by the capsule.
    const float feet = position.y - playerHeight;
    if (position.x + radius < mesh.boundsMin.x ||
        position.x - radius > mesh.boundsMax.x ||
        position.z + radius < mesh.boundsMin.z ||
        position.z - radius > mesh.boundsMax.z ||
        position.y < mesh.boundsMin.y || feet > mesh.boundsMax.y) return;

    // The player is a vertical capsule from feet to eyes. Iterating a few times
    // settles contacts that push into one another, e.g. a corner of the hull.
    constexpr int kIterations = 3;
    constexpr float kStepHeight = 1.0f / 3.0f;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        bool moved = false;
        const float segmentBottom = position.y - playerHeight + radius;
        const float segmentTop = position.y - radius;

        // Gather the cells the capsule's footprint touches, deduplicated.
        static std::vector<int> candidates;
        candidates.clear();
        const int spanCells =
            int(radius / BlackHawkCollisionMesh::kCellSize) + 1;
        for (int oz = -spanCells; oz <= spanCells; ++oz) {
            for (int ox = -spanCells; ox <= spanCells; ++ox) {
                const std::vector<int>* cell = mesh.CellAt(
                    position.x + ox * BlackHawkCollisionMesh::kCellSize,
                    position.z + oz * BlackHawkCollisionMesh::kCellSize);
                if (cell) candidates.insert(candidates.end(),
                                            cell->begin(), cell->end());
            }
        }
        if (candidates.empty()) break;
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()),
                         candidates.end());

        for (int triangleIndex : candidates) {
            const BlackHawkCollisionMesh::Triangle& triangle =
                mesh.triangles[triangleIndex];
            // Clamp the capsule's axis to the triangle's height band, then test
            // the sphere at that height. Cheap stand-in for a full capsule test
            // and accurate enough at player scale.
            const float triangleMidY =
                (triangle.a.y + triangle.b.y + triangle.c.y) / 3.0f;
            const float sampleY =
                (std::max)(segmentBottom, (std::min)(segmentTop, triangleMidY));
            const XMVECTOR centre =
                XMVectorSet(position.x, sampleY, position.z, 0.0f);
            const XMVECTOR closest = ClosestPointOnTriangle(
                centre, XMLoadFloat3(&triangle.a), XMLoadFloat3(&triangle.b),
                XMLoadFloat3(&triangle.c));
            const XMVECTOR delta = centre - closest;
            const float distanceSq = XMVectorGetX(XMVector3LengthSq(delta));
            if (distanceSq >= radius * radius) continue;

            XMFLOAT3 contact{};
            XMStoreFloat3(&contact, closest);

            // Walkable surface underfoot: stand on it rather than be pushed.
            if (triangle.normal.y > 0.5f && contact.y <= feet + kStepHeight &&
                contact.y >= feet - 0.5f) {
                floorY = (std::max)(floorY, contact.y);
                continue;
            }

            const float distance = std::sqrt((std::max)(distanceSq, 0.0f));
            XMFLOAT3 push{};
            if (distance > 1e-4f) {
                XMStoreFloat3(&push, XMVector3Normalize(delta));
            } else {
                // Dead centre on the face: fall back to the surface normal.
                push = triangle.normal;
            }
            const float depth = radius - distance;
            position.x += push.x * depth;
            position.z += push.z * depth;
            moved = true;
        }
        if (!moved) break;
    }
}

static void ResolvePlayerBlackHawkCollision(
        XMFLOAT3& position, float& floorY, float radius, float playerHeight) {
    const VehicleSystem& vehicles = g_game.vehicles;
    if (!vehicles.blackHawkVisible || vehicles.blackHawkCarryingPlayer) return;

    // Down and static: collide against the real mesh instead of the boxes.
    if (vehicles.BlackHawkIsDown() && g_blackHawkCollisionMesh.valid) {
        ResolvePlayerBlackHawkMeshCollision(position, floorY, radius,
                                            playerHeight);
        return;
    }

    // Cheap reject: the hull never reaches beyond ~11 m from the origin.
    const float toX = position.x - vehicles.blackHawkPosition.x;
    const float toZ = position.z - vehicles.blackHawkPosition.z;
    const float toY = position.y - vehicles.blackHawkPosition.y;
    if (toX * toX + toY * toY + toZ * toZ > 14.0f * 14.0f) return;

    // Into the aircraft's frame. Inverse of the same roll/pitch/yaw the world
    // matrix applies, so the collider tracks the wreck's final attitude.
    const XMMATRIX orientation = XMMatrixRotationRollPitchYaw(
        vehicles.blackHawkPitch, vehicles.blackHawkYaw, vehicles.blackHawkRoll);
    const XMMATRIX toLocal = XMMatrixTranspose(orientation); // pure rotation
    XMFLOAT3 local{};
    XMStoreFloat3(&local, XMVector3TransformNormal(
        XMVectorSet(toX, toY, toZ, 0.0f), toLocal));

    // The player is a capsule; approximate it in local space as a sphere at the
    // body's mid height so a tilted wreck still blocks the whole body.
    const float halfBody = playerHeight * 0.5f;
    const float feetWorld = position.y - playerHeight;

    for (const BlackHawkHullSegment& segment : kBlackHawkHull) {
        // Nearest point on this segment's box, in local space.
        const float centreForward =
            (segment.forwardMin + segment.forwardMax) * 0.5f;
        const float halfForward =
            (segment.forwardMax - segment.forwardMin) * 0.5f;
        const float centreHeight =
            (segment.heightMin + segment.heightMax) * 0.5f;
        const float halfHeight =
            (segment.heightMax - segment.heightMin) * 0.5f;

        const float dForward = local.z - centreForward;
        const float dRight = local.x;
        const float dUp = local.y - centreHeight;

        const float overForward = halfForward + radius - std::abs(dForward);
        const float overRight = segment.halfWidth + radius - std::abs(dRight);
        const float overUp = halfHeight + halfBody - std::abs(dUp);
        if (overForward <= 0.0f || overRight <= 0.0f || overUp <= 0.0f) continue;

        // Standing on top of this segment: hold the player up instead of
        // shoving them out. Only meaningful while the hull is roughly level.
        constexpr float kStepHeight = 1.0f / 3.0f;
        const float segmentTopWorld =
            vehicles.blackHawkPosition.y + segment.heightMax;
        const bool level = std::abs(vehicles.blackHawkPitch) < 0.5f &&
                           std::abs(vehicles.blackHawkRoll) < 0.5f;
        if (level && segmentTopWorld <= feetWorld + kStepHeight &&
            segmentTopWorld >= feetWorld - 0.30f) {
            floorY = (std::max)(floorY, segmentTopWorld);
            continue;
        }

        // Push out along the least-penetrated axis so the player slides along
        // the hull rather than popping through or over it.
        XMFLOAT3 pushLocal{ 0.0f, 0.0f, 0.0f };
        if (overRight <= overForward && overRight <= overUp)
            pushLocal.x = dRight >= 0.0f ? overRight : -overRight;
        else if (overUp <= overForward)
            pushLocal.y = dUp >= 0.0f ? overUp : -overUp;
        else
            pushLocal.z = dForward >= 0.0f ? overForward : -overForward;

        XMFLOAT3 pushWorld{};
        XMStoreFloat3(&pushWorld, XMVector3TransformNormal(
            XMLoadFloat3(&pushLocal), orientation));
        position.x += pushWorld.x;
        position.z += pushWorld.z;
        // Vertical push only lifts; dropping the player is gravity's job.
        if (pushWorld.y > 0.0f)
            floorY = (std::max)(floorY, position.y - playerHeight + pushWorld.y);
    }
}

static void ResolvePlayerWorldObjectCollisions(
        XMFLOAT3& position, float& floorY, float radius,
        float playerHeight) {
    if (!scene.camera.FPSMode || g_emptyLevelMode) return;
    const float feet = position.y - playerHeight;
    constexpr float kStepHeight = 1.0f / 3.0f;

    // Explosive barrels use the same collision dimensions as bullet hit tests.
    for (ExplosiveBarrel& barrel : scene.explosiveBarrels) {
        if (!barrel.active || barrel.held) continue;
        constexpr float barrelRadius = 0.44f;
        constexpr float barrelHalfHeight = 0.78f;
        const float bottom = barrel.position.y - barrelHalfHeight;
        const float top = barrel.position.y + barrelHalfHeight;
        const float dx = position.x - barrel.position.x;
        const float dz = position.z - barrel.position.z;
        float distance = std::sqrt(dx * dx + dz * dz);
        const float reach = radius + barrelRadius;
        if (distance >= reach) continue;
        if (top <= feet + kStepHeight && top >= feet - 0.20f) {
            floorY = (std::max)(floorY, top);
            continue;
        }
        if (position.y <= bottom || feet >= top) continue;
        float normalX = 1.0f;
        float normalZ = 0.0f;
        if (distance > 0.0001f) {
            normalX = dx / distance;
            normalZ = dz / distance;
        } else {
            distance = 0.0f;
        }
        const float push = reach - distance + 0.002f;
        position.x += normalX * push;
        position.z += normalZ * push;
        if (barrel.thrown) {
            barrel.velocity.x -= normalX * 2.0f;
            barrel.velocity.z -= normalZ * 2.0f;
            if (barrel.physicsHandle != 0)
                g_destruction.SetExplosiveBarrelVelocity(
                    barrel.physicsHandle, barrel.velocity,
                    { 1.5f * normalZ, 2.5f, -1.5f * normalX });
        }
    }

    if (g_trees.IsInitialized())
        g_trees.ResolvePlayerCollision(
            position, floorY, radius, playerHeight);

    // Low oriented hull box matches projectile collision. Deck top becomes a
    // temporary floor when landing on it; sides push the player out in hull-local
    // space as the patrol boat turns.
    if (!g_boatModel || g_boatSunk) return;
    const float boatY = g_boatPosition.y - g_boatSinkDepth;
    const float top = boatY + kBoatDeckOffset;
    const float bottom = boatY - kBoatHullHeight;
    const float sinYaw = std::sin(g_boatYaw);
    const float cosYaw = std::cos(g_boatYaw);
    const float worldX = position.x - g_boatPosition.x;
    const float worldZ = position.z - g_boatPosition.z;
    float localX = worldX * cosYaw - worldZ * sinYaw;
    float localZ = worldX * sinYaw + worldZ * cosYaw;
    const float expandedBeam = kBoatDeckHalfBeam + radius;
    const float expandedLength = kBoatDeckHalfLength + radius;
    if (std::abs(localX) >= expandedBeam ||
        std::abs(localZ) >= expandedLength) return;
    if (top <= feet + kStepHeight && top >= feet - 0.25f) {
        floorY = (std::max)(floorY, top);
        return;
    }
    if (position.y <= bottom || feet >= top) return;
    const float penetrationX = expandedBeam - std::abs(localX);
    const float penetrationZ = expandedLength - std::abs(localZ);
    if (penetrationX < penetrationZ)
        localX = std::copysign(expandedBeam,
            std::abs(localX) > 0.001f ? localX : 1.0f);
    else
        localZ = std::copysign(expandedLength,
            std::abs(localZ) > 0.001f ? localZ : 1.0f);
    position.x = g_boatPosition.x + localX * cosYaw + localZ * sinYaw;
    position.z = g_boatPosition.z - localX * sinYaw + localZ * cosYaw;
}

// How far above the feet a walkable surface still counts as floor to step onto
// rather than a wall to be blocked by. Sized for a doorway sill and a kerb; much
// larger and the player climbs walls, much smaller and they catch on thresholds.
static constexpr float kPlayerStepHeight = 0.45f;

// How far below its feet a bandit still finds prefab ground. Covers the drop
// through a deck's own thickness plus a step down, without letting an actor
// that genuinely walked off a tower hang in the air for a frame.
static constexpr float kBanditPrefabSupportDrop = 0.9f;

// Highest walkable prefab surface under an actor, for standing on props the
// terrain heightfield knows nothing about -- a watchtower deck, a container
// roof, a walkway. Returns false when nothing supports the actor, leaving the
// caller on terrain height.
//
// Cast from a little above the feet so an actor already resting exactly on the
// surface still hits it, down to `maxDrop` below. Walkability is the surface
// normal, matching CollisionMeshResolveCapsule's floor test: a wall face
// registers a hit but must not read as ground.
//
// A raycast rather than a capsule resolve because only the floor height is
// wanted here; horizontal blocking for bandits stays with the existing box
// pushout.
static bool PrefabSurfaceSupports(const XMFLOAT3& position, float feetY,
                                  float maxDrop, float& surfaceY) {
    // Cosine of the steepest slope that still counts as standable (~46 deg),
    // the same threshold the capsule resolve uses for floorY.
    constexpr float kWalkableNormalY = 0.7f;
    constexpr float kProbeRise = 0.6f;
    const XMFLOAT3 start(position.x, feetY + kProbeRise, position.z);
    const XMFLOAT3 end(position.x, feetY - maxDrop, position.z);
    bool supported = false;
    float best = 0.0f;
    for (const CollisionMeshInstance& instance : g_prefabMeshColliders) {
        CollisionMeshRayHit hit;
        if (!CollisionMeshInstanceRaycast(instance, start, end, 0.0f, hit))
            continue;
        // Normals are pre-flipped to oppose the ray, so a floor under a
        // downward cast always reads +Y regardless of source winding.
        if (hit.normal.y < kWalkableNormalY) continue;
        if (!supported || hit.point.y > best) {
            best = hit.point.y;
            supported = true;
        }
    }
    if (supported) surfaceY = best;
    return supported;
}

static void ResolvePlayerPrefabCollisions(XMFLOAT3& position, float& floorY,
                                          float radius, float playerHeight) {
    if (!scene.camera.FPSMode) return;
    const float feet = position.y - playerHeight;

    // Per-triangle geometry first, so the player can walk into a hangar rather
    // than being stopped by the box that wraps the whole airport.
    for (const CollisionMeshInstance& instance : g_prefabMeshColliders) {
        const XMFLOAT3 base(position.x, feet, position.z);
        const CollisionMeshPushout pushout = CollisionMeshInstanceResolveCapsule(
            instance, base, radius, playerHeight, kPlayerStepHeight);
        if (!pushout.touched) continue;
        position.x += pushout.displacement.x;
        position.y += pushout.displacement.y;
        position.z += pushout.displacement.z;
        // Raising floorY is what stands the player on an interior floor; the
        // ground snap later in the same frame reads it.
        if (pushout.hasFloor) floorY = (std::max)(floorY, pushout.floorY);
    }

    for (const PrefabCollider& collider : g_prefabColliders) {
        // Entities with a triangle mesh were just resolved against it. Applying
        // the bounds box on top would immediately shove the player back out of
        // the building they just walked into.
        if (g_meshCollisionEntities.count(collider.entityId)) continue;
        if (feet >= collider.center.y + collider.halfExtents.y ||
            position.y <= collider.center.y - collider.halfExtents.y) continue;
        const float yaw = collider.yawRadians;
        XMFLOAT3 local = PrefabColliderToLocal(collider, position);
        float localX = local.x;
        float localZ = local.z;
        const float halfX = collider.halfExtents.x + radius;
        const float halfZ = collider.halfExtents.z + radius;
        if (std::abs(localX) >= halfX || std::abs(localZ) >= halfZ) continue;
        const float penetrationX = halfX - std::abs(localX);
        const float penetrationZ = halfZ - std::abs(localZ);
        if (penetrationX < penetrationZ)
            localX = std::copysign(halfX, std::abs(localX) > 0.001f ? localX : 1.0f);
        else
            localZ = std::copysign(halfZ, std::abs(localZ) > 0.001f ? localZ : 1.0f);
        const float worldCosine = std::cos(yaw);
        const float worldSine = std::sin(yaw);
        position.x = collider.center.x +
                     localX * worldCosine - localZ * worldSine;
        position.z = collider.center.z +
                     localX * worldSine + localZ * worldCosine;
    }
}

// Queues the player-collision volumes for the wireframe overlay.
//
// Deliberately reads the same collections ResolvePlayerPrefabCollisions walks,
// rather than re-deriving boxes from prefab bounds: a debug view that computes
// its own geometry can agree with the renderer while disagreeing with the
// collision system, which is exactly the bug it would be used to find.
void BuildCollisionDebugLines() {
    g_collisionDebugRenderer.Clear();
    if (!g_showCollisionDebug) return;

    for (const PrefabCollider& collider : g_prefabColliders) {
        // Amber for a box that is genuinely what gets queried; dim grey when a
        // triangle mesh supersedes it for segment tests, so a shot passing
        // "through" the grey box is expected rather than alarming.
        const bool superseded = g_meshCollisionEntities.count(collider.entityId) > 0;
        const XMFLOAT4 color = superseded ? XMFLOAT4(0.45f, 0.45f, 0.5f, 0.35f)
                                          : XMFLOAT4(1.0f, 0.72f, 0.2f, 0.9f);
        g_collisionDebugRenderer.AddBox(collider.center, collider.halfExtents,
                                        collider.yawRadians, color);
    }

    // Mesh colliders draw their world bounds in green -- the broadphase volume a
    // query must enter before any triangle is tested. The triangles themselves
    // are not drawn: at 835k for the airport alone they would bury the scene.
    for (const CollisionMeshInstance& instance : g_prefabMeshColliders) {
        g_collisionDebugRenderer.AddBounds(instance.worldBoundsMin,
                                           instance.worldBoundsMax,
                                           XMFLOAT4(0.3f, 1.0f, 0.45f, 0.8f));
    }
}

// Bullet penetration. The rule itself lives in BulletPenetration.h so it can be
// tested without a scene; this adapts it to the live projectile.
using SGE::kPenetrationCostFlesh;
using SGE::kPenetrationFalloffFlesh;
using SGE::kPenetrationCostSheet;
using SGE::kPenetrationFalloffSheet;

static bool TryPenetrate(Projectile& projectile, float cost, float falloff) {
    SGE::PenetrationState state{ projectile.penetrationPower,
                                 projectile.penetratedCount,
                                 projectile.damageMultiplier };
    if (!SGE::TryPenetrateState(state, cost, falloff)) return false;
    projectile.penetrationPower = state.power;
    projectile.penetratedCount = state.crossed;
    projectile.damageMultiplier = state.damage;
    return true;
}

static bool HitPrefabColliderSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                                     float radius, XMFLOAT3& hit,
                                     uint64_t* hitEntityId,
                                     XMFLOAT3* hitNormal) {
    float closestDistanceSquared = FLT_MAX;
    bool struck = false;
    for (const PrefabCollider& collider : g_prefabColliders) {
        // Skip the bounds box of anything that also has a triangle mesh. The box
        // always encloses the geometry, so its hit is at or before the real
        // surface and nearest-wins would let it shadow every mesh hit -- shots
        // would stop at an invisible plane across the hangar doorway.
        if (g_meshCollisionEntities.count(collider.entityId)) continue;
        XMFLOAT3 candidate;
        if (!PrefabColliderIntersectsSegment(collider, start, end, radius,
                                             &candidate)) continue;
        const float dx = candidate.x - start.x;
        const float dy = candidate.y - start.y;
        const float dz = candidate.z - start.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (distanceSquared < closestDistanceSquared) {
            closestDistanceSquared = distanceSquared;
            hit = candidate;
            if (hitEntityId) *hitEntityId = collider.entityId;
            // Box hits carry no surface normal. Leave the caller's value alone
            // so its existing fallback (reversed velocity, or the negated shot
            // direction) still applies.
            struck = true;
        }
    }
    for (const CollisionMeshInstance& instance : g_prefabMeshColliders) {
        CollisionMeshRayHit meshHit;
        if (!CollisionMeshInstanceRaycast(instance, start, end, radius, meshHit))
            continue;
        const float dx = meshHit.point.x - start.x;
        const float dy = meshHit.point.y - start.y;
        const float dz = meshHit.point.z - start.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (distanceSquared < closestDistanceSquared) {
            closestDistanceSquared = distanceSquared;
            hit = meshHit.point;
            if (hitEntityId) *hitEntityId = instance.entityId;
            if (hitNormal) *hitNormal = meshHit.normal;
            struck = true;
        }
    }
    return struck;
}

static XMFLOAT3 GrenadeFallbackNormal(const Projectile& grenade) {
    XMVECTOR velocity = XMLoadFloat3(&grenade.velocity);
    if (XMVectorGetX(XMVector3LengthSq(velocity)) < 1e-6f)
        return { 0.0f, 1.0f, 0.0f };
    XMFLOAT3 normal;
    XMStoreFloat3(&normal, -XMVector3Normalize(velocity));
    return normal;
}

static void SyncGrenadePhysicsBodies(bool advancePreviousPosition) {
    if (!scene.useDestruction || !g_destruction.IsInitialized()) return;
    for (Projectile& grenade : scene.projectiles) {
        if (!grenade.grenade) continue;
        if (grenade.held) {
            ReleaseGrenadePhysicsBody(grenade);
            continue;
        }
        if (grenade.grenadePhysicsHandle == 0 && grenade.active) {
            grenade.grenadePhysicsHandle = g_destruction.CreateGrenadeBody(
                grenade.position, grenade.velocity, grenade.molotov,
                scene.grenadeGravityScale);
        }
        if (grenade.grenadePhysicsHandle == 0) continue;
        DestructionBodyPose pose;
        if (!g_destruction.GetGrenadeBodyPose(
                grenade.grenadePhysicsHandle, pose)) {
            grenade.grenadePhysicsHandle = 0;
            if (!grenade.active) continue;
            grenade.grenadePhysicsHandle = g_destruction.CreateGrenadeBody(
                grenade.position, grenade.velocity, grenade.molotov,
                scene.grenadeGravityScale);
            if (grenade.grenadePhysicsHandle == 0 ||
                !g_destruction.GetGrenadeBodyPose(
                    grenade.grenadePhysicsHandle, pose)) continue;
        }
        if (advancePreviousPosition)
            grenade.previousPosition = grenade.position;
        grenade.position = pose.position;
        grenade.rotation = pose.rotation;
        grenade.velocity = pose.linearVelocity;
    }
}

// Swept grenade collision against every solid gameplay surface. Hit helpers
// already expand their shapes by radius, so fast throws cannot tunnel through
// thin walls between frames.
static bool HitGrenadeCollision(const Projectile& grenade, float radius,
                                XMFLOAT3& closestHit,
                                XMFLOAT3& closestNormal,
                                bool includePhysicsWorld) {
    const XMFLOAT3 start = grenade.previousPosition;
    const XMFLOAT3 end = grenade.position;
    const XMFLOAT3 fallback = GrenadeFallbackNormal(grenade);
    float closestDistanceSquared = FLT_MAX;
    bool struck = false;
    const auto accept = [&](const XMFLOAT3& hit, XMFLOAT3 normal) {
        const float dx = hit.x - start.x;
        const float dy = hit.y - start.y;
        const float dz = hit.z - start.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (distanceSquared >= closestDistanceSquared) return;
        XMVECTOR n = XMLoadFloat3(&normal);
        if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-6f)
            n = XMLoadFloat3(&fallback);
        XMStoreFloat3(&closestNormal, XMVector3Normalize(n));
        closestHit = hit;
        closestDistanceSquared = distanceSquared;
        struck = true;
    };
    const auto radialNormal = [&](const XMFLOAT3& hit,
                                  const XMFLOAT3& center) {
        XMFLOAT3 normal{ hit.x - center.x, hit.y - center.y,
                        hit.z - center.z };
        const XMVECTOR n = XMLoadFloat3(&normal);
        if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-6f) return fallback;
        XMStoreFloat3(&normal, XMVector3Normalize(n));
        return normal;
    };

    XMFLOAT3 candidate;
    if (grenade.grenadeCollisionGrace <= 0.0f && g_banditLoaded) {
        for (const auto& bandit : g_bandits) {
            if (bandit && bandit->BlocksProjectile(
                    start, end, radius, &candidate))
                accept(candidate, fallback);
        }
    }

    // Hostile grenades collide with player body after leaving thrower.
    if (grenade.hostile && grenade.grenadeCollisionGrace <= 0.0f) {
        const XMFLOAT3 playerCenter{
            scene.camera.Position.x, scene.camera.Position.y - 0.82f,
            scene.camera.Position.z };
        const XMVECTOR a = XMLoadFloat3(&start);
        const XMVECTOR ab = XMLoadFloat3(&end) - a;
        const XMVECTOR center = XMLoadFloat3(&playerCenter);
        const float lengthSquared = XMVectorGetX(XMVector3LengthSq(ab));
        float t = lengthSquared > 1e-6f
            ? XMVectorGetX(XMVector3Dot(center - a, ab)) / lengthSquared
            : 0.0f;
        t = (std::max)(0.0f, (std::min)(1.0f, t));
        XMVECTOR point = a + ab * t;
        constexpr float playerRadius = 0.58f;
        const float expanded = playerRadius + radius;
        if (XMVectorGetX(XMVector3LengthSq(point - center)) <=
            expanded * expanded) {
            XMStoreFloat3(&candidate, point);
            accept(candidate, radialNormal(candidate, playerCenter));
        }
    }

    if (HitHelicopterSegment(start, end, radius, candidate))
        accept(candidate, radialNormal(candidate, g_helicopterPosition));
    if (HitSecondaryHelicopterSegment(start, end, radius, candidate))
        accept(candidate, radialNormal(candidate, g_secondaryHelicopterPosition));
    if (HitBoatSegment(start, end, radius, candidate))
        accept(candidate, radialNormal(candidate, g_boatPosition));
    size_t hitTurretIndex = 0;
    if (HitAATurretSegment(start, end, radius, candidate, hitTurretIndex))
        accept(candidate, radialNormal(
            candidate, g_game.vehicles.aaTurrets[hitTurretIndex].position));

    size_t barrelIndex = 0;
    if (HitExplosiveBarrelSegment(
            start, end, radius, barrelIndex, candidate) &&
        (includePhysicsWorld ||
         scene.explosiveBarrels[barrelIndex].physicsHandle == 0))
        accept(candidate, radialNormal(
            candidate, scene.explosiveBarrels[barrelIndex].position));
    if (includePhysicsWorld &&
        g_destruction.HitTestSegment(start, end, radius, candidate))
        accept(candidate, fallback);
    if (g_trees.BlocksSegment(start, end, radius))
        accept(start, fallback);
    // Seeded with the reversed-velocity fallback: a bounds-box hit leaves it
    // untouched and behaves exactly as before, while a triangle hit overwrites
    // it with the real surface normal so the grenade bounces off a hangar wall
    // the way it does off terrain.
    XMFLOAT3 prefabNormal = fallback;
    if (HitPrefabColliderSegment(start, end, radius, candidate, nullptr,
                                 &prefabNormal))
        accept(candidate, prefabNormal);
    if (includePhysicsWorld &&
        HitTerrainSegment(start, end, radius, candidate)) {
        auto params = CurrentTerrainParams();
        params.heightScale = scene.terrainHeightScale;
        constexpr float sampleOffset = 0.20f;
        const float left = TerrainRendererDX12::HeightAt(
            params, candidate.x - sampleOffset, candidate.z);
        const float right = TerrainRendererDX12::HeightAt(
            params, candidate.x + sampleOffset, candidate.z);
        const float back = TerrainRendererDX12::HeightAt(
            params, candidate.x, candidate.z - sampleOffset);
        const float front = TerrainRendererDX12::HeightAt(
            params, candidate.x, candidate.z + sampleOffset);
        XMFLOAT3 terrainNormal{
            left - right, sampleOffset * 2.0f, back - front };
        accept(candidate, terrainNormal);
    }
    return struck;
}

static void BounceGrenade(Projectile& grenade, const XMFLOAT3& hit,
                          const XMFLOAT3& surfaceNormal) {
    XMVECTOR normal = XMVector3Normalize(XMLoadFloat3(&surfaceNormal));
    XMVECTOR velocity = XMLoadFloat3(&grenade.velocity);
    const float incomingSpeed = XMVectorGetX(XMVector3Dot(velocity, normal));
    if (incomingSpeed < 0.0f) {
        constexpr float restitution = 0.42f;
        constexpr float tangentRetention = 0.72f;
        velocity -= normal * ((1.0f + restitution) * incomingSpeed);
        const XMVECTOR normalVelocity =
            normal * XMVectorGetX(XMVector3Dot(velocity, normal));
        velocity = normalVelocity +
                   (velocity - normalVelocity) * tangentRetention;
        if (XMVectorGetX(XMVector3LengthSq(velocity)) < 0.04f)
            velocity = XMVectorZero();
        XMStoreFloat3(&grenade.velocity, velocity);
    }
    XMVECTOR corrected = XMLoadFloat3(&hit) + normal * 0.025f;
    XMStoreFloat3(&grenade.position, corrected);
}

// Small Environment Query System-style cover test. Candidate points must be
// reachable on the navmesh and hide both torso and head behind nearby world
// geometry. Only one enemy runs this search per frame; selected points cache.
static bool CoverRayBlockedNear(const XMFLOAT3& start, const XMFLOAT3& end,
                                float& blockerDistance) {
    blockerDistance = FLT_MAX;
    bool blocked = false;
    const auto accept = [&](const XMFLOAT3& hit) {
        const float dx = end.x - hit.x;
        const float dy = end.y - hit.y;
        const float dz = end.z - hit.z;
        blockerDistance = (std::min)(blockerDistance,
            std::sqrt(dx * dx + dy * dy + dz * dz));
        blocked = true;
    };

    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(start, end, 0.08f, hit))
        accept(hit);
    if (HitPrefabColliderSegment(start, end, 0.08f, hit)) accept(hit);
    if (HitTerrainSegment(start, end, 0.08f, hit)) accept(hit);
    if (g_trees.BlocksSegment(start, end, 0.08f)) {
        blocked = true;
        blockerDistance = (std::min)(blockerDistance, 2.5f);
    }
    return blocked && blockerDistance <= 4.75f;
}

static bool QueryBanditCover(const SkinnedEnemy& bandit,
                             const XMFLOAT3& playerPosition,
                             XMFLOAT3& bestPosition) {
    if (!g_navigation.Ready()) return false;

    float bestScore = -FLT_MAX;
    bool found = false;
    const float fromPlayerX = bandit.position.x - playerPosition.x;
    const float fromPlayerZ = bandit.position.z - playerPosition.z;
    const float fromPlayerLength = std::sqrt(
        fromPlayerX * fromPlayerX + fromPlayerZ * fromPlayerZ);
    const float awayX = fromPlayerLength > 0.001f
        ? fromPlayerX / fromPlayerLength : 1.0f;
    const float awayZ = fromPlayerLength > 0.001f
        ? fromPlayerZ / fromPlayerLength : 0.0f;
    const float angleOffset = static_cast<float>(bandit.spawnSlot + 1) * 0.731f;
    constexpr float rings[] = { 3.5f, 6.5f, 9.5f };
    constexpr int directions = 12;

    auto terrainParams = CurrentTerrainParams();
    terrainParams.heightScale = scene.terrainHeightScale;
    for (float ring : rings) {
        for (int direction = 0; direction < directions; ++direction) {
            const float angle = angleOffset + XM_2PI *
                static_cast<float>(direction) / static_cast<float>(directions);
            XMFLOAT3 requested{
                bandit.position.x + std::sin(angle) * ring,
                bandit.position.y,
                bandit.position.z + std::cos(angle) * ring };
            if (scene.useMeshTerrain && g_terrain.supported)
                requested.y = TerrainRendererDX12::HeightAt(
                    terrainParams, requested.x, requested.z);

            XMFLOAT3 torso{ requested.x, requested.y + 1.12f, requested.z };
            XMFLOAT3 head{ requested.x, requested.y + 1.72f, requested.z };
            float torsoBlocker = FLT_MAX, headBlocker = FLT_MAX;
            if (!CoverRayBlockedNear(playerPosition, torso, torsoBlocker) ||
                !CoverRayBlockedNear(playerPosition, head, headBlocker))
                continue;

            std::vector<XMFLOAT3> path;
            if (!g_navigation.FindPath(bandit.position, requested, path) ||
                path.empty())
                continue;
            XMFLOAT3 candidate = path.back();
            const float snapX = candidate.x - requested.x;
            const float snapZ = candidate.z - requested.z;
            if (snapX * snapX + snapZ * snapZ > 1.8f * 1.8f) continue;

            float pathLength = 0.0f;
            XMFLOAT3 previous = bandit.position;
            for (const XMFLOAT3& point : path) {
                const float px = point.x - previous.x;
                const float pz = point.z - previous.z;
                pathLength += std::sqrt(px * px + pz * pz);
                previous = point;
            }
            const float playerDx = candidate.x - playerPosition.x;
            const float playerDz = candidate.z - playerPosition.z;
            const float playerDistance = std::sqrt(
                playerDx * playerDx + playerDz * playerDz);
            if (playerDistance < 5.0f) continue;

            const float moveDx = candidate.x - bandit.position.x;
            const float moveDz = candidate.z - bandit.position.z;
            const float moveLength = std::sqrt(moveDx * moveDx + moveDz * moveDz);
            const float retreatAlignment = moveLength > 0.001f
                ? (moveDx * awayX + moveDz * awayZ) / moveLength : 0.0f;
            float crowdPenalty = 0.0f;
            bool occupied = false;
            for (const auto& other : g_bandits) {
                if (!other || other.get() == &bandit || other->Dead()) continue;
                const float ox = candidate.x - other->position.x;
                const float oz = candidate.z - other->position.z;
                const float separation = std::sqrt(ox * ox + oz * oz);
                if (separation < 1.25f) { occupied = true; break; }
                if (separation < 3.0f) crowdPenalty += (3.0f - separation) * 2.2f;
            }
            if (occupied) continue;

            const float desiredRange = bandit.health <= 40.0f ? 17.0f : 12.0f;
            const float score = playerDistance * 0.38f - pathLength * 0.82f +
                retreatAlignment * 3.2f -
                std::abs(playerDistance - desiredRange) * 0.16f -
                (torsoBlocker + headBlocker) * 0.12f - crowdPenalty;
            if (score <= bestScore) continue;
            bestScore = score;
            bestPosition = candidate;
            found = true;
        }
    }
    return found;
}
