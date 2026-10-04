#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static CollisionMesh g_humveeHullCollision;
static CollisionMesh g_humveeTurretCollision;

static void BuildHumveeProjectileCollision() {
    g_humveeHullCollision = {};
    g_humveeTurretCollision = {};
    if (!g_humveeModel) return;
    std::vector<float> triangles;
    if (ExtractPrefabCollisionTriangles(g_humveeModel, triangles, nullptr,
                                        g_humveeTurretNode.get()))
        BuildCollisionMesh(std::move(triangles), g_humveeHullCollision);
    if (g_humveeTurretNode &&
        ExtractPrefabCollisionTriangles(g_humveeTurretNode, triangles)) {
        // Store the gun in its own frame so every vehicle's traverse can share
        // the same tree without freezing collision at the imported gun angle.
        const XMMATRIX inverse = XMMatrixInverse(nullptr,
            XMLoadFloat4x4(&g_humveeTurretNode->globalTransform));
        for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
            XMFLOAT3 point{ triangles[i], triangles[i + 1], triangles[i + 2] };
            XMStoreFloat3(&point,
                XMVector3TransformCoord(XMLoadFloat3(&point), inverse));
            triangles[i] = point.x;
            triangles[i + 1] = point.y;
            triangles[i + 2] = point.z;
        }
        BuildCollisionMesh(std::move(triangles), g_humveeTurretCollision);
    }
}

static bool HitHumveeSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                              float radius, CollisionMeshRayHit& result,
                              size_t ignoredVehicle = kNoHumvee) {
    if (!g_humveeModel || !g_levelPlacesHumvee || g_trainingRangeMode ||
        g_emptyLevelMode) return false;
    bool found = false;
    for (size_t index = 0; index < LevelHumveeCount(); ++index) {
        if (index == ignoredVehicle || !HumveeAlive(index)) continue;
        const XMMATRIX world = HumveeWorldMatrix(index);
        const auto consider = [&](const CollisionMesh& mesh, FXMMATRIX frame) {
            if (mesh.Empty()) return;
            CollisionMeshInstance instance;
            InitializeCollisionMeshInstance(instance, mesh, frame);
            CollisionMeshRayHit candidate;
            if (CollisionMeshInstanceRaycast(instance, start, end, radius,
                                              candidate) &&
                (!found || candidate.t < result.t)) {
                result = candidate;
                found = true;
            }
        };
        consider(g_humveeHullCollision, world);
        if (g_humveeTurretNode && !g_humveeTurretCollision.Empty()) {
            const float yaw = index < g_humveeGameplay.size()
                ? g_humveeGameplay[index].turretYaw : 0.0f;
            const XMMATRIX gun = XMMatrixAffineTransformation(
                XMLoadFloat3(&g_humveeTurretNode->scale), XMVectorZero(),
                XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), yaw),
                XMLoadFloat3(&g_humveeTurretNode->translation)) *
                XMLoadFloat4x4(&g_humveeModel->globalTransform);
            consider(g_humveeTurretCollision, gun * world);
        }
    }
    return found;
}
