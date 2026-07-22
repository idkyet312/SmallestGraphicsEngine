#ifndef PREFAB_COLLIDERS_H
#define PREFAB_COLLIDERS_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

struct PrefabCollider {
    uint64_t entityId = 0;
    std::string prefabId;
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 halfExtents{ 0.5f, 0.5f, 0.5f };
    float yawRadians = 0.0f;
};

extern std::vector<PrefabCollider> g_prefabColliders;

inline DirectX::XMFLOAT3 PrefabColliderToLocal(
    const PrefabCollider& collider, const DirectX::XMFLOAT3& point) {
    const float cosine = std::cos(-collider.yawRadians);
    const float sine = std::sin(-collider.yawRadians);
    const float x = point.x - collider.center.x;
    const float z = point.z - collider.center.z;
    return { x * cosine - z * sine, point.y - collider.center.y,
             x * sine + z * cosine };
}

inline bool PrefabColliderIntersectsSegment(
    const PrefabCollider& collider, const DirectX::XMFLOAT3& start,
    const DirectX::XMFLOAT3& end, float radius,
    DirectX::XMFLOAT3* hitPoint = nullptr) {
    const DirectX::XMFLOAT3 a = PrefabColliderToLocal(collider, start);
    const DirectX::XMFLOAT3 b = PrefabColliderToLocal(collider, end);
    const float origin[3] = { a.x, a.y, a.z };
    const float delta[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    const float half[3] = { collider.halfExtents.x + radius,
                            collider.halfExtents.y + radius,
                            collider.halfExtents.z + radius };
    float entry = 0.0f;
    float exit = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(delta[axis]) < 1e-6f) {
            if (origin[axis] < -half[axis] || origin[axis] > half[axis])
                return false;
            continue;
        }
        float nearTime = (-half[axis] - origin[axis]) / delta[axis];
        float farTime = (half[axis] - origin[axis]) / delta[axis];
        if (nearTime > farTime) std::swap(nearTime, farTime);
        entry = (std::max)(entry, nearTime);
        exit = (std::min)(exit, farTime);
        if (entry > exit) return false;
    }
    if (hitPoint) {
        hitPoint->x = start.x + (end.x - start.x) * entry;
        hitPoint->y = start.y + (end.y - start.y) * entry;
        hitPoint->z = start.z + (end.z - start.z) * entry;
    }
    return true;
}

#endif
