#include "PrefabRuntime.h"

#include <fstream>
#include <iostream>
#include <limits>

using namespace DirectX;
using nlohmann::json;

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

static XMFLOAT3 ToWorld(const ShootingTargetInstance& target, XMFLOAT3 local) {
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3TransformCoord(XMLoadFloat3(&local),
        XMLoadFloat4x4(&target.localToWorld)));
    return result;
}

static CollisionMesh ReadTargetMesh(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    uint32_t header[5]{};
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    CHECK(header[0] == 0x46546c67 && header[1] == 2 && header[4] == 0x4e4f534a);
    std::string text(header[3], '\0');
    stream.read(text.data(), text.size());
    const json gltf = json::parse(text);
    uint32_t binaryHeader[2]{};
    stream.read(reinterpret_cast<char*>(binaryHeader), sizeof(binaryHeader));
    CHECK(binaryHeader[1] == 0x004e4942);
    const auto binaryStart = stream.tellg();
    const auto primitive = gltf.at("meshes")[0].at("primitives")[0];
    const auto readAccessor = [&](int index, auto& values) {
        const auto& accessor = gltf.at("accessors")[index];
        const auto& view = gltf.at("bufferViews")[accessor.at("bufferView").get<int>()];
        values.resize(accessor.at("count").get<size_t>());
        stream.seekg(binaryStart + static_cast<std::streamoff>(
            view.value("byteOffset", 0u) + accessor.value("byteOffset", 0u)));
        stream.read(reinterpret_cast<char*>(values.data()),
                    values.size() * sizeof(values[0]));
        CHECK(stream.good());
    };
    std::vector<XMFLOAT3> positions;
    std::vector<uint32_t> indices;
    readAccessor(primitive.at("attributes").at("POSITION").get<int>(), positions);
    const int indexAccessor = primitive.at("indices").get<int>();
    CHECK(gltf.at("accessors")[indexAccessor].at("componentType") == 5125);
    readAccessor(indexAccessor, indices);
    std::vector<float> soup;
    for (uint32_t index : indices) {
        const auto& point = positions.at(index);
        soup.insert(soup.end(), { point.x, point.y, point.z });
    }
    CollisionMesh mesh;
    CHECK(BuildCollisionMesh(std::move(soup), mesh));
    CHECK(mesh.TriangleCount() == 36);
    return mesh;
}

