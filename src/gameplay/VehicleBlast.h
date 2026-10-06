#pragma once

#include "VehicleSystem.h"

namespace SGE {

inline float TankC4BlastDamage(float surfaceDistance, float radius,
                               float maxHealth) {
    if (radius <= 0.0f || surfaceDistance >= radius) return 0.0f;
    // Two attached charges destroy any authored hull, independent of its HP.
    const float directDamage = maxHealth * 0.5f;
    return surfaceDistance < 1.5f ? directDamage
        : directDamage * 0.6f * (1.0f - surfaceDistance / radius);
}

inline float AATurretBlastDamage(float distance, float radius, float fragDamage,
                                bool c4, bool rocket, bool missile, bool hostile) {
    const float reach = radius + VehicleSystem::AATurretMountHeight;
    if (distance >= reach) return 0.0f;
    const bool demolition = c4 || ((rocket || missile) && !hostile);
    // Distance is measured to the base, so contact anywhere on the gun must
    // still count as a direct demolition hit.
    if (demolition && distance < VehicleSystem::AATurretMountHeight +
            VehicleSystem::AATurretBarrelLength + 0.5f)
        return VehicleSystem::AATurretMaxHealth;
    return (demolition ? VehicleSystem::AATurretMaxHealth : fragDamage) *
        (1.0f - distance / reach);
}

inline bool MissileBlastHitsHumvee(const DirectX::XMFLOAT3& center, float radius,
                                   const DirectX::XMFLOAT4X4& chassisPose) {
    if (radius <= 0.0f) return false;
    using namespace DirectX;
    XMFLOAT3 local;
    XMStoreFloat3(&local, XMVector3TransformCoord(XMLoadFloat3(&center),
        XMMatrixInverse(nullptr, XMLoadFloat4x4(&chassisPose))));
    // Match the chassis bounds used by the gameplay collision queries. A
    // terrain impact beside the hull must not miss just because its centre
    // sits above the ground or the vehicle is turned across the blast.
    const float dx = (std::max)(0.0f, std::abs(local.x) - 2.25f);
    const float dy = (std::max)(0.0f, std::abs(local.y) - 0.7f);
    const float dz = (std::max)(0.0f, std::abs(local.z) - 1.05f);
    return dx * dx + dy * dy + dz * dz < radius * radius;
}

} // namespace SGE
