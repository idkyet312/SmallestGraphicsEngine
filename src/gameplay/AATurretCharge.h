#ifndef SGE_AA_TURRET_CHARGE_H
#define SGE_AA_TURRET_CHARGE_H

#include "ChargeAnchor.h"
#include "VehicleSystem.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace SGE {

inline DirectX::XMMATRIX AATurretChargeMatrix(
        const VehicleSystem::AATurret& turret, ChargeAnchorPart part) {
    using namespace DirectX;
    const XMMATRIX base = XMMatrixTranslation(
        turret.position.x, turret.position.y, turret.position.z);
    if (part == ChargeAnchorPart::AATurretBase) return base;
    const XMMATRIX pivot = XMMatrixTranslation(
        turret.position.x,
        turret.position.y + VehicleSystem::AATurretMountHeight,
        turret.position.z);
    const XMMATRIX traverse = XMMatrixRotationY(turret.yaw);
    return part == ChargeAnchorPart::AATurretElevation
        ? XMMatrixRotationX(-turret.pitch) * traverse * pivot
        : traverse * pivot;
}

inline void ResolveAATurretCharge(const VehicleSystem::AATurret& turret,
                                  ChargeAnchorPart part,
                                  const DirectX::XMFLOAT3& localPosition,
                                  const DirectX::XMFLOAT3& localNormal,
                                  DirectX::XMFLOAT3& worldPosition,
                                  DirectX::XMFLOAT3& worldNormal) {
    using namespace DirectX;
    const XMMATRIX frame = AATurretChargeMatrix(turret, part);
    XMStoreFloat3(&worldPosition,
                  XMVector3TransformCoord(XMLoadFloat3(&localPosition), frame));
    XMStoreFloat3(&worldNormal, XMVector3Normalize(
        XMVector3TransformNormal(XMLoadFloat3(&localNormal), frame)));
}

struct AATurretChargeHit {
    ChargeAnchorPart part = ChargeAnchorPart::World;
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 normal{};
    DirectX::XMFLOAT3 localPosition{};
    DirectX::XMFLOAT3 localNormal{};
    float fraction = FLT_MAX;
};

inline bool AATurretChargeIsNearer(const DirectX::XMFLOAT3& start,
                                  const DirectX::XMFLOAT3& turretHit,
                                  const DirectX::XMFLOAT3& worldHit) {
    const auto distanceSq = [&start](const DirectX::XMFLOAT3& point) {
        const float dx = point.x - start.x;
        const float dy = point.y - start.y;
        const float dz = point.z - start.z;
        return dx * dx + dy * dy + dz * dz;
    };
    return distanceSq(turretHit) < distanceSq(worldHit);
}

