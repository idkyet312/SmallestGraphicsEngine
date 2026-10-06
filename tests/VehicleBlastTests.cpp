#include "VehicleBlast.h"

#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using namespace DirectX;
    for (const float maxHealth : {900.0f, 1500.0f, 2400.0f}) {
        float health = maxHealth;
        health -= SGE::TankC4BlastDamage(0.0f, 5.4f, maxHealth);
        Check(health > 0.0f && health == maxHealth * 0.5f,
              "one attached C4 must leave a full-health tank alive at half HP");
        health -= SGE::TankC4BlastDamage(0.0f, 5.4f, maxHealth);
        Check(health <= 0.0f,
              "the second attached C4 must destroy the tank for any authored HP");
        Check(SGE::TankC4BlastDamage(1.49f, 5.4f, maxHealth) == maxHealth * 0.5f,
              "a charge close to the hull must retain direct-hit damage");
        const float splash = SGE::TankC4BlastDamage(3.0f, 5.4f, maxHealth);
        Check(splash > 0.0f && splash < maxHealth * 0.5f,
              "nearby C4 splash must be weaker than an attached charge");
        Check(SGE::TankC4BlastDamage(5.4f, 5.4f, maxHealth) == 0.0f &&
              SGE::TankC4BlastDamage(7.0f, 5.4f, maxHealth) == 0.0f &&
              SGE::TankC4BlastDamage(0.0f, 0.0f, maxHealth) == 0.0f,
              "C4 outside its blast radius or with no radius must not hurt tanks");
    }
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