int main() {
    std::ifstream source(std::string(SGE_SOURCE_DIR) +
        "/Content/Prefabs/Props/shooting_target_01.json");
    CHECK(source.good());
    if (!source.good()) return 1;
    json prefab;
    source >> prefab;
    const json settings = prefab.at("components").at("shootingTarget");
    ShootingTargetInstance target;
    target.entityId = 17;
    target.definition = ReadShootingTarget(settings);
    CHECK(target.definition.centers[0].name == "HEAD");
    CHECK(target.definition.centers[1].name == "TORSO");
    const auto mesh = ReadTargetMesh(std::string(SGE_SOURCE_DIR) + "/" +
        prefab.at("components").at("staticMesh").at("path").get<std::string>());

    // Use the actual authored centers, including the head's small X offset.
    // Nonuniform scale and full rotation exercise the face-plane conversion.
    const XMMATRIX transforms[] = {
        XMMatrixIdentity(),
        XMMatrixScaling(2.0f, 0.5f, 3.0f) *
            XMMatrixRotationRollPitchYaw(0.3f, 1.2f, -0.4f) *
            XMMatrixTranslation(13.0f, 8.0f, -21.0f),
        XMMatrixScaling(-1.5f, 2.0f, 1.0f) * XMMatrixRotationY(2.8f)
    };
    for (const XMMATRIX& transform : transforms) {
        XMStoreFloat4x4(&target.localToWorld, transform);
        CollisionMeshInstance collision;
        InitializeCollisionMeshInstance(collision, mesh, transform);
        for (const auto& center : target.definition.centers) {
            const XMFLOAT3 bullseye(center.position.x, center.position.y,
                                     target.definition.faceZ);
            const auto hit = ScoreShootingTarget(target, ToWorld(target, bullseye));
            CHECK(hit.points == 10);
            CHECK(hit.center == center.name);
            const auto start = ToWorld(target,
                { center.position.x, center.position.y, 2.0f });
            const auto end = ToWorld(target,
                { center.position.x, center.position.y, -2.0f });
            CollisionMeshRayHit impact;
            CHECK(CollisionMeshInstanceRaycast(collision, start, end, 0.0f, impact));
            CHECK(ScoreShootingTarget(target, impact.point).points == 10);
            CHECK(ScoreShootingTarget(target, impact.point).center == center.name);
            const auto shortEnd = ToWorld(target,
                { center.position.x, center.position.y, target.definition.faceZ + 0.05f });
            CHECK(!CollisionMeshInstanceRaycast(collision, start, shortEnd, 0.0f, impact));
            CHECK(CollisionMeshInstanceRaycast(collision, end, start, 0.0f, impact));
            CHECK(ScoreShootingTarget(target, impact.point).points == 0);
            for (const auto& ring : target.definition.rings) {
                XMFLOAT3 point = bullseye;
                point.x += center.radii.x * ring.radius;
                const auto edge = ScoreShootingTarget(target, ToWorld(target, point));
                if (edge.points != ring.points) {
                    XMFLOAT3 recovered;
                    const auto worldPoint = ToWorld(target, point);
                    XMStoreFloat3(&recovered, XMVector3TransformCoord(XMLoadFloat3(&worldPoint),
                        XMMatrixInverse(nullptr, transform)));
                    const float dx = (recovered.x - center.position.x) / center.radii.x;
                    const float dy = (recovered.y - center.position.y) / center.radii.y;
                    std::cerr << center.name << " radius " << ring.radius << " got " << edge.points
                        << " expected " << ring.points << " squared-distance error "
                        << dx * dx + dy * dy - ring.radius * ring.radius << '\n';
                }
                CHECK(edge.points == ring.points);
                point.x += center.radii.x * 0.002f;
                CHECK(ScoreShootingTarget(target, ToWorld(target, point)).points < ring.points);
            }
            XMFLOAT3 outside = bullseye;
            outside.y += center.radii.y * 1.02f;
            CHECK(ScoreShootingTarget(target, ToWorld(target, outside)).points == 0);
            XMFLOAT3 back = bullseye;
            back.z = -target.definition.faceZ;
            CHECK(ScoreShootingTarget(target, ToWorld(target, back)).points == 0);
        }
    }
    XMStoreFloat4x4(&target.localToWorld, XMMatrixScaling(0, 1, 1));
    CHECK(ScoreShootingTarget(target, {}).points == 0);
    XMStoreFloat4x4(&target.localToWorld, XMMatrixIdentity());
    CHECK(ScoreShootingTarget(target,
        { std::numeric_limits<float>::quiet_NaN(), 0, 0 }).points == 0);

    const auto rejects = [&](json invalid) {
        try { ReadShootingTarget(invalid); return false; }
        catch (const std::exception&) { return true; }
    };
    json invalid = settings;
    invalid["centers"].erase(1);
    CHECK(rejects(invalid));
    invalid = settings;
    invalid["centers"][0]["radii"][0] = 0;
    CHECK(rejects(invalid));
    invalid = settings;
    invalid["rings"][1]["radius"] = 0.1;
    CHECK(rejects(invalid));
    invalid = settings;
    invalid["rings"][1]["points"] = 11;
    CHECK(rejects(invalid));
    invalid = settings;
    invalid["rings"][0]["points"] = -1;
    CHECK(rejects(invalid));

    PrefabRuntimeState state;
    state.shootingTargets.push_back(target);
    state.shootingRange.Record(17, { 10, "HEAD" });
    state.shootingRange.Record(17, { 8, "TORSO" });
    state.shootingRange.Record(18, { 7, "TORSO" });
    state.shootingRange.Record(18, {});
    CHECK(state.shootingRange.totalPoints == 25);
    CHECK(state.shootingRange.hits == 3);
    CHECK(state.shootingRange.targetPoints.at(17) == 18);
    CHECK(state.shootingRange.lastTargetPoints == 7);
    CHECK(state.shootingRange.lastHit.points == 0);
    CHECK(state.shootingRange.feedbackSeconds == 2.0f);

    const XMMATRIX delta = XMMatrixRotationZ(0.7f) * XMMatrixTranslation(5, 2, 9);
    const auto update = ApplyPrefabEntityTransformDelta(state, 17, delta);
    CHECK(update.shootingTargets == 1);
    const auto& moved = state.shootingTargets[0];
    const auto& head = moved.definition.centers[0];
    CHECK(ScoreShootingTarget(moved, ToWorld(moved,
        { head.position.x, head.position.y, moved.definition.faceZ })).points == 10);
    state.ClearDerived();
    CHECK(state.shootingTargets.empty());
    CHECK(state.shootingRange.totalPoints == 25);
    state.ResetGameplayState();
    CHECK(state.shootingRange.totalPoints == 0);
    CHECK(state.shootingRange.hits == 0);
    CHECK(state.shootingRange.targetPoints.empty());
    CHECK(state.shootingRange.feedbackSeconds == 0);
    return failures ? 1 : 0;
}
