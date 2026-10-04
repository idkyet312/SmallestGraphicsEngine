#include "VehicleBlast.h"

#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using namespace DirectX;
    VehicleSystem vehicles;
    const size_t target = vehicles.PlaceAATurret({ 0.0f, 0.0f, 0.0f });
    const float radius = 14.0f; // Default deploy-screen missile kill radius.
    const auto damage = [&](float distance, bool c4, bool rocket, bool missile,
                            bool hostile = false) {
        return SGE::AATurretBlastDamage(distance, radius, 500.0f,
                                       c4, rocket, missile, hostile);
    };
    Check(vehicles.DamageAATurret(target,
              damage(3.0f, false, false, true)).destroyed,
          "a deploy missile beside the mount must destroy a full-health gun");
    Check(damage(VehicleSystem::AATurretMountHeight +
                     VehicleSystem::AATurretBarrelLength,
                 false, false, true) == VehicleSystem::AATurretMaxHealth,
          "a missile striking the raised barrel must destroy the gun");
    Check(damage(3.0f, false, false, false) < VehicleSystem::AATurretMaxHealth,
          "frag blasts must retain their damage");
    Check(damage(3.0f, false, true, false) == VehicleSystem::AATurretMaxHealth &&
          damage(3.0f, true, false, false) == VehicleSystem::AATurretMaxHealth,
          "rockets and C4 must retain their demolition damage");
    Check(damage(3.0f, false, true, false, true) < VehicleSystem::AATurretMaxHealth,
          "hostile shells must not gain friendly demolition damage");
    Check(damage(10.0f, false, false, true) > 0.0f &&
          damage(10.0f, false, false, true) < VehicleSystem::AATurretMaxHealth,
          "distant missile splash must fall off");
    Check(damage(16.0f, false, false, true) == 0.0f,
          "missiles outside the blast must not damage a turret");

    XMFLOAT4X4 pose;
    XMStoreFloat4x4(&pose, XMMatrixTranslation(10.0f, 1.0f, 20.0f));
    Check(SGE::MissileBlastHitsHumvee({ 10.0f, 0.0f, 20.0f }, radius, pose),
          "a terrain impact underneath the Humvee must destroy it");
    Check(SGE::MissileBlastHitsHumvee({ 14.0f, 1.0f, 20.0f }, 2.0f, pose),
          "blast overlap with the hull must count even outside centre reach");
    Check(!SGE::MissileBlastHitsHumvee({ 15.0f, 1.0f, 20.0f }, 2.0f, pose),
          "a near miss outside the hull's blast reach must leave it intact");
    XMStoreFloat4x4(&pose, XMMatrixRotationY(XM_PIDIV2) *
                          XMMatrixTranslation(10.0f, 1.0f, 20.0f));
    Check(SGE::MissileBlastHitsHumvee({ 10.0f, 1.0f, 24.0f }, 2.0f, pose) &&
          !SGE::MissileBlastHitsHumvee({ 14.0f, 1.0f, 20.0f }, 2.0f, pose),
          "blast overlap must follow the Humvee's orientation");
    Check(!SGE::MissileBlastHitsHumvee({ 10.0f, 1.0f, 20.0f }, 0.0f, pose),
          "a disabled blast radius must not destroy the Humvee");
    return 0;
}
