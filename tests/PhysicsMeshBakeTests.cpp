// The physics mesh bake (DestructionDX12's StaticMeshRegistry) saves the
// b3MeshData blob b3CreateMesh returns and later hands Box3D a byte copy of it
// at a different address. That only works if the blob is position independent:
// offsets, no pointers. This checks the assumption directly -- a copy, with the
// original destroyed, must ray cast and collide exactly like the original.
#include <box3d/box3d.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <malloc.h>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    if (ok) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

// A bumpy unindexed triangle soup, built the way the engine feeds collision
// meshes in: three vertices per triangle, welded by Box3D.
b3MeshData* BuildSoupMesh() {
    constexpr int kCells = 24;
    constexpr float kCell = 1.0f;
    const auto height = [](int x, int z) {
        return 0.25f * std::sin(x * 0.7f) * std::cos(z * 0.5f);
    };
    std::vector<b3Vec3> vertices;
    for (int z = 0; z < kCells; ++z) {
        for (int x = 0; x < kCells; ++x) {
            const float x0 = (x - kCells / 2) * kCell, x1 = x0 + kCell;
            const float z0 = (z - kCells / 2) * kCell, z1 = z0 + kCell;
            const b3Vec3 a{ x0, height(x, z), z0 };
            const b3Vec3 b{ x1, height(x + 1, z), z0 };
            const b3Vec3 c{ x0, height(x, z + 1), z1 };
            const b3Vec3 d{ x1, height(x + 1, z + 1), z1 };
            vertices.insert(vertices.end(), { a, c, b, b, c, d });
        }
    }
    std::vector<int32_t> indices(vertices.size());
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<int32_t>(i);
    b3MeshDef def = {};
    def.vertices = vertices.data();
    def.indices = indices.data();
    def.vertexCount = static_cast<int>(vertices.size());
    def.triangleCount = static_cast<int>(vertices.size() / 3);
    def.weldVertices = true;
    def.weldTolerance = 0.001f;
    def.identifyEdges = true;
    return b3CreateMesh(&def, nullptr, 0);
}

float DropSphere(const b3MeshData* mesh) {
    b3WorldDef worldDef = b3DefaultWorldDef();
    worldDef.gravity = { 0.0f, -9.81f, 0.0f };
    const b3WorldId world = b3CreateWorld(&worldDef);
    b3BodyDef groundDef = b3DefaultBodyDef();
    const b3BodyId ground = b3CreateBody(world, &groundDef);
    b3ShapeDef shapeDef = b3DefaultShapeDef();
    b3CreateMeshShape(ground, &shapeDef, mesh, { 1.0f, 1.0f, 1.0f });

    b3BodyDef ballDef = b3DefaultBodyDef();
    ballDef.type = b3_dynamicBody;
    ballDef.position = { 0.3f, 3.0f, -0.4f };
    const b3BodyId ball = b3CreateBody(world, &ballDef);
    const b3Sphere sphere{ { 0.0f, 0.0f, 0.0f }, 0.5f };
    b3CreateSphereShape(ball, &shapeDef, &sphere);
    for (int i = 0; i < 180; ++i) b3World_Step(world, 1.0f / 60.0f, 4);
    const float y = static_cast<float>(b3Body_GetPosition(ball).y);
    b3DestroyWorld(world);
    return y;
}

b3CastOutput CastDown(const b3MeshData* mesh, float x, float z) {
    const b3Mesh shape{ mesh, { 1.0f, 1.0f, 1.0f } };
    b3RayCastInput input{};
    input.origin = { x, 5.0f, z };
    input.translation = { 0.0f, -10.0f, 0.0f };
    input.maxFraction = 1.0f;
    return b3RayCastMesh(&shape, &input);
}
}  // namespace

int main() {
    b3MeshData* original = BuildSoupMesh();
    Check(original != nullptr, "b3CreateMesh built the soup");
    if (!original) return 1;
    Check(original->version == B3_MESH_VERSION, "blob carries the mesh version");

    const float originalRest = DropSphere(original);
    const b3CastOutput originalHit = CastDown(original, 2.3f, -3.1f);

    // Relocate exactly as the bake does, then destroy the original so any
    // pointer into it would dangle.
    const int bytes = original->byteCount;
    auto* copy = static_cast<b3MeshData*>(_aligned_malloc(bytes, 32));
    std::memcpy(copy, original, bytes);
    b3DestroyMesh(original);

    const b3CastOutput copyHit = CastDown(copy, 2.3f, -3.1f);
    Check(originalHit.hit && copyHit.hit, "ray hits original and copy");
    Check(std::abs(copyHit.fraction - originalHit.fraction) < 1e-6f &&
          copyHit.triangleIndex == originalHit.triangleIndex,
          "copy ray cast matches original");

    const float copyRest = DropSphere(copy);
    Check(originalRest > 0.2f && originalRest < 1.0f,
          "sphere rests on the original mesh");
    Check(std::abs(copyRest - originalRest) < 1e-4f,
          "sphere rests identically on the copy");

    _aligned_free(copy);
    if (failures == 0) std::cout << "PhysicsMeshBakeTests passed\n";
    return failures == 0 ? 0 : 1;
}