// The three proxies follow the same Base/Gun/Elevation frames as the drawn
// turret. The bounds come from the built-in model; the authored model uses the
// same mount height and barrel length. Sweeping expanded boxes keeps a fast C4
// throw from tunnelling through a narrow barrel between frames.
inline bool HitAATurretChargeSegment(
        const VehicleSystem::AATurret& turret,
        const DirectX::XMFLOAT3& start,
        const DirectX::XMFLOAT3& end, float radius,
        AATurretChargeHit& result) {
    using namespace DirectX;
    if (!turret.Active()) return false;
    struct Box {
        ChargeAnchorPart part;
        XMFLOAT3 low, high;
    };
    static const Box boxes[] = {
        { ChargeAnchorPart::AATurretBase, {-1.65f, 0.0f, -1.65f}, {1.65f, 0.30f, 1.65f} },
        { ChargeAnchorPart::AATurretBase, {-2.15f, 0.05f, -0.35f}, {2.15f, 0.30f, 0.35f} },
        { ChargeAnchorPart::AATurretBase, {-0.35f, 0.05f, -2.15f}, {0.35f, 0.30f, 2.15f} },
        { ChargeAnchorPart::AATurretBase, {-0.70f, 0.22f, -0.70f}, {0.70f, 1.55f, 0.70f} },
        { ChargeAnchorPart::AATurretBase, {-1.35f, 0.22f, -0.45f}, {-0.75f, 0.85f, 0.45f} },
        { ChargeAnchorPart::AATurretBase, {0.75f, 0.22f, -0.45f}, {1.35f, 0.85f, 0.45f} },
        { ChargeAnchorPart::AATurretTraverse, {-0.55f, -0.32f, -0.55f}, {0.55f, 0.34f, 0.62f} },
        { ChargeAnchorPart::AATurretTraverse, {-0.95f, -0.30f, -0.72f}, {0.95f, 0.78f, -0.55f} },
        { ChargeAnchorPart::AATurretElevation, {-0.40f, -0.16f, 0.55f}, {-0.08f, 0.16f, VehicleSystem::AATurretBarrelLength} },
        { ChargeAnchorPart::AATurretElevation, {0.08f, -0.16f, 0.55f}, {0.40f, 0.16f, VehicleSystem::AATurretBarrelLength} },
    };

    bool found = false;
    for (const Box& box : boxes) {
        const XMMATRIX frame = AATurretChargeMatrix(turret, box.part);
        XMVECTOR determinant;
        const XMMATRIX inverse = XMMatrixInverse(&determinant, frame);
        XMFLOAT3 a, b;
        XMStoreFloat3(&a, XMVector3TransformCoord(XMLoadFloat3(&start), inverse));
        XMStoreFloat3(&b, XMVector3TransformCoord(XMLoadFloat3(&end), inverse));
        const float origin[3] = { a.x, a.y, a.z };
        const float delta[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
        const float lower[3] = { box.low.x - radius, box.low.y - radius,
                                 box.low.z - radius };
        const float upper[3] = { box.high.x + radius, box.high.y + radius,
                                 box.high.z + radius };
        float enter = 0.0f, leave = 1.0f;
        XMFLOAT3 localNormal{ 0.0f, 0.0f, 0.0f };
        bool missed = false;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(delta[axis]) < 1e-6f) {
                if (origin[axis] < lower[axis] || origin[axis] > upper[axis])
                    missed = true;
                continue;
            }
            float nearTime = (lower[axis] - origin[axis]) / delta[axis];
            float farTime = (upper[axis] - origin[axis]) / delta[axis];
            float sign = -1.0f;
            if (nearTime > farTime) {
                std::swap(nearTime, farTime);
                sign = 1.0f;
            }
            if (nearTime > enter) {
                enter = nearTime;
                localNormal = { 0.0f, 0.0f, 0.0f };
                (&localNormal.x)[axis] = sign;
            }
            leave = (std::min)(leave, farTime);
            if (enter > leave) missed = true;
        }
        if (missed || enter >= result.fraction || enter > 1.0f) continue;
        if (localNormal.x == 0.0f && localNormal.y == 0.0f &&
            localNormal.z == 0.0f) {
            const XMVECTOR reverse = XMVectorSet(-delta[0], -delta[1],
                                                  -delta[2], 0.0f);
            if (XMVectorGetX(XMVector3LengthSq(reverse)) < 1e-6f) continue;
            XMStoreFloat3(&localNormal, XMVector3Normalize(reverse));
        }
        const XMVECTOR center = XMLoadFloat3(&a) +
            (XMLoadFloat3(&b) - XMLoadFloat3(&a)) * enter;
        const XMVECTOR surface = center - XMLoadFloat3(&localNormal) * radius;
        XMFLOAT3 localPosition;
        XMStoreFloat3(&localPosition, surface);
        XMFLOAT3 worldPosition, worldNormal;
        ResolveAATurretCharge(turret, box.part, localPosition, localNormal,
                              worldPosition, worldNormal);
        result = { box.part, worldPosition, worldNormal,
                   localPosition, localNormal, enter };
        found = true;
    }
    return found;
}

} // namespace SGE

#endif
